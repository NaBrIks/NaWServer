// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/request_body.h>
#include <NaWServer/memory_manager.h>
#include <NaWServer/write_buffer.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace NaWServer
{
	struct Header
	{
		const char *name;
		const char *value;
	};
	// Immutable borrowed configuration. Table and strings outlive the server.
	class DefaultHeaders
	{
		const Header *items_{};
		std::size_t size_{};

	public:
		constexpr DefaultHeaders() = default;
		template <std::size_t N>
		constexpr DefaultHeaders(const Header (&items)[N])
		  : items_(items), size_(N)
		{
		}

		constexpr const Header *data() const
		{
			return items_;
		}

		constexpr std::size_t size() const
		{
			return size_;
		}

	};
	enum class ContentType
	{
		Default,
		Text,
		Html,
		Json,
		OctetStream
	};
	enum class CachePolicy
	{
		Default,
		NoStore,
		NoCache,
		Private,
		Public
	};
	enum class ConnectionPolicy
	{
		Default,
		Close
	};
	enum class HeaderFlags : unsigned
	{
		None = 0,
		NoSniff = 1
	};
	constexpr HeaderFlags operator|(HeaderFlags a, HeaderFlags b)
	{
		return static_cast<HeaderFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
	}

	struct ResponseOptions
	{
		ContentType content_type;
		CachePolicy cache;
		HeaderFlags flags;
		ConnectionPolicy connection;
		constexpr ResponseOptions(
			ContentType type = ContentType::Default,
			CachePolicy policy = CachePolicy::Default,
			HeaderFlags header_flags = HeaderFlags::None,
			ConnectionPolicy connection_policy = ConnectionPolicy::Default)
			: content_type(type), cache(policy), flags(header_flags), connection(connection_policy)
		{
		}
	};
	enum class SendResult
	{
		Ok,
		AlreadySent,
		InvalidArgument,
		HeaderLimit,
		BufferTooSmall,
		TransportError
	};
	enum class ResponseState
	{
		Preparing,
		Sending,
		Complete,
		Failed,
		External,
		Dropped
	};
	struct ExternalControl
	{
		void *context{};
		void (*abort)(void *)
		{
		};
	};
	template <std::size_t N> class TextBuffer;
	// Borrowed through sendSource(). A reader fills the supplied destination and
	// returns Data with 1..capacity bytes; premature End/error aborts the response.
	struct ResponseSource
	{
		void *context = nullptr;
		std::size_t size = 0;
		BodyReadResult (*read)(void *, std::size_t, std::uint8_t *, std::size_t) = nullptr;
	};
	// Explicit ownership for bounded, incremental download execution after the
	// endpoint returns. release closes the source on success, failure or Drop.
	// read must be a short bounded operation; no Request/Response is retained.
	class OwnedResponseSource
	{
		ResponseSource source_;
		void (*release_)(void *) = nullptr;
	public:
		OwnedResponseSource() = default;
		OwnedResponseSource(ResponseSource source, void (*release)(void *)) : source_(source), release_(release) {}
		OwnedResponseSource(const OwnedResponseSource &) = delete;
		OwnedResponseSource &operator=(const OwnedResponseSource &) = delete;
		OwnedResponseSource(OwnedResponseSource &&other) noexcept { *this = std::move(other); }
		OwnedResponseSource &operator=(OwnedResponseSource &&other) noexcept
		{
			if(this != &other)
			{
				reset(); source_ = other.source_; release_ = other.release_;
				other.source_ = {}; other.release_ = nullptr;
			}
			return *this;
		}
		~OwnedResponseSource() { reset(); }
		explicit operator bool() const { return release_ && (!source_.size || source_.read); }
		ResponseSource view() const { return source_; }
		void reset()
		{
			const auto release = release_; release_ = nullptr;
			if(release) release(source_.context);
			source_ = {};
		}
	};
	// send consumes all borrowed arguments before returning. Ok is not peer ACK.
	struct ResponseTransport
	{
		void *context{};
		SendResult (*send)(void *, int, const char *, const Header *, std::size_t, ByteView)
		{
		};
		// Optional explicit immutable-body capability. Only the body may be
		// retained; headers/type are still consumed before return.
		SendResult (*send_immutable)(void *, int, const char *, const Header *, std::size_t, ByteView) = nullptr;
		SendResult (*send_owned)(void *, int, const char *, const Header *, std::size_t, ByteView,
		    MemoryBlock &) = nullptr;
		SendResult (*send_source)(void *, int, const char *, const Header *, std::size_t, ResponseSource) = nullptr;
		SendResult (*begin_stream)(void *, int, const char *, const Header *, std::size_t, std::size_t) = nullptr;
		SendResult (*write_stream)(void *, ByteView) = nullptr;
		SendResult (*finish_stream)(void *) = nullptr;
		// Private adapter notification: direct synchronous streaming bypassed its
		// response capture. No source or Response pointers are retained.
		void (*direct_started)(void *) = nullptr;
		WriteBuffer (*acquire_stream)(void *, std::size_t) = nullptr;
		SendResult (*send_owned_source)(void *, int, const char *, const Header *, std::size_t,
		    OwnedResponseSource &) = nullptr;
	};
	class Response
	{
	public:
		static constexpr std::size_t max_headers = 16;
		static constexpr std::size_t header_storage = 768;

	private:
		ResponseTransport transport_;
		DefaultHeaders defaults_;
		ResponseOptions options_;
		std::array<Header, max_headers> headers_{};
		std::size_t count_{};
		std::array<char, header_storage> storage_{};
		std::size_t used_{};
		ResponseState state_{ResponseState::Preparing};
		SendResult error_{SendResult::Ok};
		ExternalControl external_{};
		std::size_t stream_remaining_ = 0;
		WriteBuffer stream_buffer_;
		static bool commitStream(void *, std::size_t, std::uint64_t, std::size_t, WritePolicy);
		static void cancelStream(void *, std::size_t, std::uint64_t);
		SendResult sendBody(int status, const char *type, ByteView body, bool immutable, MemoryBlock *owned = nullptr);
		SendResult reject(SendResult error);
		const char *mime(const char *fallback) const;

	public:
		// Transfer a manager lease through completion/cancellation. Ordinary/native
		// synchronous transports consume it before returning without retaining it.
		SendResult sendOwned(int status, const char *type, MemoryBlock block, std::size_t used);
		explicit Response(ResponseTransport transport, DefaultHeaders defaults = {}, ResponseOptions options = {})
		  : transport_(transport), defaults_(defaults), options_(options)
		{
		}

		Response(const Response &) = delete;
		Response &operator=(const Response &) = delete;
		Response(Response &&) = delete;
		Response &operator=(Response &&) = delete;
		[[nodiscard]] SendResult header(std::string_view name, std::string_view value);
		[[nodiscard]] SendResult options(ResponseOptions options);
		[[nodiscard]] SendResult send(int status, const char *content_type, ByteView body);
		[[nodiscard]] SendResult sendSource(int status, const char *content_type, ResponseSource source);
		[[nodiscard]] SendResult sendOwnedSource(int status, const char *content_type, OwnedResponseSource source);
		// Bounded synchronous producer API. Each write consumes its borrowed input
		// before returning; the TCP sender may run independently on another worker.
		[[nodiscard]] SendResult beginStream(int status, const char *content_type, std::size_t size);
		[[nodiscard]] SendResult write(ByteView bytes);
		// The lease must be committed/cancelled before finish or destruction of
		// this Response. An uncommitted lease owns memory but is never transmitted.
		[[nodiscard]] WriteBuffer acquire(std::size_t capacity);
		[[nodiscard]] SendResult finish();
		// Body backing is immutable and outlives server.stop(). Native transports
		// may consume it immediately; deferred TCP can send it without a large copy.
		[[nodiscard]] SendResult sendImmutable(int status, const char *content_type, ByteView body);
		[[nodiscard]] SendResult bytes(int status, const void *data, std::size_t size)
		{
			return send(status, mime("application/octet-stream"), {static_cast<const uint8_t *>(data), size});
		}

		[[nodiscard]] SendResult bytes(int status, ByteView body)
		{
			return bytes(status, body.data(), body.size());
		}

		[[nodiscard]] SendResult text(int status, std::string_view body)
		{
			return send(
				status,
				mime("text/plain; charset=utf-8"),
				{reinterpret_cast<const uint8_t *>(body.data()), body.size()});
		}

		template <std::size_t N> [[nodiscard]] SendResult text(int status, const TextBuffer<N> &body)
		{
			return body.valid() ? text(status, body.view()) : reject(SendResult::BufferTooSmall);
		}

		[[nodiscard]] SendResult empty(int status = 204)
		{
			return send(status, mime("text/plain"), {});
		}

		[[nodiscard]] SendResult redirect(std::string_view location, int status = 302);
		ResponseState state() const
		{
			return state_;
		}

		bool started() const
		{
			return state_ != ResponseState::Preparing;
		}

		bool failed() const
		{
			return state_ == ResponseState::Failed || error_ != SendResult::Ok;
		}

		SendResult error() const
		{
			return error_;
		}

		void fail();
		// Terminal cancellation is interpreted by the I/O owner after the callback.
		void drop()
		{
			if(state_ == ResponseState::Dropped) return;
			fail();
			state_ = ResponseState::Dropped;
		}
		bool dropped() const { return state_ == ResponseState::Dropped; }
		// Prepared pointers remain valid only while this Response/configuration lives.
		// An external transport must copy them before returning from the callback.
		[[nodiscard]] SendResult resolveHeaders(std::array<Header, max_headers> &outgoing, std::size_t &count);
		// Reserved for an explicitly authorized transport adapter, not deferred HTTP.
		[[nodiscard]] SendResult claimExternal(ExternalControl control);
	};
	// Optional caller-owned buffered text builder, neither a socket nor async.
	template <std::size_t N> class TextBuffer
	{
		static_assert(N > 0, "TextBuffer includes a trailing NUL slot");
		std::array<char, N> data_{};
		std::size_t size_{};
		bool valid_{true};

	public:
		bool print(std::string_view value)
		{
			if(!valid_ || value.size() > N - 1 - size_)
				return valid_ = false;

			if(!value.empty())
				std::memcpy(data_.data() + size_, value.data(), value.size());

			size_ += value.size();
			data_[size_] = 0;
			return true;
		}

		bool printf(const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
		__attribute__((format(printf, 2, 3)))
#endif
		{
			if(!valid_ || !format)
				return valid_ = false;

			va_list args;
			va_start(args, format);
			int count = std::vsnprintf(data_.data() + size_, N - size_, format, args);
			va_end(args);
			if(count < 0 || static_cast<std::size_t>(count) >= N - size_)
			{
				data_[size_] = 0;
				return valid_ = false;
			}

			size_ += static_cast<std::size_t>(count);
			return true;
		}

		bool print(float value)
		{
			return printf("%f", static_cast<double>(value));
		}

		bool valid() const
		{
			return valid_;
		}

		std::string_view view() const
		{
			return {data_.data(), size_};
		}

	};
} // namespace NaWServer
