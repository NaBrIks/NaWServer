// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

#ifndef NAWSERVER_ADMISSION_KEY_BYTES
#define NAWSERVER_ADMISSION_KEY_BYTES 64
#endif
#ifndef NAWSERVER_ADMISSION_MAX_ACTIVE
#define NAWSERVER_ADMISSION_MAX_ACTIVE 32
#endif
#ifndef NAWSERVER_ADMISSION_MAX_RESERVED_KEYS
#define NAWSERVER_ADMISSION_MAX_RESERVED_KEYS 5
#endif

namespace NaWServer
{

	enum class AdmissionResource : std::uint8_t { Http, WebSocket, EventStream };
	enum class AdmissionDecision { Accepted, Capacity, Rate, Invalid };
	struct AdmissionLimits
	{
		std::uint16_t http = 1, websocket = 1, event_stream = 1;
		// Zero disables rate limiting; this is a fixed window, not a task/timer.
		std::uint16_t requests_per_window = 0;
		std::uint32_t window_ms = 1000;
	};
	struct AdmissionOptions
	{
		// Borrowed immutable configuration (normally literals/static storage).
		// This key reserves capacity; authentication remains an application concern.
		std::string_view reserved_key;
		AdmissionLimits reserved;
		AdmissionLimits shared;
		std::uint16_t shared_clients = 1;
		std::uint32_t idle_timeout_ms = 5000;
		struct ReservedProfile
		{
			std::string_view key;
			AdmissionLimits limits;
		};
		// Immutable profiles outlive the policy. Do not mutate keys while leases
		// are active; the application owns credential storage and publication.
		const ReservedProfile *reserved_profiles = nullptr;
		std::size_t reserved_profile_count = 0;
	};
	struct AdmissionStats
	{
		std::array<std::uint16_t, 3> active{};
		std::uint16_t active_reserved = 0, active_shared_clients = 0;
		std::uint32_t accepted = 0, capacity_rejected = 0, rate_rejected = 0;
	};
	struct AdmissionTicket
	{
		std::size_t client = 0;
		std::uint32_t generation = 0;
		AdmissionResource resource = AdmissionResource::Http;
		std::size_t permit = NAWSERVER_ADMISSION_MAX_ACTIVE;
		explicit operator bool() const {
			return generation != 0;
		}
	};
	struct AdmissionResult
	{
		AdmissionDecision decision = AdmissionDecision::Invalid;
		AdmissionTicket ticket;
		std::uint32_t retry_after_ms = 0;
	};

	// Allocation-free optional policy, independent of HTTP parsers, sockets and
	// applications. One owner serializes calls. Each client owns its configured
	// quota: unused reserved capacity is never lent to another identity.
	template <std::size_t SharedCapacity> class Admission
	{
		static_assert(SharedCapacity > 0, "At least one shared client slot is required");
		static_assert(NAWSERVER_ADMISSION_KEY_BYTES > 0, "Admission keys need bounded storage");
		static_assert(NAWSERVER_ADMISSION_MAX_RESERVED_KEYS > 0, "Reserved profile capacity must be positive");
		struct Client
		{
			std::array<char, NAWSERVER_ADMISSION_KEY_BYTES> key{};
			std::size_t key_size = 0;
			std::array<std::uint16_t, 3> active{};
			std::uint64_t last_used = 0, window_started = 0;
			std::uint32_t generation = 0;
			std::uint16_t requests = 0;
			bool used = false, retired = false;
			bool busy() const
			{
				return active[0] || active[1] || active[2];
			}

		};
		AdmissionOptions options_;
		std::array<Client, SharedCapacity + NAWSERVER_ADMISSION_MAX_RESERVED_KEYS> clients_{};
		struct Permit
		{
			std::size_t client = 0;
			std::uint32_t generation = 0;
			AdmissionResource resource = AdmissionResource::Http;
			bool active = false;
		};
		std::array<Permit, NAWSERVER_ADMISSION_MAX_ACTIVE> permits_{};
		bool valid_;
		std::size_t reserved_count_ = 0;
		std::uint32_t accepted_ = 0, capacity_rejected_ = 0, rate_rejected_ = 0;

		static void increment(std::uint32_t &value)
		{
			if(value != std::numeric_limits<std::uint32_t>::max()) ++value;
		}

		AdmissionResult refused(AdmissionDecision decision, std::uint32_t retry)
		{
			if(decision == AdmissionDecision::Capacity) increment(capacity_rejected_);
			if(decision == AdmissionDecision::Rate) increment(rate_rejected_);
			return {decision, {}, retry};
		}

		constexpr std::size_t sharedBegin() const
		{
			return std::max<std::size_t>(reserved_count_, 1);
		}

		static bool equal(std::string_view left, std::string_view right)
		{
			if(left.size() != right.size()) return false;

			unsigned difference = 0;
			for(std::size_t index = 0; index < left.size(); ++index)
				difference |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
			return difference == 0;
		}

	public:
		constexpr explicit Admission(AdmissionOptions options = {})
		  : options_(options), valid_(options.shared_clients <= SharedCapacity && options.idle_timeout_ms &&
		        options.reserved_key.size() <= NAWSERVER_ADMISSION_KEY_BYTES &&
		        options.reserved.window_ms && options.shared.window_ms)
		{
			reserved_count_ = options.reserved_profile_count ? options.reserved_profile_count :
			    (options.reserved_key.empty() ? 0 : 1);
			valid_ = valid_ && reserved_count_ <= NAWSERVER_ADMISSION_MAX_RESERVED_KEYS &&
			    (!options.reserved_profile_count || (options.reserved_profiles && options.reserved_key.empty()));
			const auto count = [](const AdmissionLimits &limits)
				{
					return std::size_t{limits.http} + limits.websocket + limits.event_stream;
				};
			std::size_t protected_permits = options.reserved_profile_count ? 0 :
			    (reserved_count_ ? count(options.reserved) : 0);
			if(valid_ && options.reserved_profile_count)
			{
				for(std::size_t index = 0; index < reserved_count_; ++index)
				{
					const auto &profile = options.reserved_profiles[index];
					valid_ = valid_ && !profile.key.empty() && profile.key.size() <= NAWSERVER_ADMISSION_KEY_BYTES &&
					    profile.limits.window_ms;
					for(std::size_t previous = 0; previous < index; ++previous)
						valid_ = valid_ && profile.key != options.reserved_profiles[previous].key;
					protected_permits += count(profile.limits);
				}
			}
			valid_ = valid_ && protected_permits <= permits_.size() &&
			    count(options.shared) <= (permits_.size() - protected_permits) /
			    std::max<std::size_t>(options.shared_clients, 1);
		}

		bool valid() const
		{
			return valid_;
		}

		constexpr const AdmissionOptions &options() const
		{
			return options_;
		}

		constexpr std::size_t reservedProfiles() const
		{
			return reserved_count_;
		}

		bool reserved(AdmissionTicket ticket) const
		{
			return ticket && ticket.client < reserved_count_;
		}

		bool protectedKey(std::string_view key) const
		{
			if(!valid_ || key.empty()) return false;
			bool found = false;
			for(std::size_t index = 0; index < reserved_count_; ++index)
				found = equal(key, options_.reserved_profile_count ? options_.reserved_profiles[index].key :
				    options_.reserved_key) || found;
			return found;
		}

		AdmissionStats stats() const
		{
			AdmissionStats result;
			result.accepted = accepted_;
			result.capacity_rejected = capacity_rejected_;
			result.rate_rejected = rate_rejected_;
			if(!valid_) return result;
			for(const auto &permit : permits_)
			{
				if(!permit.active) continue;
				++result.active[static_cast<std::size_t>(permit.resource)];
				if(permit.client < reserved_count_) ++result.active_reserved;
			}
			for(std::size_t index = sharedBegin(); index < sharedBegin() + options_.shared_clients; ++index)
				if(index < clients_.size() && clients_[index].busy()) ++result.active_shared_clients;
			return result;
		}

		AdmissionResult acquire(std::string_view key, AdmissionResource resource, std::uint64_t now,
		    bool charge_rate = true)
		{
			const auto kind = static_cast<std::size_t>(resource);
			if(!valid_ || key.size() > NAWSERVER_ADMISSION_KEY_BYTES || kind >= 3) return {};

			std::size_t reserved_index = reserved_count_;
			for(std::size_t index = 0; index < reserved_count_; ++index)
			{
				const auto expected = options_.reserved_profile_count ? options_.reserved_profiles[index].key :
				    options_.reserved_key;
				if(equal(key, expected)) reserved_index = index;
			}
			const bool reserved = reserved_index < reserved_count_;
			const auto &limits = reserved ? (options_.reserved_profile_count ?
			        options_.reserved_profiles[reserved_index].limits : options_.reserved) : options_.shared;
			const std::uint16_t capacities[]{limits.http, limits.websocket, limits.event_stream};
			if(!capacities[kind]) return refused(AdmissionDecision::Capacity, 1000);

			std::size_t selected = reserved_index;
			if(!reserved)
			{
				selected = clients_.size();
				std::size_t available = selected;
				for(std::size_t index = sharedBegin(); index < sharedBegin() + options_.shared_clients; ++index)
				{
					auto &client = clients_[index];
					if(client.used && equal(key, {client.key.data(), client.key_size}))
					{
						selected = index;
						break;
					}

					if(!client.retired && !client.busy() && (!client.used ||
					    (now >= client.last_used && now - client.last_used >= options_.idle_timeout_ms)))
						available = index;
				}
				if(selected == clients_.size()) selected = available;

				if(selected == clients_.size()) return refused(AdmissionDecision::Capacity, options_.idle_timeout_ms);
			}

			auto &client = clients_[selected];
			if(client.used && now < client.last_used) return {};

			if(!client.used || (!reserved && !equal(key, {client.key.data(), client.key_size})))
			{
				if(client.generation == std::numeric_limits<std::uint32_t>::max())
				{
					client.retired = true;
					return refused(AdmissionDecision::Capacity, options_.idle_timeout_ms);
				}

				++client.generation;
				client.used = true;
				client.key_size = key.size();
				if(!key.empty()) std::memcpy(client.key.data(), key.data(), key.size());

				client.requests = 0;
				client.window_started = now;
			}

			if(now >= client.window_started && now - client.window_started >= limits.window_ms)
			{
				client.requests = 0;
				client.window_started = now;
			}

			if(charge_rate && limits.requests_per_window && client.requests >= limits.requests_per_window)
				return refused(AdmissionDecision::Rate, static_cast<std::uint32_t>(limits.window_ms -
				        std::min<std::uint64_t>(limits.window_ms, now - client.window_started)));

			if(client.active[kind] >= capacities[kind]) return refused(AdmissionDecision::Capacity, 1000);

			std::size_t permit = 0;
			while(permit < permits_.size() && (permits_[permit].active ||
			    permits_[permit].generation == std::numeric_limits<std::uint32_t>::max()))
				++permit;
			if(permit == permits_.size()) return refused(AdmissionDecision::Capacity, 1000);

			auto &lease = permits_[permit];
			lease.client = selected;
			lease.resource = resource;
			lease.active = true;
			++lease.generation;
			++client.active[kind];
			if(charge_rate && limits.requests_per_window) ++client.requests;

			client.last_used = now;
			increment(accepted_);
			return {AdmissionDecision::Accepted, {selected, lease.generation, resource, permit}, 0};
		}

		bool release(AdmissionTicket ticket, std::uint64_t now)
		{
			const auto kind = static_cast<std::size_t>(ticket.resource);
			if(!ticket || ticket.client >= clients_.size() || ticket.permit >= permits_.size() || kind >= 3)
				return false;

			auto &permit = permits_[ticket.permit];
			if(!permit.active || permit.generation != ticket.generation || permit.client != ticket.client ||
			    permit.resource != ticket.resource) return false;

			auto &client = clients_[ticket.client];
			if(!client.used || !client.active[kind]) return false;

			permit.active = false;
			--client.active[kind];
			client.last_used = now;
			return true;
		}

		// Reuse one permit during an upgrade; failed reclassification leaves
		// the original HTTP permit intact so rejection/cleanup remains owned.
		bool transfer(AdmissionTicket &ticket, AdmissionResource resource, std::uint64_t now)
		{
			const auto previous = static_cast<std::size_t>(ticket.resource);
			const auto next = static_cast<std::size_t>(resource);
			if(!valid_ || !ticket || ticket.client >= clients_.size() ||
			    ticket.permit >= permits_.size() || previous >= 3 || next >= 3) return false;
			auto &permit = permits_[ticket.permit];
			if(!permit.active || permit.generation != ticket.generation || permit.client != ticket.client ||
			    permit.resource != ticket.resource) return false;
			auto &client = clients_[ticket.client];
			if(!client.active[previous] || now < client.last_used) return false;
			if(previous == next) return true;
			const auto &limits = reserved(ticket) ? (options_.reserved_profile_count ?
			        options_.reserved_profiles[ticket.client].limits : options_.reserved) : options_.shared;
			const std::uint16_t capacities[]{limits.http, limits.websocket, limits.event_stream};
			if(client.active[next] >= capacities[next])
			{
				increment(capacity_rejected_);
				return false;
			}
			--client.active[previous];
			++client.active[next];
			client.last_used = now;
			permit.resource = resource;
			ticket.resource = resource;
			return true;
		}

	};

	// Borrow a statically sized policy without imposing its template capacity on
	// transports or middleware. No ownership, virtual object or heap allocation.
	class AdmissionRef
	{
		void *context_ = nullptr;
		AdmissionResult (*acquire_)(void *, std::string_view, AdmissionResource, std::uint64_t, bool) = nullptr;
		bool (*release_)(void *, AdmissionTicket, std::uint64_t) = nullptr;
		const AdmissionOptions *options_ = nullptr;
		bool (*valid_)(void *) = nullptr;
		bool (*reserved_)(void *, AdmissionTicket) = nullptr;
		bool (*transfer_)(void *, AdmissionTicket &, AdmissionResource, std::uint64_t) = nullptr;
		AdmissionStats (*stats_)(void *) = nullptr;
		bool (*protected_key_)(void *, std::string_view) = nullptr;
	public:
		constexpr AdmissionRef() = default;
		template <std::size_t N> constexpr AdmissionRef(Admission<N> &policy)
		  : context_(&policy),
		    acquire_([](void *p, std::string_view key, AdmissionResource resource, std::uint64_t now, bool rate)
		{
			return static_cast<Admission<N> *>(p)->acquire(key, resource, now, rate);
		}),
		    release_([](void *p, AdmissionTicket ticket, std::uint64_t now)
		{
			return static_cast<Admission<N> *>(p)->release(ticket, now);
		}),
		    options_(&policy.options()), valid_([](void *p) {
			return static_cast<Admission<N> *>(p)->valid();
		}), reserved_([](void *p, AdmissionTicket ticket) {
			return static_cast<Admission<N> *>(p)->reserved(ticket);
		}), transfer_([](void *p, AdmissionTicket &ticket, AdmissionResource resource, std::uint64_t now) {
			return static_cast<Admission<N> *>(p)->transfer(ticket, resource, now);
		}), stats_([](void *p) {
			return static_cast<Admission<N> *>(p)->stats();
		}), protected_key_([](void *p, std::string_view key) {
			return static_cast<Admission<N> *>(p)->protectedKey(key);
		})
		{
		}

		explicit operator bool() const
		{
			return context_ != nullptr;
		}

		const void *identity() const
		{
			return context_;
		}

		bool valid() const
		{
			return context_ && valid_(context_);
		}

		const AdmissionOptions &options() const
		{
			return *options_;
		}

		AdmissionResult acquire(std::string_view key, AdmissionResource resource, std::uint64_t now,
		    bool rate = true) const
		{
			return context_ ? acquire_(context_, key, resource, now, rate) : AdmissionResult{};
		}

		bool release(AdmissionTicket ticket, std::uint64_t now) const
		{
			return context_ && release_(context_, ticket, now);
		}

		bool reserved(AdmissionTicket ticket) const
		{
			return context_ && reserved_(context_, ticket);
		}

		bool transfer(AdmissionTicket &ticket, AdmissionResource resource, std::uint64_t now) const
		{
			return context_ && transfer_(context_, ticket, resource, now);
		}

		AdmissionStats stats() const
		{
			return context_ ? stats_(context_) : AdmissionStats{};
		}

		bool protectedKey(std::string_view key) const
		{
			return context_ && protected_key_(context_, key);
		}

	};

} // namespace NaWServer
