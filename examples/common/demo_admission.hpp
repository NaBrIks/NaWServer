// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp.h>
#include <NaWServer/admission_http.h>
#include <esp_timer.h>

// Fixture configuration only. The public module has no knowledge of these
// routes/boards or this deliberately public test identity; it is not an auth key.
namespace nawserver_demo_admission
{
	template <class Writer> void encode_pool(Writer &writer, const char *name, NaWServer::MemoryPoolStats stats)
	{
		writer.map(9);
		writer.put("id", name);
		writer.put("fixed", stats.fixed_blocks);
		writer.put("allocated", stats.allocated_blocks);
		writer.put("active", stats.active_blocks);
		writer.put("allocated_bytes", stats.allocated_bytes);
		writer.put("active_bytes", stats.active_bytes);
		writer.put("peak_bytes", stats.peak_bytes);
		writer.put("rejected", stats.rejected);
		writer.put("blocked", static_cast<unsigned>(stats.blocked));
		writer.end();
	}

	template <class Writer, class SseBackend> void encode_memory(Writer &writer, NaWServer::TcpHttpBackend &backend,
	    SseBackend &sse)
	{
		const auto total = backend.bufferMemoryStats();
		const auto pools = backend.managedMemoryStats();
		writer.map(4);
		writer.put("v", 2);
		writer.put("source", "device");
		writer.key("tcp");
		writer.map(6);
		writer.put("used_bytes", total.used_bytes);
		writer.put("peak_bytes", total.peak_bytes);
		writer.put("limit_bytes", total.limit_bytes);
		writer.put("budget_rejections", total.budget_rejections);
		writer.put("allocation_failures", total.allocation_failures);
		writer.put("blocked", static_cast<unsigned>(total.blocked));
		writer.end();
		writer.key("pools");
		writer.array(8);
		encode_pool(writer, "http_headers", pools.headers);
		encode_pool(writer, "http_bodies", pools.bodies);
		encode_pool(writer, "http_responses", pools.responses);
		encode_pool(writer, "ws", pools.streams);
		encode_pool(writer, "reserved_headers", pools.reserved_headers);
		encode_pool(writer, "reserved_bodies", pools.reserved_bodies);
		encode_pool(writer, "reserved_responses", pools.reserved_responses);
		encode_pool(writer, "sse", sse.stats().memory);
		writer.end();
		writer.end();
	}

#if NAWSERVER_CLIENT_QUOTAS
	inline NaWServer::AdmissionOptions configuration()
	{
		NaWServer::AdmissionOptions options;
		options.reserved_key = "naw-qos-test-primary";
		options.reserved = {1, 0, 0, 8, 1000};
		options.shared = {4, 1, 1, 12, 1000};
		options.shared_clients = 2;
		options.idle_timeout_ms = 5000;
		return options;
	}

	inline NaWServer::Admission<2> policy(configuration());
	inline std::uint64_t now() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000); }
	inline NaWServer::AdmissionMiddleware admission(NaWServer::AdmissionRef(policy), now);
#endif

	template<class Counter> auto makeHeaderChain(Counter &counter)
	{
#if NAWSERVER_CLIENT_QUOTAS
		return NaWServer::makeHeaderChain(counter, admission);
#else
		return NaWServer::makeHeaderChain(counter);
#endif
	}

#if NAWSERVER_TCP_MANAGED
	inline void configure(NaWServer::TcpOptions &options)
	{
		options.managed.ingress_slots = 4;
		options.http_memory.enabled = true;
		options.http_memory.initial_requests = 2;
		options.http_memory.maximum_requests = 9;
		options.http_memory.reserved_requests = NAWSERVER_CLIENT_QUOTAS ? 1 : 0;
		options.http_memory.initial_websocket_clients = 0;
		options.keep_alive.enabled = true;
		options.keep_alive.maximum_idle = 4;
		options.keep_alive.idle_timeout_ms = 5000;
		options.keep_alive.maximum_requests = 100;
		options.buffer_memory.max_bytes = 64 * 1024;
		options.buffer_memory.min_free_bytes = 32 * 1024;
	}

	inline void configure(NaWServer::HttpLimits &limits)
	{
		limits.max_connections = 14;
		limits.max_header_bytes = NAWSERVER_TCP_MANAGED_HEAD_BYTES;
	}

#else
	inline void configure(NaWServer::TcpOptions &)
	{
	}

	inline void configure(NaWServer::HttpLimits &)
	{
	}

#endif
}
