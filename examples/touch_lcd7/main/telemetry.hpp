// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include "wire.hpp"
#include "runtime.hpp"
namespace lcd7_web
{
	void telemetry_init();
	void telemetry_collect(bool details);
	void encode_info(Writer &w);
	void encode_summary(Writer &w, unsigned clients, bool websocket);
	void encode_tasks(Writer &w, int offset, int limit);
	bool valid_snapshot(const char *snapshot);
	void encode_experiment(Writer &w, const Experiment &state);
	void encode_led(Writer &w, const Led &state);
	void encode_strip(Writer &w, const Strip &state);
	struct Counters
	{
		uint64_t http = 0, rx_frames = 0, tx_frames = 0, rx_bytes = 0, tx_bytes = 0, rejected = 0;
	};
	void count_http();
	void count_rx(size_t bytes);
	void count_tx(size_t bytes, unsigned clients);
	void count_rejected();
	size_t diagnostic_json(char *output, size_t capacity, char *id, size_t id_capacity);
} // namespace lcd7_web
