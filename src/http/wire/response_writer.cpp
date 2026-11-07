// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "response_writer.h"
#include "detail/ascii.h"
#include "http/header_validation.h"
#include <charconv>
#include <cstring>
#include <limits>

namespace NaWServer::http::wire
{

	namespace
	{

		ByteView view(std::string_view bytes)
		{
			return {reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()};
		}

		bool connectionHeader(const char *name)
		{
			return detail::asciiEqualIgnoreCase(name, "Connection");
		}

		bool reservedHeader(const char *name)
		{
			return detail::asciiEqualIgnoreCase(name, "Content-Length") ||
			       detail::asciiEqualIgnoreCase(name, "Content-Type") ||
			       detail::asciiEqualIgnoreCase(name, "Transfer-Encoding");
		}

	} // namespace

	bool ResponseWriter::appendPrefix(std::string_view bytes)
	{
		if(bytes.size() > prefix_.size() - prefix_size_) return false;

		std::memcpy(prefix_.data() + prefix_size_, bytes.data(), bytes.size());
		prefix_size_ += bytes.size();
		return true;
	}

	bool ResponseWriter::appendNumber(std::size_t number)
	{
		const auto result = std::to_chars(prefix_.data() + prefix_size_, prefix_.data() + prefix_.size(), number);
		if(result.ec != std::errc{}) return false;

		prefix_size_ = static_cast<std::size_t>(result.ptr - prefix_.data());
		return true;
	}

	SendResult ResponseWriter::prepare(HttpMethod method, int status, const char *content_type,
	    const Header *headers, std::size_t count, ByteView body, bool keep_alive)
	{
		if(body.size() && !body.data()) return SendResult::InvalidArgument;

		return prepareImpl(method, status, content_type, headers, count, body, body.size(), keep_alive);
	}

	SendResult ResponseWriter::prepareHead(HttpMethod method, int status, const char *type,
	    const Header *headers, std::size_t count, std::size_t length, bool keep_alive)
	{
		return prepareImpl(method, status, type, headers, count, {}, length, keep_alive);
	}

	SendResult ResponseWriter::prepareImpl(HttpMethod method, int status, const char *content_type,
	    const Header *headers, std::size_t count, ByteView body, std::size_t length, bool keep_alive)
	{
		if(!complete()) return SendResult::AlreadySent;

		reset();
		if(status < 200 || status > 599 || !content_type || !*content_type ||
		    !detail::isValidHeaderValue(content_type) || (body.size() && !body.data()) ||
		    length > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
		    ((status == 204 || status == 304) && length) || (count && !headers))
			return SendResult::InvalidArgument;

		if(count > Response::max_headers) return SendResult::HeaderLimit;

		for(std::size_t index = 0; index < count; ++index)
		{
			const auto &header = headers[index];
			if(!header.name || !header.value || !detail::isValidHeaderName(header.name) ||
			    !detail::isValidHeaderValue(header.value) || reservedHeader(header.name) ||
			    (connectionHeader(header.name) && !detail::asciiEqualIgnoreCase(header.value, "close")))
				return SendResult::InvalidArgument;

			for(std::size_t previous = 0; previous < index; ++previous)
			{
				if(detail::asciiEqualIgnoreCase(headers[previous].name, header.name))
					return SendResult::InvalidArgument;
			}
		}

		const bool no_body = status == 204 || status == 304;
		if(!appendPrefix("HTTP/1.1 ") || !appendNumber(static_cast<std::size_t>(status)) ||
		    !appendPrefix(keep_alive ? " Response\r\nConnection: keep-alive\r\n" :
		    " Response\r\nConnection: close\r\n") ||
		    (!no_body && (!appendPrefix("Content-Length: ") || !appendNumber(length) || !appendPrefix("\r\n"))) ||
		    !appendPrefix("Content-Type: "))
		{
			reset();
			return SendResult::BufferTooSmall;
		}

		content_type_ = content_type;
		headers_ = headers;
		header_count_ = count;
		body_ = no_body || method == HttpMethod::Head ? ByteView{} : body;
		phase_ = Phase::Prefix;
		span_ = view({prefix_.data(), prefix_size_});
		return SendResult::Ok;
	}

	ByteView ResponseWriter::current() const
	{
		if(span_.empty()) return {};

		return {span_.data() + offset_, span_.size() - offset_};
	}

	bool ResponseWriter::advance(std::size_t bytes)
	{
		if(!bytes || span_.empty() || bytes > span_.size() - offset_) return false;

		offset_ += bytes;
		if(offset_ == span_.size()) nextSpan();

		return true;
	}

	bool ResponseWriter::complete() const
	{
		return phase_ == Phase::Complete;
	}

	void ResponseWriter::reset()
	{
		prefix_size_ = 0;
		content_type_ = nullptr;
		headers_ = nullptr;
		header_count_ = 0;
		header_index_ = 0;
		body_ = {};
		span_ = {};
		offset_ = 0;
		phase_ = Phase::Complete;
	}

	void ResponseWriter::selectHeader()
	{
		while(header_index_ < header_count_ && connectionHeader(headers_[header_index_].name))
			++header_index_;

		if(header_index_ == header_count_)
		{
			phase_ = Phase::HeadersEnd;
			span_ = view("\r\n");
		}
		else
		{
			phase_ = Phase::HeaderName;
			span_ = view(headers_[header_index_].name);
		}
	}

	void ResponseWriter::nextSpan()
	{
		offset_ = 0;
		switch(phase_)
		{
		case Phase::Prefix:
			phase_ = Phase::ContentType;
			span_ = view(content_type_);
			break;
		case Phase::ContentType:
			phase_ = Phase::TypeLineEnd;
			span_ = view("\r\n");
			break;
		case Phase::TypeLineEnd:
			selectHeader();
			break;
		case Phase::HeaderName:
			phase_ = Phase::HeaderSeparator;
			span_ = view(": ");
			break;
		case Phase::HeaderSeparator:
			phase_ = Phase::HeaderValue;
			span_ = view(headers_[header_index_].value);
			if(span_.empty()) nextSpan();

			break;
		case Phase::HeaderValue:
			phase_ = Phase::HeaderLineEnd;
			span_ = view("\r\n");
			break;
		case Phase::HeaderLineEnd:
			++header_index_;
			selectHeader();
			break;
		case Phase::HeadersEnd:
			phase_ = Phase::Body;
			span_ = body_;
			if(span_.empty()) reset();

			break;
		case Phase::Body:
			reset();
			break;
		case Phase::Complete:
			break;
		}
	}

} // namespace NaWServer::http::wire
