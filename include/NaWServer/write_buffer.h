// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>

namespace NaWServer
{
	struct WritePolicy
	{
		std::uint8_t priority = 0;
		std::uint64_t expires_at_ms = 0;
		// Only complete, not-yet-started application units may opt into discard.
		// Ordinary HTTP/WS/SSE writes never enable this implicitly.
		bool discard_expired = false;
		// Nonzero explicitly permits replacement of an unstarted matching unit.
		std::uint64_t replacement_key = 0;
	};

	// Exclusive producer ownership. The queue does not read filling storage.
	// A lease must be returned before the transport/runtime is destroyed.
	class WriteBuffer
	{
		void *context_ = nullptr;
		std::size_t slot_ = 0;
		std::uint64_t serial_ = 0;
		std::uint8_t *data_ = nullptr;
		std::size_t capacity_ = 0;
		WritePolicy policy_;
		bool (*commit_)(void *, std::size_t, std::uint64_t, std::size_t, WritePolicy) = nullptr;
		void (*cancel_)(void *, std::size_t, std::uint64_t) = nullptr;
	public:
		WriteBuffer() = default;
		WriteBuffer(void *context, std::size_t slot, std::uint64_t serial, std::uint8_t *data,
		    std::size_t capacity,
		    bool (*commit)(void *, std::size_t, std::uint64_t, std::size_t, WritePolicy),
		    void (*cancel)(void *, std::size_t, std::uint64_t), WritePolicy policy = {})
		  : context_(context), slot_(slot), serial_(serial), data_(data), capacity_(capacity),
		    policy_(policy), commit_(commit), cancel_(cancel)
		{
		}

		WriteBuffer(const WriteBuffer &) = delete;
		WriteBuffer &operator=(const WriteBuffer &) = delete;
		WriteBuffer(WriteBuffer &&other) noexcept { *this = std::move(other); }
		WriteBuffer &operator=(WriteBuffer &&other) noexcept
		{
			if(this != &other)
			{
				reset();
				context_ = std::exchange(other.context_, nullptr);
				slot_ = other.slot_; serial_ = other.serial_; data_ = other.data_;
				capacity_ = other.capacity_; commit_ = other.commit_; cancel_ = other.cancel_;
				policy_ = other.policy_;
			}
			return *this;
		}

		~WriteBuffer() { reset(); }
		explicit operator bool() const { return context_ != nullptr; }
		std::uint8_t *data() const { return context_ ? data_ : nullptr; }
		std::size_t capacity() const { return context_ ? capacity_ : 0; }
		bool commit(std::size_t size) { return commit(size, policy_); }
		bool commit(std::size_t size, WritePolicy policy)
		{
			if(!context_ || size > capacity_ || !commit_(context_, slot_, serial_, size, policy)) return false;

			context_ = nullptr;
			return true;
		}

		void reset()
		{
			if(context_) cancel_(context_, slot_, serial_);

			context_ = nullptr;
		}
	};
}
