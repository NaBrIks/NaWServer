// SPDX-License-Identifier: MIT
#pragma once

#include <NaWServer/backends/tcp.h>
#include <NaWServer/handler.h>
#include <NaWServer/request.h>
#include <NaWServer/response.h>
#include <esp_memory_utils.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>

namespace compact
{

	// The HTTP executor owns this bounded state. A source never retains a Request,
	// Response or byte view; the existing manager owns each filling/queued TX lease.
	class TxBulkProbe final : public NaWServer::HandlerBase
	{
		inline static constexpr NaWServer::RouteDescriptor descriptors_[2]{
			{"/__diagnostic/tx-bulk", NaWServer::HttpMethod::Get},
			{"/__diagnostic/tx-stats", NaWServer::HttpMethod::Get}
		};
		struct TouchedBlock
		{
			std::uintptr_t address = 0;
			std::size_t bytes = 0;
		};
		std::array<TouchedBlock, 32> touched_{};
		NaWServer::TcpHttpBackend *backend_ = nullptr;
		std::size_t expected_ = 0, generated_ = 0, internal_ = 0, external_ = 0;
		std::size_t touched_count_ = 0, touched_bytes_ = 0, read_calls_ = 0;
		std::size_t peak_jobs_ = 0, peak_used_bytes_ = 0, rejected_ = 0;
		std::uint32_t transfer_ = 0;
		bool active_ = false, released_ = false, canceled_ = false;
		std::uint64_t total_generated_ = 0, total_internal_ = 0, total_external_ = 0, total_reads_ = 0;
		std::uint64_t total_released_ = 0, total_completed_ = 0, total_canceled_ = 0;
		std::size_t lifetime_touched_count_ = 0, lifetime_touched_bytes_ = 0;
		std::size_t lifetime_peak_jobs_ = 0, lifetime_peak_used_bytes_ = 0;
		bool counter_overflow_ = false;

		void increment(std::uint64_t &value)
		{
			if(value == std::numeric_limits<std::uint64_t>::max()) counter_overflow_ = true;
			else ++value;
		}

		static bool parseSize(std::string_view query, std::size_t &bytes)
		{
			bytes = 1024 * 1024;
			if(query.empty()) return true;

			constexpr std::string_view prefix = "bytes=";
			if(query.substr(0, prefix.size()) != prefix || query.size() > prefix.size() + 7) return false;

			query.remove_prefix(prefix.size());
			if(query.empty()) return false;

			bytes = 0;
			for(const char value : query)
			{
				if(value < '0' || value > '9') return false;

				const auto digit = static_cast<std::size_t>(value - '0');
				if(bytes > (1024 * 1024 - digit) / 10) return false;

				bytes = bytes * 10 + digit;
			}
			return bytes >= 16 * 1024 && bytes <= 1024 * 1024;
		}

		static NaWServer::BodyReadResult read(void *context, std::size_t offset,
		    std::uint8_t *destination, std::size_t capacity)
		{
			auto &self = *static_cast<TxBulkProbe *>(context);
			if(!self.active_ || offset != self.generated_ || offset > self.expected_ || !destination || !capacity)
				return {NaWServer::BodyReadStatus::TransportError, 0};

			const auto bytes = std::min(capacity, self.expected_ - offset);
			if(!bytes) return {NaWServer::BodyReadStatus::End, 0};

			const bool external = esp_ptr_external_ram(destination);
			if(external != esp_ptr_external_ram(destination + bytes - 1) ||
			    (!external && !esp_ptr_internal(destination)))
				return {NaWServer::BodyReadStatus::TransportError, 0};
			constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
			if(self.total_generated_ > maximum - bytes ||
			    (external ? self.total_external_ : self.total_internal_) > maximum - bytes || self.total_reads_ == maximum)
			{
				self.counter_overflow_ = true;
				return {NaWServer::BodyReadStatus::TransportError, 0};
			}

			// This callback receives the manager's TX WriteLease directly. The bytes
			// are generated in the selected RAM, rather than borrowing immutable Flash.
			for(std::size_t index = 0; index < bytes; ++index)
			{
				const auto position = offset + index;
				destination[index] = static_cast<std::uint8_t>(position * 31u + (position >> 8));
			}
			self.generated_ += bytes;
			(external ? self.external_ : self.internal_) += bytes;
			++self.read_calls_;
			self.total_generated_ += bytes;
			(external ? self.total_external_ : self.total_internal_) += bytes;
			++self.total_reads_;
			const auto address = reinterpret_cast<std::uintptr_t>(destination);
			bool known = false;
			for(std::size_t index = 0; index < self.touched_count_; ++index)
				if(self.touched_[index].address == address) known = true;

			if(!known && self.touched_count_ < self.touched_.size())
			{
				self.touched_[self.touched_count_++] = {address, capacity};
				self.touched_bytes_ += capacity;
			}
			const auto state = self.backend_->transmissionStats();
			self.peak_jobs_ = std::max(self.peak_jobs_, state.queued_jobs + state.filling_jobs);
			self.peak_used_bytes_ = std::max(self.peak_used_bytes_, state.used_bytes);
			self.lifetime_touched_count_ = std::max(self.lifetime_touched_count_, self.touched_count_);
			self.lifetime_touched_bytes_ = std::max(self.lifetime_touched_bytes_, self.touched_bytes_);
			self.lifetime_peak_jobs_ = std::max(self.lifetime_peak_jobs_, self.peak_jobs_);
			self.lifetime_peak_used_bytes_ = std::max(self.lifetime_peak_used_bytes_, self.peak_used_bytes_);
			return {NaWServer::BodyReadStatus::Data, bytes};
		}

		static void release(void *context)
		{
			auto &self = *static_cast<TxBulkProbe *>(context);
			self.canceled_ = self.generated_ != self.expected_;
			self.released_ = true;
			self.active_ = false;
			self.increment(self.total_released_);
			self.increment(self.canceled_ ? self.total_canceled_ : self.total_completed_);
		}

		void stats(NaWServer::Response &response)
		{
			char bytes[1280];
			const auto size = std::snprintf(bytes, sizeof(bytes),
			    "{\"transfer\":%lu,\"active\":%s,\"released\":%s,\"canceled\":%s,"
			    "\"expected_bytes\":%zu,\"generated_bytes\":%zu,\"internal_bytes\":%zu,\"psram_bytes\":%zu,"
			    "\"read_calls\":%zu,\"touched_blocks\":%zu,\"touched_capacity_bytes\":%zu,"
			    "\"peak_global_busy_jobs\":%zu,\"peak_global_used_bytes\":%zu,\"rejected_requests\":%zu,"
			    "\"total_generated_bytes\":\"%llu\",\"total_internal_bytes\":\"%llu\",\"total_psram_bytes\":\"%llu\","
			    "\"total_read_calls\":\"%llu\",\"total_released_transfers\":\"%llu\","
			    "\"total_completed_transfers\":\"%llu\",\"total_canceled_transfers\":\"%llu\","
			    "\"lifetime_touched_blocks\":%zu,\"lifetime_touched_capacity_bytes\":%zu,"
			    "\"lifetime_peak_busy_jobs\":%zu,\"lifetime_peak_used_bytes\":%zu,\"counter_overflow\":%s}",
			    static_cast<unsigned long>(transfer_), active_ ? "true" : "false",
			    released_ ? "true" : "false", canceled_ ? "true" : "false", expected_, generated_, internal_, external_,
			    read_calls_, touched_count_, touched_bytes_, peak_jobs_, peak_used_bytes_, rejected_,
			    static_cast<unsigned long long>(total_generated_), static_cast<unsigned long long>(total_internal_),
			    static_cast<unsigned long long>(total_external_), static_cast<unsigned long long>(total_reads_),
			    static_cast<unsigned long long>(total_released_), static_cast<unsigned long long>(total_completed_),
			    static_cast<unsigned long long>(total_canceled_), lifetime_touched_count_, lifetime_touched_bytes_,
			    lifetime_peak_jobs_, lifetime_peak_used_bytes_, counter_overflow_ ? "true" : "false");
			if(size <= 0 || static_cast<std::size_t>(size) >= sizeof(bytes))
			{
				response.drop();
				return;
			}
			(void)response.send(200, "application/json", {
				reinterpret_cast<const std::uint8_t *>(bytes), static_cast<std::size_t>(size)});
		}

	public:
		void bind(NaWServer::TcpHttpBackend &backend) { backend_ = &backend; }

		NaWServer::RouteDescriptors routes() const override { return {descriptors_, 2}; }

		NaWServer::BodyOptions bodyOptions() const override { return {NaWServer::BodyMode::None}; }

		void handleRequest(NaWServer::Request *request, NaWServer::Response &response) override
		{
			(void)response.header("Cache-Control", "no-store");
			if(request->path() == "/__diagnostic/tx-stats")
			{
				stats(response);
				return;
			}
			std::size_t bytes = 0;
			if(!parseSize(request->rawQuery(), bytes))
			{
				(void)response.text(400, "Use bytes=16384..1048576");
				return;
			}
			if(active_ || !backend_ || !backend_->usesSharedTransmission())
			{
				if(rejected_ == std::numeric_limits<std::size_t>::max()) counter_overflow_ = true;
				else ++rejected_;
				(void)response.text(503, "One shared-TX generator at a time");
				return;
			}
			if(counter_overflow_ || transfer_ == std::numeric_limits<std::uint32_t>::max())
			{
				counter_overflow_ = true;
				(void)response.text(503, "Diagnostic counter range exhausted");
				return;
			}
			++transfer_;
			expected_ = bytes;
			generated_ = internal_ = external_ = touched_count_ = touched_bytes_ = read_calls_ = 0;
			peak_jobs_ = peak_used_bytes_ = 0;
			touched_ = {};
			active_ = true;
			released_ = canceled_ = false;
			NaWServer::OwnedResponseSource source({this, bytes, read}, release);
			if(response.sendOwnedSource(200, "application/octet-stream", std::move(source)) != NaWServer::SendResult::Ok)
				response.drop();
		}
	};

	TxBulkProbe &txBulkProbe()
	{
		static TxBulkProbe instance;
		return instance;
	}

	NaWServer::HandlerBase &txBulkProbeHandler() { return txBulkProbe(); }
}
