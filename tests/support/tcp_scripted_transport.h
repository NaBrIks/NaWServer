// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace NaWServer::tcp::test
{

	enum class Operation { OpenListener, Accept, Read, Write, Wait, ShutdownWrite, Close };

	// Test input bytes and expected wait entries are borrowed until step consumption.
	// Output is fixture-owned; no caller write pointer survives writeSome().
	struct Step
	{
		Operation operation = Operation::Read;
		Status status = Status::Ok;
		IoStatus io_status = IoStatus::Progress;
		WaitStatus wait_status = WaitStatus::Ready;
		Handle handle;
		Handle expected_handle;
		const Interest *expected_interests = nullptr;
		std::size_t expected_interest_count = 0;
		std::uint16_t port = 0;
		const std::uint8_t *bytes = nullptr;
		std::size_t size = 0;
		std::size_t maximum_chunk = std::numeric_limits<std::size_t>::max();
		std::array<Ready, 8> readiness{};
		std::uint32_t elapsed_ms = 0;
		int native_error = 0;
		bool released = true;
		std::optional<CloseResult> cleanup{};
	};

	// Deterministic single-thread fixture, not a platform concurrency implementation.
	// A strict operation sequence catches unexpected retries, extra close, and I/O
	// after handoff. A valid expected_handle additionally checks the intended owner.
	class ScriptedTransport final : public Transport
	{
		struct OwnedHandle
		{
			Handle handle;
			bool listener = false;
			bool write_shutdown = false;
		};

		std::array<Step, 128> steps_{};
		std::array<OwnedHandle, 8> handles_{};
		std::array<std::uint8_t, 16384> capture_{};
		std::array<std::size_t, 7> calls_{};
		std::size_t step_count_ = 0;
		std::size_t next_step_ = 0;
		std::size_t step_offset_ = 0;
		std::size_t capture_size_ = 0;
		std::size_t failures_ = 0;
		std::size_t released_count_ = 0;
		std::uint64_t milliseconds_ = 0;

		OwnedHandle *find(Handle handle)
		{
			for(auto &owned : handles_)
			{
				if(owned.handle.valid && owned.handle.value == handle.value) return &owned;
			}
			return nullptr;
		}

		bool own(Handle handle, bool listener)
		{
			if(!handle.valid || find(handle)) return false;

			for(auto &owned : handles_)
			{
				if(!owned.handle.valid)
				{
					owned = {handle, listener, false};
					return true;
				}
			}
			return false;
		}

		Step *next(Operation operation, Handle handle = {})
		{
			++calls_[static_cast<std::size_t>(operation)];
			if(next_step_ == step_count_ || steps_[next_step_].operation != operation ||
			    (steps_[next_step_].expected_handle.valid &&
			    (!handle.valid || steps_[next_step_].expected_handle.value != handle.value)))
			{
				++failures_;
				return nullptr;
			}

			return &steps_[next_step_];
		}

		void consume()
		{
			++next_step_;
			step_offset_ = 0;
		}

	public:
		static constexpr int script_error = -7001;
		static constexpr int capacity_error = -7002;

		explicit ScriptedTransport(std::uint64_t initial_time = 0)
		  : milliseconds_(initial_time)
		{
		}

		bool append(const Step &step)
		{
			if(step_count_ == steps_.size())
			{
				++failures_;
				return false;
			}

			steps_[step_count_++] = step;
			return true;
		}

		std::uint64_t milliseconds() const
		{
			return milliseconds_;
		}

		void advance(std::uint32_t milliseconds)
		{
			const auto available = std::numeric_limits<std::uint64_t>::max() - milliseconds_;
			milliseconds_ += std::min(available, static_cast<std::uint64_t>(milliseconds));
		}

		std::size_t failures() const
		{
			return failures_;
		}

		std::size_t remaining() const
		{
			return step_count_ - next_step_;
		}

		std::size_t calls(Operation operation) const
		{
			return calls_[static_cast<std::size_t>(operation)];
		}

		std::size_t released() const
		{
			return released_count_;
		}

		std::size_t owned() const
		{
			std::size_t count = 0;
			for(const auto &entry : handles_)
			{
				if(entry.handle.valid) ++count;
			}
			return count;
		}

		const std::uint8_t *capturedBytes() const
		{
			return capture_.data();
		}

		std::size_t capturedSize() const
		{
			return capture_size_;
		}

		ListenerResult openListener(const char *address, std::uint16_t, int backlog) override
		{
			if(!address || !*address || backlog <= 0) return {Status::InvalidArgument, 0, {}, 0};

			auto *step = next(Operation::OpenListener);
			if(!step) return {Status::Fatal, script_error, {}, 0};

			ListenerResult result{step->status, step->native_error, {}, 0, step->cleanup};
			if(result.status == Status::Ok || step->handle.valid)
			{
				if(!own(step->handle, true))
				{
					++failures_;
					result = {Status::Fatal, capacity_error, {}, 0};
				}
				else
				{
					result.handle = step->handle;
					result.port = step->port;
				}
			}

			consume();
			return result;
		}

		AcceptResult accept(Handle listener) override
		{
			if(!listener.valid) return {Status::InvalidArgument, 0, {}};

			const auto *owned = find(listener);
			if(!owned || !owned->listener) return {Status::InvalidState, 0, {}};

			auto *step = next(Operation::Accept, listener);
			if(!step) return {Status::Fatal, script_error, {}};

			AcceptResult result{step->status, step->native_error, {}, step->cleanup};
			if(result.status == Status::Ok || step->handle.valid)
			{
				if(!own(step->handle, false))
				{
					++failures_;
					result = {Status::Fatal, capacity_error, {}};
				}
				else
				{
					result.handle = step->handle;
				}
			}

			consume();
			return result;
		}

		IoResult readSome(Handle handle, std::uint8_t *destination, std::size_t capacity) override
		{
			if(!handle.valid || !destination || !capacity) return {IoStatus::InvalidArgument};

			const auto *owned = find(handle);
			if(!owned || owned->listener) return {IoStatus::InvalidState};

			auto *step = next(Operation::Read, handle);
			if(!step) return {IoStatus::Fatal, 0, script_error};

			if(step->io_status != IoStatus::Progress)
			{
				const IoResult result{step->io_status, 0, step->native_error};
				consume();
				return result;
			}

			if(!step->bytes || step_offset_ >= step->size || !step->maximum_chunk)
			{
				++failures_;
				consume();
				return {IoStatus::Fatal, 0, script_error};
			}

			const auto size = std::min({capacity, step->size - step_offset_, step->maximum_chunk});
			std::memcpy(destination, step->bytes + step_offset_, size);
			step_offset_ += size;
			if(step_offset_ == step->size) consume();

			return {IoStatus::Progress, size};
		}

		IoResult writeSome(Handle handle, const std::uint8_t *bytes, std::size_t size) override
		{
			if(!handle.valid || !bytes || !size) return {IoStatus::InvalidArgument};

			const auto *owned = find(handle);
			if(!owned || owned->listener || owned->write_shutdown) return {IoStatus::InvalidState};

			auto *step = next(Operation::Write, handle);
			if(!step) return {IoStatus::Fatal, 0, script_error};

			if(step->io_status != IoStatus::Progress)
			{
				const IoResult result{step->io_status, 0, step->native_error};
				consume();
				return result;
			}

			if(step_offset_ >= step->size || !step->maximum_chunk)
			{
				++failures_;
				consume();
				return {IoStatus::Fatal, 0, script_error};
			}

			const auto written = std::min({size, step->size - step_offset_, step->maximum_chunk});
			if(step->bytes && std::memcmp(bytes, step->bytes + step_offset_, written) != 0)
			{
				++failures_;
				return {IoStatus::Fatal, 0, script_error};
			}

			if(written > capture_.size() - capture_size_)
			{
				++failures_;
				return {IoStatus::Fatal, 0, capacity_error};
			}

			std::memcpy(capture_.data() + capture_size_, bytes, written);
			capture_size_ += written;
			step_offset_ += written;
			if(step_offset_ == step->size) consume();

			return {IoStatus::Progress, written};
		}

		WaitResult wait(const Interest *interests, Ready *ready, std::size_t count, std::uint32_t timeout_ms) override
		{
			if(ready) std::fill_n(ready, count, Ready{});

			if(!interests || !ready || !count || count > Step{}.readiness.size())
				return {WaitStatus::InvalidArgument};

			for(std::size_t index = 0; index < count; ++index)
			{
				if(!interests[index].handle.valid || (!interests[index].read && !interests[index].write))
					return {WaitStatus::InvalidArgument};

				if(!find(interests[index].handle)) return {WaitStatus::InvalidState};
			}
			auto *step = next(Operation::Wait, interests[0].handle);
			if(!step) return {WaitStatus::Fatal, 0, script_error};

			if(step->expected_interests)
			{
				if(count != step->expected_interest_count)
				{
					++failures_;
					return {WaitStatus::Fatal, 0, script_error};
				}

				for(std::size_t index = 0; index < count; ++index)
				{
					const auto &expected = step->expected_interests[index];
					if(expected.handle.valid != interests[index].handle.valid ||
					    expected.handle.value != interests[index].handle.value ||
					    expected.read != interests[index].read || expected.write != interests[index].write)
					{
						++failures_;
						return {WaitStatus::Fatal, 0, script_error};
					}
				}
			}

			if(step->elapsed_ms > timeout_ms)
			{
				++failures_;
				consume();
				return {WaitStatus::Fatal, 0, script_error};
			}

			advance(step->elapsed_ms);
			WaitResult result{step->wait_status, 0, step->native_error};
			if(result.status == WaitStatus::Ready)
			{
				for(std::size_t index = 0; index < count; ++index)
				{
					ready[index].readable = interests[index].read && step->readiness[index].readable;
					ready[index].writable = interests[index].write && step->readiness[index].writable;
					ready[index].error = step->readiness[index].error;
					if(ready[index].readable || ready[index].writable || ready[index].error) ++result.ready_count;
				}
				if(!result.ready_count)
				{
					++failures_;
					result = {WaitStatus::Fatal, 0, script_error};
				}
			}

			consume();
			return result;
		}

		OperationResult shutdownWrite(Handle handle) override
		{
			if(!handle.valid) return {Status::InvalidArgument};

			auto *owned = find(handle);
			if(!owned || owned->listener || owned->write_shutdown) return {Status::InvalidState};

			auto *step = next(Operation::ShutdownWrite, handle);
			if(!step) return {Status::Fatal, script_error};

			const OperationResult result{step->status, step->native_error};
			if(result.status == Status::Ok) owned->write_shutdown = true;

			consume();
			return result;
		}

		CloseResult close(Handle &handle) override
		{
			if(!handle.valid) return {Status::InvalidArgument};

			auto *owned = find(handle);
			if(!owned) return {Status::InvalidState};

			auto *step = next(Operation::Close, handle);
			if(!step) return {Status::Fatal, script_error};

			const CloseResult result{step->status, step->native_error, step->released};
			if(result.released)
			{
				*owned = {};
				handle = {};
				++released_count_;
			}

			consume();
			return result;
		}

	};

} // namespace NaWServer::tcp::test
