// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/backends/tcp_transmission.h>

namespace NaWServer::examples
{
	// Both demo encoders use UTF-8 field names. The caller owns map framing.
	inline constexpr std::size_t transmission_field_count = 29;
	template <class Writer> void transmissionFields(Writer &writer, const tcp::TransmissionStats &stats)
	{
		writer.put("enabled", stats.enabled);
		writer.put("maximum_jobs", stats.maximum_jobs);
		writer.put("maximum_bytes", stats.maximum_bytes);
		writer.put("block_bytes", stats.block_bytes);
		writer.put("step_bytes", stats.step_bytes);
		writer.put("stall_timeout_ms", stats.stall_timeout_ms);
		writer.put("close_timeout_ms", stats.close_timeout_ms);
		writer.put("maximum_jobs_per_connection", stats.maximum_jobs_per_connection);
		writer.put("reserved_important_jobs", stats.reserved_important_jobs);
		writer.put("reserved_important_bytes", stats.reserved_important_bytes);
		writer.put("queued_jobs", stats.queued_jobs);
		writer.put("filling_jobs", stats.filling_jobs);
		writer.put("active_jobs", stats.active_jobs);
		writer.put("allocated_bytes", stats.allocated_bytes);
		writer.put("used_bytes", stats.used_bytes);
		writer.put("peak_bytes", stats.peak_bytes);
		writer.put("queued_bytes", stats.queued_bytes);
		writer.put("accepted_bytes", stats.accepted_bytes);
		writer.put("borrowed_bytes", stats.borrowed_bytes);
		writer.put("sent_bytes", stats.sent_bytes);
		writer.put("discarded_bytes", stats.discarded_bytes);
		writer.put("failed_bytes", stats.failed_bytes);
		writer.put("completed_jobs", stats.completed_jobs);
		writer.put("discarded_jobs", stats.discarded_jobs);
		writer.put("rejected_jobs", stats.rejected_jobs);
		writer.put("failed_jobs", stats.failed_jobs);
		writer.put("would_block_count", stats.would_block_count);
		writer.put("last_failure", static_cast<unsigned>(stats.last_failure));
		writer.put("last_native_error", stats.last_native_error);
	}
}
