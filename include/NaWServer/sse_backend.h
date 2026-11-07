// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/detail/sse/handoff_types.hpp>
#include <memory>
#include <NaWServer/write_buffer.h>

namespace NaWServer
{
	class Request;
	namespace sse
	{
		struct AdapterStats
		{
			std::size_t active = 0, queued_bytes = 0, completed = 0, sent_frames = 0, begun = 0, rolled_back = 0;
			std::array<std::size_t, 8> closed_by_reason{};
			MemoryPoolStats memory;
		};
		class Backend
		{
		public:
			class Reservation
			{
				friend class Backend;
				Backend *owner_ = nullptr;
				Token token_;
				AbortHandle abort_;
			public:
				LastEventId last_id;
				Reservation() = default;
				~Reservation();
				Reservation(const Reservation &) = delete;
				Reservation &operator=(const Reservation &) = delete;
				Token token() const
				{
					return token_;
				}

				AbortHandle abort_handle() const
				{
					return abort_;
				}

			};
			virtual ~Backend() = default;
			virtual bool start() = 0;
			virtual bool stop(std::uint32_t timeout_ms) = 0;
			virtual Admit reserve(Request &, const Frame &, Reservation &) = 0;
			virtual bool activate(Reservation &) = 0;
			virtual void rollback(Reservation &) = 0;
			virtual Publish publish(Token, const Frame &) = 0;
			virtual bool close(Token) = 0;
			virtual AdapterStats stats() = 0;
			virtual std::size_t close_errors() const = 0;
			std::size_t active()
			{
				return stats().active;
			}

			std::size_t completions()
			{
				return stats().completed;
			}

		protected:
			bool owns(const Reservation &value) const
			{
				return value.owner_ == this;
			}

			bool available(const Reservation &value) const
			{
				return !value.owner_;
			}

			void bind(Reservation &value, Token token, AbortHandle abort)
			{
				value.owner_ = this; value.token_ = token; value.abort_ = abort;
			}

			void release(Reservation &value)
			{
				value.owner_ = nullptr;
			}

			void reset(Reservation &value)
			{
				value.token_ = {}; value.abort_ = {}; value.last_id = {};
			}

		};
		// Owned connection, never a retained Request/Response/Next. A reserved stream
		// must restore callback ownership on rollback; activate cannot allocate/fail.
		class Stream
		{
		public:
			struct WriteStatus { std::size_t bytes = 0; bool failed = false; };
			virtual ~Stream() = default;
			virtual void activate() = 0;
			virtual void rollback() = 0;
			virtual int write(const char *bytes, std::size_t size) = 0; // -1 fatal, 0 would-block
			// Optional shared transmission path. Filling leases may be reserved
			// before activation, but publication waits for callback ownership handoff.
			virtual bool supportsWriteLease() const { return false; }
			virtual WriteBuffer acquireWrite(std::size_t) { return {}; }
			virtual bool readyForWrite() const { return false; }
			virtual WriteStatus writeStatus() const { return {}; }
			virtual void finish() = 0;
			virtual bool isOpen() const = 0;
		};
		inline constexpr char exchange_key = 0;
		class Exchange
		{
		public:
			virtual ~Exchange() = default;
			// One serialized HTTP backend per SSE service; stable identity through stop.
			virtual const void *owner() const = 0;
			virtual std::shared_ptr<Stream> retain() = 0;
		};
	}
}
