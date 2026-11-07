// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

// Synchronous response contract: bounded metadata and borrowed body.
#include <NaWServer/response.h>
#include "detail/ascii.h"
#include "http/header_validation.h"
#include <limits>
#include <algorithm>
namespace NaWServer
{
	namespace
	{
		bool reserved(std::string_view name)
		{
			return detail::asciiEqualIgnoreCase(name, "Content-Type") ||
			       detail::asciiEqualIgnoreCase(name, "Content-Length") ||
			       detail::asciiEqualIgnoreCase(name, "Transfer-Encoding") ||
			       detail::asciiEqualIgnoreCase(name, "Connection");
		}

	} // namespace
	SendResult Response::reject(SendResult error)
	{
		if(error_ == SendResult::Ok)
			error_ = error;

		if(state_ == ResponseState::External && external_.abort)
			external_.abort(external_.context);

		return error;
	}

	SendResult Response::options(ResponseOptions options)
	{
		if(started())
			return reject(SendResult::AlreadySent);

		options_ = options;
		return SendResult::Ok;
	}

	SendResult Response::header(std::string_view name, std::string_view value)
	{
		if(started())
			return reject(SendResult::AlreadySent);

		if(!detail::isValidHeaderName(name) || !detail::isValidHeaderValue(value) || reserved(name))
			return reject(SendResult::InvalidArgument);

		std::size_t index = count_;
		for(std::size_t i = 0; i < count_; ++i)
			if(detail::asciiEqualIgnoreCase(headers_[i].name, name))
			{
				index = i;
				break;
			}

		if(index == max_headers)
			return reject(SendResult::HeaderLimit);

		if(name.size() + value.size() + 2 > storage_.size() - used_)
			return reject(SendResult::BufferTooSmall);

		char *key = storage_.data() + used_;
		std::memcpy(key, name.data(), name.size());
		key[name.size()] = 0;
		char *content = key + name.size() + 1;
		if(!value.empty())
			std::memcpy(content, value.data(), value.size());

		content[value.size()] = 0;
		used_ += name.size() + value.size() + 2;
		headers_[index] = {key, content};
		if(index == count_)
			++count_;

		return SendResult::Ok;
	}

	const char *Response::mime(const char *fallback) const
	{
		switch(options_.content_type)
		{
		case ContentType::Text:
			return "text/plain; charset=utf-8";
		case ContentType::Html:
			return "text/html; charset=utf-8";
		case ContentType::Json:
			return "application/json";
		case ContentType::OctetStream:
			return "application/octet-stream";
		default:
			return fallback;
		}
	}

	SendResult Response::resolveHeaders(std::array<Header, max_headers> &outgoing, std::size_t &total)
	{
		if(started())
			return reject(SendResult::AlreadySent);

		if(error_ != SendResult::Ok)
			return error_;

		total = 0;
		auto put = [&](Header item) -> bool
			{
				if(!item.name || !item.value || !detail::isValidHeaderName(item.name) ||
				    !detail::isValidHeaderValue(item.value) || reserved(item.name))
					return false;

				for(std::size_t i = 0; i < total; ++i)
					if(detail::asciiEqualIgnoreCase(outgoing[i].name, item.name))
					{
						outgoing[i] = item;
						return true;
					}

				if(total == max_headers)
					return false;

				outgoing[total++] = item;
				return true;
			};
		for(std::size_t i = 0; i < defaults_.size(); ++i)
			if(!put(defaults_.data()[i]))
				return reject(SendResult::HeaderLimit);

		const char *cache = nullptr;
		switch(options_.cache)
		{
		case CachePolicy::NoStore:
			cache = "no-store";
			break;
		case CachePolicy::NoCache:
			cache = "no-cache";
			break;
		case CachePolicy::Private:
			cache = "private";
			break;
		case CachePolicy::Public:
			cache = "public";
			break;
		default:
			break;
		}
		if(cache && !put({"Cache-Control", cache}))
			return reject(SendResult::HeaderLimit);

		if((static_cast<unsigned>(options_.flags) & static_cast<unsigned>(HeaderFlags::NoSniff)) &&
		    !put({"X-Content-Type-Options", "nosniff"}))
			return reject(SendResult::HeaderLimit);

		for(std::size_t i = 0; i < count_; ++i)
			if(!put(headers_[i]))
				return reject(SendResult::HeaderLimit);

		if(options_.connection == ConnectionPolicy::Close)
		{
			if(total == max_headers)
				return reject(SendResult::HeaderLimit);

			outgoing[total++] = {"Connection", "close"};
		}

		return SendResult::Ok;
	}

	SendResult Response::send(int status, const char *content_type, ByteView body)
	{
		return sendBody(status, content_type, body, false);
	}

	SendResult Response::sendImmutable(int status, const char *content_type, ByteView body)
	{
		return sendBody(status, content_type, body, true);
	}

	SendResult Response::beginStream(int status, const char *type, std::size_t size)
	{
		if(started()) return reject(SendResult::AlreadySent);

		if(!transport_.begin_stream || !transport_.write_stream || !transport_.finish_stream ||
		    status < 200 || status > 599 || !type || !*type || !detail::isValidHeaderValue(type) ||
		    ((status == 204 || status == 304) && size) ||
		    size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
			return reject(SendResult::InvalidArgument);

		std::array<Header, max_headers> outgoing{};
		std::size_t count = 0;
		const auto prepared = resolveHeaders(outgoing, count);
		if(prepared != SendResult::Ok) return prepared;

		state_ = ResponseState::Sending;
		const auto result = transport_.begin_stream(transport_.context, status, type, outgoing.data(), count, size);
		if(result != SendResult::Ok) { state_ = ResponseState::Failed; return reject(result); }

		stream_remaining_ = size;
		return SendResult::Ok;
	}

	SendResult Response::write(ByteView bytes)
	{
		if(stream_buffer_) return reject(SendResult::InvalidArgument);

		if(state_ != ResponseState::Sending || !transport_.write_stream)
			return reject(SendResult::AlreadySent);

		if(bytes.size() > stream_remaining_ || (bytes.size() && !bytes.data()))
		{
			fail();
			return reject(SendResult::InvalidArgument);
		}
		const auto result = bytes.empty() ? SendResult::Ok : transport_.write_stream(transport_.context, bytes);
		if(result != SendResult::Ok) { fail(); return reject(result); }

		stream_remaining_ -= bytes.size();
		return SendResult::Ok;
	}

	SendResult Response::finish()
	{
		if(state_ != ResponseState::Sending || !transport_.finish_stream)
			return reject(SendResult::AlreadySent);

		if(stream_remaining_ || stream_buffer_) { fail(); return reject(SendResult::InvalidArgument); }

		const auto result = transport_.finish_stream(transport_.context);
		state_ = result == SendResult::Ok ? ResponseState::Complete : ResponseState::Failed;
		return result == SendResult::Ok ? result : reject(result);
	}

	WriteBuffer Response::acquire(std::size_t capacity)
	{
		if(state_ != ResponseState::Sending || !transport_.acquire_stream || stream_buffer_ ||
		    !capacity || !stream_remaining_) return {};

		capacity = std::min(capacity, stream_remaining_);
		stream_buffer_ = transport_.acquire_stream(transport_.context, capacity);
		if(!stream_buffer_) return {};

		return {this, 0, 0, stream_buffer_.data(), std::min(capacity, stream_buffer_.capacity()),
		    commitStream, cancelStream};
	}

	bool Response::commitStream(void *context, std::size_t, std::uint64_t, std::size_t size, WritePolicy policy)
	{
		auto &response = *static_cast<Response *>(context);
		// A body block is only part of an HTTP response. Dropping it while keeping
		// the connection open would corrupt framing and possibly the next response.
		if(policy.discard_expired || policy.replacement_key || response.state_ != ResponseState::Sending ||
		    size > response.stream_remaining_ || !response.stream_buffer_.commit(size, policy)) return false;

		response.stream_remaining_ -= size;
		return true;
	}

	void Response::cancelStream(void *context, std::size_t, std::uint64_t)
	{
		static_cast<Response *>(context)->stream_buffer_.reset();
	}

	SendResult Response::sendSource(int status, const char *type, ResponseSource source)
	{
		if(started()) return reject(SendResult::AlreadySent);

		if(!transport_.send_source || (source.size && !source.read) || status < 200 || status > 599 ||
		    !type || !*type || !detail::isValidHeaderValue(type) ||
		    ((status == 204 || status == 304) && source.size) ||
		    source.size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
			return reject(SendResult::InvalidArgument);

		std::array<Header, max_headers> outgoing{};
		std::size_t count = 0;
		const auto prepared = resolveHeaders(outgoing, count);
		if(prepared != SendResult::Ok) return prepared;

		state_ = ResponseState::Sending;
		const auto result = transport_.send_source(transport_.context, status, type, outgoing.data(), count, source);
		state_ = result == SendResult::Ok ? ResponseState::Complete : ResponseState::Failed;
		return result == SendResult::Ok ? result : reject(result);
	}

	SendResult Response::sendOwnedSource(int status, const char *type, OwnedResponseSource source)
	{
		if(started()) return reject(SendResult::AlreadySent);
		const auto view = source.view();
		if(!source || !transport_.send_owned_source || status < 200 || status > 599 ||
		    !type || !*type || !detail::isValidHeaderValue(type) ||
		    ((status == 204 || status == 304) && view.size) ||
		    view.size > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
			return reject(SendResult::InvalidArgument);
		std::array<Header, max_headers> outgoing{};
		std::size_t count = 0;
		const auto prepared = resolveHeaders(outgoing, count);
		if(prepared != SendResult::Ok) return prepared;
		state_ = ResponseState::Sending;
		const auto result = transport_.send_owned_source(transport_.context, status, type, outgoing.data(), count, source);
		state_ = result == SendResult::Ok ? ResponseState::Complete : ResponseState::Failed;
		return result == SendResult::Ok ? result : reject(result);
	}

	SendResult Response::sendOwned(int status, const char *type, MemoryBlock block, std::size_t used)
	{
		if(used > block.size()) return reject(SendResult::InvalidArgument);

		return sendBody(status, type, {block.data(), used}, false, &block);
	}

	SendResult Response::sendBody(int status, const char *content_type, ByteView body, bool immutable,
	    MemoryBlock *owned)
	{
		if(started())
			return reject(SendResult::AlreadySent);

		if(error_ != SendResult::Ok)
			return error_;

		if(status < 200 || status > 599 || !content_type || !*content_type ||
		    !detail::isValidHeaderValue(content_type) || (body.size() && !body.data()) ||
		    body.size() > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
		    ((status == 204 || status == 304) && body.size()))
			return reject(SendResult::InvalidArgument);

		std::array<Header, max_headers> outgoing{};
		std::size_t total = 0;
		const auto prepared = resolveHeaders(outgoing, total);
		if(prepared != SendResult::Ok)
			return prepared;

		state_ = ResponseState::Sending;
		const auto sender = immutable && transport_.send_immutable ? transport_.send_immutable : transport_.send;
		const auto result = owned && transport_.send_owned ?
		    transport_.send_owned(transport_.context, status, content_type, outgoing.data(), total, body, *owned) :
		    (sender ? sender(transport_.context, status, content_type, outgoing.data(), total,
		    body) : SendResult::TransportError);
		if(result == SendResult::Ok)
			state_ = ResponseState::Complete;
		else
		{
			state_ = ResponseState::Failed;
			error_ = result;
		}

		return result;
	}

	SendResult Response::redirect(std::string_view location, int status)
	{
		if(status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
			return reject(SendResult::InvalidArgument);

		const auto result = header("Location", location);
		return result == SendResult::Ok ? empty(status) : result;
	}

	void Response::fail()
	{
		if(dropped()) return;
		if(state_ == ResponseState::External)
		{
			(void)reject(SendResult::TransportError);
			return;
		}

		state_ = ResponseState::Failed;
		error_ = SendResult::TransportError;
	}

	SendResult Response::claimExternal(ExternalControl control)
	{
		if(started())
			return reject(SendResult::AlreadySent);

		if(error_ != SendResult::Ok)
			return error_;

		if(!control.context || !control.abort)
			return reject(SendResult::InvalidArgument);

		external_ = control;
		state_ = ResponseState::External;
		return SendResult::Ok;
	}

} // namespace NaWServer
