// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/detail/sse/encoder.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <NaWServer/memory_manager.h>

namespace NaWServer::sse
{
	// Portable state machine: all calls serialized by owner. No I/O under its lock.
	// One consumer issues one step per slot, performs I/O unlocked, then acknowledges.
	constexpr std::size_t client_capacity = 4, pending_capacity = 3, step_capacity = 256;
	struct Limits
	{
		std::size_t clients = 4, pending = 3, bytes_per_client = 2304, total_bytes = 8192;
		std::uint64_t heartbeat_ms = 15000, stall_ms = 2000, transfer_ms = 5000;
		BufferMemoryOptions buffer_memory{{}, defaultBufferLimit(BufferGroup::Sse)};
		std::size_t initial_clients = 0;
	};
	struct Token
	{
		std::size_t slot = client_capacity;
		std::uint64_t generation = 0;
	};
	inline bool operator==(Token a, Token b)
	{
		return a.slot == b.slot && a.generation == b.generation;
	}

	enum class Publish
	{
		Queued,
		Closed,
		TooLarge,
		IndividualOverflow,
		GlobalFull,
		Busy
	};
	enum class CloseReason
	{
		None,
		Requested,
		Overflow,
		Stall,
		Deadline,
		Disconnected,
		Shutdown
	};
	enum class StepKind
	{
		None,
		Write,
		Close
	};
	struct Step
	{
		StepKind kind = StepKind::None;
		Token token;
		std::uint64_t serial = 0;
		std::array<char, step_capacity> bytes{};
		std::size_t size = 0;
		int fd = -1;
	};
	inline Frame default_header()
	{
		constexpr std::string_view header = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: "
		    "no-cache\r\nConnection: close\r\n\r\n";
		Frame out;
		std::memcpy(out.bytes.data(), header.data(), header.size());
		out.size = header.size();
		return out;
	}

	class Sessions
	{
	public:
		explicit Sessions(Limits limits = {})
		  : limits_(limits)
		{
		}

		Sessions(const Sessions &) = delete;
		Sessions &operator=(const Sessions &) = delete;
		bool valid_limits() const
		{
			return limits_.clients && limits_.clients <= client_capacity &&
			       limits_.initial_clients <= limits_.clients && limits_.pending &&
			       limits_.pending <= pending_capacity && limits_.bytes_per_client && limits_.total_bytes &&
			       limits_.heartbeat_ms && limits_.stall_ms && limits_.transfer_ms && limits_.buffer_memory.max_bytes;
		}

		// Explicit adapter startup allocates payload storage once. Pure state-machine
		// users may let reserve initialize it; constructors never allocate.
		bool prepareStorage(bool direct_only = false)
		{
			if(storage_ready_) return direct_only_ == direct_only;

			if(direct_only)
			{
				if(!valid_limits()) return false;

				direct_only_ = storage_ready_ = true;
				return true;
			}

			if(!valid_limits() || !buffers_.initialize(limits_.buffer_memory, BufferGroup::Sse)) return false;

			if(!frames_memory_.initialize(buffers_, {limits_.initial_clients, limits_.clients, sizeof(FrameSet)}))
			{
				frames_memory_.reset();
				buffers_.reset();
				return false;
			}

			storage_ready_ = true;
			direct_only_ = false;
			return true;
		}

		void releaseStorage()
		{
			if(active()) return;

			for(auto &slot : slots_)
				slot.frames = nullptr;
			for(auto &block : frames_)
				block.reset();
			frames_memory_.reset();
			storage_ready_ = false;
			direct_only_ = false;
			buffers_.reset();
		}

		BufferMemoryStats bufferMemoryStats() const
		{
			return buffers_.stats();
		}

		MemoryPoolStats managedMemoryStats() const
		{
			return frames_memory_.stats();
		}

		bool hasCapacity() const
		{
			for(std::size_t index = 0; index < limits_.clients; ++index)
				if(!slots_[index].active && slots_[index].generation != std::numeric_limits<std::uint64_t>::max())
					return true;

			return false;
		}

		Token reserve(int fd, const Frame &header, std::uint64_t now)
		{
			if(!valid_limits() || stopping_ || fd < 0 || !header.size || header.size > frame_capacity ||
			    !prepareStorage())
				return {};

			for(const auto &s : slots_)
				if(s.active && s.fd == fd)
					return {};

			for(std::size_t i = 0; i < limits_.clients; ++i)
			{
				auto &s = slots_[i];
				if(s.active || s.generation == std::numeric_limits<std::uint64_t>::max())
					continue;

				frames_[i] = frames_memory_.acquire(sizeof(FrameSet));
				if(!frames_[i]) return {};

				auto generation = s.generation + 1;
				auto *frames = new(frames_[i].data()) FrameSet{};
				s = Slot{};
				s.frames = frames;
				s.active = true;
				s.fd = fd;
				s.generation = generation;
				s.idle_since = s.started = s.progress = now;
				s.header = true;
				s.frames->current = header;
				return {i, generation};
			}
			return {};
		}

		// Reserved slots never issue I/O, including close. Callback must settle them
		// with activate (external ownership) or rollback (original callback owns fd).
		bool activate(Token token, std::uint64_t now)
		{
			auto *s = find(token);
			if(!s || s->activated)
				return false;

			s->activated = true;
			s->idle_since = s->started = s->progress = now;
			return !stopping_ && s->closing == CloseReason::None;
		}

		bool rollback(Token token)
		{
			auto *s = find(token);
			if(!s || s->activated || s->inflight)
				return false;

			const auto generation = s->generation;
			*s = Slot{};
			frames_[token.slot].reset();
			s->generation = generation;
			return true;
		}

		Token open(int fd, std::uint64_t now)
		{
			auto token = reserve(fd, default_header(), now);
			activate(token, now);
			return token;
		}

		Token reserveDirect(int fd, std::size_t header_size, std::uint64_t now)
		{
			if(stopping_ || fd < 0 || !header_size || header_size > frame_capacity || !prepareStorage(true)) return {};

			for(const auto &slot : slots_) if(slot.active && slot.fd == fd) return {};

			for(std::size_t index = 0; index < limits_.clients; ++index)
			{
				auto &slot = slots_[index];
				if(slot.active || slot.generation == std::numeric_limits<std::uint64_t>::max()) continue;

				const auto generation = slot.generation + 1;
				slot = Slot{};
				slot.active = slot.direct = slot.header = true;
				slot.fd = fd;
				slot.generation = generation;
				slot.idle_since = slot.started = slot.progress = now;
				addDirect(slot, header_size, false, now);
				return {index, generation};
			}
			return {};
		}

		bool direct(Token token) const
		{
			return token.slot < slots_.size() && slots_[token.slot].active &&
			       slots_[token.slot].generation == token.generation && slots_[token.slot].direct;
		}

		std::size_t directHeaderSize(Token token) const
		{
			return direct(token) ? slots_[token.slot].direct_entries[0].size : 0;
		}

		Publish directReadiness(Token token, std::size_t size)
		{
			auto *slot = find(token);
			if(!slot || !slot->direct || !slot->activated || stopping_ || slot->closing != CloseReason::None)
				return Publish::Closed;

			if(!size || size > frame_capacity) return Publish::TooLarge;

			if(slot->count == limits_.pending || size > limits_.bytes_per_client - slot->bytes)
			{
				slot->closing = CloseReason::Overflow;
				return Publish::IndividualOverflow;
			}
			return size > limits_.total_bytes - total_bytes_ ? Publish::GlobalFull : Publish::Queued;
		}

		void directQueued(Token token, std::size_t size, bool event, std::uint64_t now)
		{
			auto *slot = find(token);
			if(!slot) return;

			addDirect(*slot, size, event, now);
			slot->heartbeat_waiting = false;
			if(event) { ++slot->count; slot->bytes += size; total_bytes_ += size; }
		}

		bool directHeartbeat(Token token, std::uint64_t now) const
		{
			return direct(token) && !slots_[token.slot].direct_count &&
			       slots_[token.slot].closing == CloseReason::None && !stopping_ &&
			       now - slots_[token.slot].idle_since >= limits_.heartbeat_ms;
		}

		void directHeartbeatBlocked(Token token, std::uint64_t now)
		{
			auto *slot = find(token);
			if(!slot) return;

			if(!slot->heartbeat_waiting)
			{
				slot->heartbeat_waiting = true;
				slot->heartbeat_started = now;
			}
			else if(now - slot->heartbeat_started >= limits_.stall_ms)
				close(token, CloseReason::Stall);
		}

		void directProgress(Token token, std::size_t remaining, bool failed, std::uint64_t now)
		{
			auto *slot = find(token);
			if(!slot || !slot->direct) return;

			if(failed || remaining > slot->direct_bytes)
			{
				close(token, CloseReason::Disconnected);
				return;
			}
			auto accepted = slot->direct_bytes - remaining;
			const bool was_sending = slot->sending;
			if(accepted) slot->progress = now;

			while(accepted && slot->direct_count)
			{
				auto &entry = slot->direct_entries[slot->direct_head];
				const auto bytes = std::min(accepted, entry.remaining);
				entry.remaining -= bytes; slot->direct_bytes -= bytes; accepted -= bytes;
				if(entry.remaining) break;

				if(entry.event)
				{
					--slot->count; slot->bytes -= entry.size; total_bytes_ -= entry.size; ++sent_frames_;
				}
				else if(slot->header) slot->header = false;

				slot->direct_head = (slot->direct_head + 1) % slot->direct_entries.size();
				--slot->direct_count;
				slot->started = slot->progress = now;
			}
			slot->sending = slot->direct_count != 0;
			if(was_sending && !slot->sending) slot->idle_since = now;
		}

		Publish publish(Token token, const Frame &frame)
		{
			auto *s = find(token);
			if(!s || !s->activated || s->closing != CloseReason::None || stopping_)
				return Publish::Closed;

			if(!frame.size || frame.size > frame_capacity)
				return Publish::TooLarge;

			if(s->count == limits_.pending || frame.size > limits_.bytes_per_client - s->bytes)
			{
				s->closing = CloseReason::Overflow;
				return Publish::IndividualOverflow;
			}

			if(frame.size > limits_.total_bytes - total_bytes_)
				return Publish::GlobalFull;

			s->frames->queue[(s->head + s->count) % pending_capacity] = frame;
			++s->count;
			s->bytes += frame.size;
			total_bytes_ += frame.size;
			return Publish::Queued;
		}

		void close(Token token, CloseReason why = CloseReason::Requested)
		{
			if(auto *s = find(token); s && s->closing == CloseReason::None)
				s->closing = why;
		}

		void shutdown()
		{
			stopping_ = true;
			for(auto &s : slots_)
				if(s.active && s.closing == CloseReason::None)
					s.closing = CloseReason::Shutdown;
		}

		// Resume only a fully drained service. Retain generations and diagnostics:
		// replacing Sessions would make an old published token valid after restart.
		bool restart()
		{
			if(active()) return false;

			stopping_ = false;
			return true;
		}

		Step next(std::size_t index, std::uint64_t now)
		{
			if(index >= client_capacity)
				return {};

			auto &s = slots_[index];
			if(!s.active || !s.activated || s.inflight)
				return {};

			// Deadline belongs to one header/event/comment, never the subscription age.
			if(s.sending && s.closing == CloseReason::None)
			{
				if(now - s.started >= limits_.transfer_ms)
					s.closing = CloseReason::Deadline;
				else if(now - s.progress >= limits_.stall_ms)
					s.closing = CloseReason::Stall;
			}

			Step step;
			step.token = {index, s.generation};
			step.fd = s.fd;
			if(s.closing != CloseReason::None)
				step.kind = StepKind::Close;
			else if(s.direct)
				return {}; // Shared sender owns payload and reports native progress separately.
			else
			{
				if(!s.sending)
				{
					if(s.header)
					{
						// Owned header was copied before reservation became visible.
					}
					else if(s.count)
					{
						s.frames->current = s.frames->queue[s.head];
						s.current_queued = true;
					}
					else if(now - s.idle_since >= limits_.heartbeat_ms)
					{
						comment("heartbeat", s.frames->current);
						s.current_queued = false;
					}
					else
						return {};

					s.sending = true;
					s.started = s.progress = now;
					s.offset = 0;
				}

				step.kind = StepKind::Write;
				step.size = std::min(step_capacity, s.frames->current.size - s.offset);
				std::memcpy(step.bytes.data(), s.frames->current.bytes.data() + s.offset, step.size);
			}

			s.inflight = true;
			step.serial = ++s.serial;
			s.issued = step.size;
			return step;
		}

		// written: -1 fatal/disconnect, 0 would-block, positive actual partial write.
		bool written(const Step &step, int written, std::uint64_t now)
		{
			auto *s = find(step.token);
			if(!s || !s->inflight || s->serial != step.serial || step.kind != StepKind::Write)
				return false;

			s->inflight = false;
			if(written < 0 || static_cast<std::size_t>(written) > s->issued)
			{
				close(step.token, CloseReason::Disconnected);
				return true;
			}

			if(now - s->started >= limits_.transfer_ms)
				close(step.token, CloseReason::Deadline);

			if(written)
			{
				s->offset += static_cast<std::size_t>(written);
				s->progress = now;
			}

			if(s->offset == s->frames->current.size)
			{
				s->sending = false;
				s->idle_since = now;
				if(s->header)
					s->header = false;
				else if(s->current_queued)
				{
					total_bytes_ -= s->frames->queue[s->head].size;
					s->bytes -= s->frames->queue[s->head].size;
					s->head = (s->head + 1) % pending_capacity;
					--s->count;
					++sent_frames_;
				}
			}

			return true;
		}

		// Only after transport has stopped fd use and completed the async request.
		bool completed(const Step &step)
		{
			auto *s = find(step.token);
			if(!s || !s->inflight || s->serial != step.serial || step.kind != StepKind::Close)
				return false;

			total_bytes_ -= s->bytes;
			auto generation = s->generation;
			*s = Slot{};
			frames_[step.token.slot].reset();
			s->generation = generation;
			++completed_;
			return true;
		}

		std::size_t active() const
		{
			std::size_t n = 0;
			for(const auto &s : slots_)
				n += s.active;
			return n;
		}

		// Idle subscriptions need only their next heartbeat. Pending output and
		// retained closes keep the adapter's existing bounded completion cadence.
		std::uint32_t pollDelay(std::uint64_t now) const
		{
			std::uint32_t delay = UINT32_MAX;
			for(const auto &slot : slots_)
			{
				if(!slot.active || !slot.activated) continue;

				if(slot.closing != CloseReason::None || slot.sending || slot.header || slot.count ||
				    slot.inflight || slot.heartbeat_waiting) return 1;

				const auto elapsed = now >= slot.idle_since ? now - slot.idle_since : 0;
				if(elapsed >= limits_.heartbeat_ms) return 1;

				delay = std::min(delay, static_cast<std::uint32_t>(std::min<std::uint64_t>(
				    limits_.heartbeat_ms - elapsed, UINT32_MAX - 1)));
			}
			return delay;
		}

		std::size_t queued_bytes() const
		{
			return total_bytes_;
		}

		std::size_t sent_frames() const
		{
			return sent_frames_;
		} // socket acceptance, not browser acknowledgement

		std::size_t completions() const
		{
			return completed_;
		}

		CloseReason reason(Token token) const
		{
			if(token.slot >= client_capacity)
				return CloseReason::None;

			const auto &s = slots_[token.slot];
			return s.active && s.generation == token.generation ? s.closing : CloseReason::None;
		}

	private:
		struct FrameSet
		{
			std::array<Frame, pending_capacity> queue{};
			Frame current;
		};
		struct Slot
		{
			struct DirectEntry { std::size_t size = 0, remaining = 0; bool event = false; };
			std::array<DirectEntry, pending_capacity + 2> direct_entries{};
			std::size_t direct_head = 0, direct_count = 0, direct_bytes = 0;
			bool direct = false;
			bool heartbeat_waiting = false;
			std::uint64_t heartbeat_started = 0;
			bool active = false, activated = false, inflight = false, sending = false, header = false,
			    current_queued = false;
			int fd = -1;
			std::uint64_t generation = 0, serial = 0, started = 0, progress = 0, idle_since = 0;
			CloseReason closing = CloseReason::None;
			FrameSet *frames = nullptr;
			std::size_t head = 0, count = 0, bytes = 0, offset = 0, issued = 0;
		};
		static void addDirect(Slot &slot, std::size_t size, bool event, std::uint64_t now)
		{
			if(!slot.direct_count) { slot.started = slot.progress = now; slot.sending = true; }

			slot.direct_entries[(slot.direct_head + slot.direct_count) % slot.direct_entries.size()] = {size, size, event};
			++slot.direct_count;
			slot.direct_bytes += size;
		}
		Slot *find(Token t)
		{
			if(t.slot >= client_capacity)
				return nullptr;

			auto &s = slots_[t.slot];
			return s.active && s.generation == t.generation ? &s : nullptr;
		}

		Limits limits_;
		NaWServer::detail::BufferPool buffers_;
		MemoryManager frames_memory_;
		std::array<MemoryBlock, client_capacity> frames_;
		std::array<Slot, client_capacity> slots_{};
		std::size_t total_bytes_ = 0, sent_frames_ = 0, completed_ = 0;
		bool stopping_ = false;
		bool storage_ready_ = false;
		bool direct_only_ = false;
	};
} // namespace NaWServer::sse
