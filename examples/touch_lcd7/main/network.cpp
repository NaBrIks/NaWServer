// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "network.hpp"
#include <cstdio>
#include <cstring>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "telemetry.hpp"
namespace lcd7_web
{
	namespace
	{
		NetworkState current;
		portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
	} // namespace
	NetworkState network_state()
	{
		portENTER_CRITICAL(&lock);
		auto copy = current;
		portEXIT_CRITICAL(&lock);
		return copy;
	}

	// Each concrete transport supplies its owned connection count.
	void network_collect(unsigned count, unsigned ws, unsigned sse)
	{
		portENTER_CRITICAL(&lock);
		current.tcp_open = count;
		current.ws_open = ws;
		current.sse_open = sse;
		current.valid = true;
		current.sampled_ms = esp_timer_get_time() / 1000;
		portEXIT_CRITICAL(&lock);
	}

	const char *network_admission(const char *kind)
	{
		const auto state = network_state();
		if(state.valid && state.tcp_open >= network_policy.httpd_connections - network_policy.rejection_reserve)
			return "http_reserve";

		if(!std::strcmp(kind, "ws") && state.ws_open >= network_policy.websocket_clients)
			return "ws_limit";

		if(!std::strcmp(kind, "sse") && state.sse_open >= network_policy.sse_clients)
			return "sse_limit";

		return nullptr;
	}

	void network_rejected(const char *reason)
	{
		portENTER_CRITICAL(&lock);
		++current.rejections;
		current.last_rejection = reason;
		portEXIT_CRITICAL(&lock);
		count_rejected();
	}

	void encode_network(Writer &w)
	{
		const auto s = network_state();
		w.map(16);
		w.put("socket_limit", CONFIG_LWIP_MAX_SOCKETS);
		w.put("tcp_pcb_limit", CONFIG_LWIP_MAX_ACTIVE_TCP);
		w.put("reserved_sockets", 3);
		w.put("httpd_limit", network_policy.httpd_connections);
		w.put("httpd_open", s.tcp_open);
		w.put(
			"httpd_free",
			network_policy.httpd_connections > s.tcp_open ? network_policy.httpd_connections - s.tcp_open : 0);
		w.put("http_reserve", network_policy.rejection_reserve);
		w.put("ws_limit", network_policy.websocket_clients);
		w.put("ws_clients", s.ws_open);
		w.put("sse_limit", network_policy.sse_clients);
		w.put("sse_clients", s.sse_open);
		w.put("admission_rejected", s.rejections);
		w.put("last_rejection", s.last_rejection);
		w.put("retry_after_ms", network_policy.retry_after_ms);
		w.put("captured_ms", s.sampled_ms);
		w.put("ready", s.valid);
		w.end();
	}

	NaWServer::SendResult network_busy(NaWServer::HeaderRequest &, NaWServer::HeaderResponse &response, const char *reason)
	{
		network_rejected(reason);
		char retry[16];
		std::snprintf(retry, sizeof(retry), "%u", (network_policy.retry_after_ms + 999) / 1000);
		(void)response.header("Retry-After", retry);
		(void)response.header("Connection", "close");
		(void)response.header("Cache-Control", "no-store");
		return response.text(503, "Device busy; retry or close an unused tab.");
	}

	NaWServer::SendResult network_busy(NaWServer::HeaderRequest &request, NaWServer::Response &response, const char *reason)
	{
		network_rejected(reason);
		const auto state = network_state();
		char body[1024], retry[16];
		std::snprintf(retry, sizeof(retry), "%u", (network_policy.retry_after_ms + 999) / 1000);
		(void)response.header("Retry-After", retry);
		(void)response.options(
			{NaWServer::ContentType::Default,
			 NaWServer::CachePolicy::NoStore,
			 NaWServer::HeaderFlags::NoSniff,
			 NaWServer::ConnectionPolicy::Close});
		const bool document = request.path() == "/";
		const int
		    length = document
		                 ? std::snprintf(
			body,
			sizeof(body),
			"<!doctype html><meta charset=utf-8><title>Устройство занято</title><h1>Устройство "
			"занято</h1><p>Сохранён резерв для ответа об отказе. TCP: %u / %u, резерв: %u. Причина: "
			"%s.</p><p>Повторите загрузку через %s с или закройте лишнюю вкладку.</p>",
			state.tcp_open,
			network_policy.httpd_connections,
			network_policy.rejection_reserve,
			reason,
			retry)
		                 : std::snprintf(
			body,
			sizeof(body),
			"{\"error\":\"server_busy\",\"reason\":\"%s\",\"message\":\"Device connection limit "
			"reached; retry or close an unused "
			"tab\",\"retry_after_ms\":%u,\"httpd_open\":%u,\"httpd_limit\":%u,\"http_reserve\":%u}",
			reason,
			network_policy.retry_after_ms,
			state.tcp_open,
			network_policy.httpd_connections,
			network_policy.rejection_reserve);
		const auto result = response.send(
			503,
			document ? "text/html; charset=utf-8" : "application/json",
			{reinterpret_cast<const uint8_t *>(body), static_cast<size_t>(length)});
		// The rejection must release its reserve slot, rather than becoming keep-alive.
		return result;
	}

} // namespace lcd7_web
