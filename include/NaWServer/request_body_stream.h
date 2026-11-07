// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once

#include <NaWServer/request_body.h>
#include <cstddef>
#include <utility>

#ifndef NAWSERVER_HTTP_BODY_BLOCK_BYTES
#define NAWSERVER_HTTP_BODY_BLOCK_BYTES 1024
#endif

namespace NaWServer
{

	class HttpExchange;
	enum class BodyMode { None, Buffered, Stream };
	enum class BodyConsumeResult { Accepted, Pause, Error };

	// A bounded application-owned receiver. Accepted consumes the entire offered
	// span; Pause consumes none and the same bytes will be offered again. The span
	// is borrowed for the callback only. No socket or request object is retained.
	class BodyReceiver
	{
		void *context_ = nullptr;
		BodyConsumeResult (*consume_)(void *, ByteView) = nullptr;
		bool (*finish_)(void *) = nullptr;
		void (*cancel_)(void *) = nullptr;
		void (*release_)(void *) = nullptr;
		bool finished_ = false;
	public:
		BodyReceiver() = default;
		BodyReceiver(void *context, BodyConsumeResult (*consume)(void *, ByteView),
		    bool (*finish)(void *), void (*cancel)(void *), void (*release)(void *) = nullptr)
		  : context_(context), consume_(consume), finish_(finish), cancel_(cancel), release_(release)
		{
		}

		BodyReceiver(const BodyReceiver &) = delete;
		BodyReceiver &operator=(const BodyReceiver &) = delete;
		BodyReceiver(BodyReceiver &&other) noexcept { swap(other); }
		BodyReceiver &operator=(BodyReceiver &&other) noexcept
		{
			if(this != &other) { reset(); swap(other); }
			return *this;
		}

		~BodyReceiver() { reset(); }
		explicit operator bool() const { return consume_ && finish_ && cancel_; }
		BodyConsumeResult consume(ByteView bytes)
		{
			return *this && !finished_ ? consume_(context_, bytes) : BodyConsumeResult::Error;
		}

		bool finish()
		{
			if(!*this || finished_ || !finish_(context_)) return false;
			finished_ = true;
			return true;
		}

		void reset()
		{
			if(consume_)
			{
				if(!finished_ && cancel_) cancel_(context_);
				if(release_) release_(context_);
			}
			context_ = nullptr; consume_ = nullptr; finish_ = nullptr;
			cancel_ = release_ = nullptr; finished_ = false;
		}

	private:
		void swap(BodyReceiver &other) noexcept
		{
			using std::swap;
			swap(context_, other.context_); swap(consume_, other.consume_);
			swap(finish_, other.finish_); swap(cancel_, other.cancel_);
			swap(release_, other.release_); swap(finished_, other.finished_);
		}
	};

	struct BodyOptions
	{
		BodyMode mode = BodyMode::Buffered;
		// Zero selects the server's body limit. Stream routes may explicitly allow
		// larger uploads without enlarging their working block.
		std::size_t max_bytes = 0;
		std::size_t block_bytes = NAWSERVER_HTTP_BODY_BLOCK_BYTES;
		void *context = nullptr;
		BodyReceiver (*begin)(void *, HttpExchange &) = nullptr;
	};

} // namespace NaWServer
