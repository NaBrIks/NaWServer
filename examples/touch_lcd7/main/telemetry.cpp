// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "telemetry.hpp"
#include "../../common/memory_usage.hpp"
#include <NaWServer/mcp_config.h>
#if NAWSERVER_MCP
#include <NaWServer/json.h>
#endif
#include "device_details.hpp"
#include "network.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#if !CONFIG_IDF_TARGET_ESP32S3
#error Touch LCD adapter requires ESP32-S3
#endif
#include "application.hpp"
namespace lcd7_web
{
	namespace
	{
		struct Task
		{
			uint32_t id = 0;
			char name[configMAX_TASK_NAME_LEN]{};
			eTaskState state = eInvalid;
			unsigned priority = 0, stack = 0;
			int core = -1;
			uint64_t runtime = 0;
			uint32_t previous = 0;
			double cpu = 0;
		};
		struct Snapshot
		{
			Task tasks[32];
			unsigned count = 0, total = 0;
			uint64_t captured = 0, seq = 0;
			uint32_t previous_total = 0;
			double cpu = 0;
			double cores[2]{};
			bool valid = false;
			char id[64]{};
		};
		StaticSemaphore_t storage;
		SemaphoreHandle_t mutex;
		Snapshot current;
		char boot[24]{};
		Counters counters;
		portMUX_TYPE counter_lock = portMUX_INITIALIZER_UNLOCKED;
		TaskStatus_t task_status[32];
		Task task_copy[32];
		class Lock
		{
		public:
			Lock()
			{
				xSemaphoreTake(mutex, portMAX_DELAY);
			}

			~Lock()
			{
				xSemaphoreGive(mutex);
			}

		};
		void envelope(Writer &w, uint64_t seq, uint64_t captured, bool ws)
		{
			w.map(ws ? 7 : 6);
			w.put("v", 2);
			if(ws)
				w.put("type", "summary");

			w.put("source", "device");
			w.put("boot_id", boot);
			w.put("seq", seq);
			w.put("captured_ms", captured);
			w.key("data");
		}

		const char *task_state(eTaskState s)
		{
			switch(s)
			{
			case eRunning:
				return "running";
			case eReady:
				return "ready";
			case eBlocked:
				return "blocked";
			case eSuspended:
				return "suspended";
			case eDeleted:
				return "deleted";
			default:
				return "unknown";
			}
		}

		void capability(Writer &w, std::string_view key, bool supported, const char *reason = nullptr)
		{
			w.key(key);
			w.map(reason ? 4 : 3);
			w.put("supported", supported);
			w.put("origin", "device");
			w.put("device_status", supported ? "implemented_unverified" : "unsupported");
			if(reason)
				w.put("reason", reason);

			w.end();
		}

	} // namespace
	void count_http()
	{
		portENTER_CRITICAL(&counter_lock);
		++counters.http;
		portEXIT_CRITICAL(&counter_lock);
	}

	void count_rx(size_t n)
	{
		portENTER_CRITICAL(&counter_lock);
		++counters.rx_frames;
		counters.rx_bytes += n;
		portEXIT_CRITICAL(&counter_lock);
	}

	void count_tx(size_t n, unsigned clients)
	{
		portENTER_CRITICAL(&counter_lock);
		counters.tx_frames += clients;
		counters.tx_bytes += n * clients;
		portEXIT_CRITICAL(&counter_lock);
	}

	void count_rejected()
	{
		portENTER_CRITICAL(&counter_lock);
		++counters.rejected;
		portEXIT_CRITICAL(&counter_lock);
	}

	size_t diagnostic_json(char *output, size_t capacity, char *id, size_t id_capacity)
	{
		Lock lock;
		std::snprintf(id, id_capacity, "%s:%llu", boot, static_cast<unsigned long long>(current.seq));
		const auto length = std::snprintf(
			output,
			capacity,
			"{\"v\":2,\"source\":\"device\",\"boot_id\":\"%s\",\"seq\":%llu,\"captured_ms\":%llu,\"cpu_busy_pct\":%.1f,"
			"\"free_bytes\":%u,\"task_count\":%u}",
			boot,
			static_cast<unsigned long long>(current.seq),
			static_cast<unsigned long long>(current.captured),
			current.cpu,
			static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
			current.total);
		return length > 0 && static_cast<size_t>(length) < capacity ? static_cast<size_t>(length) : 0;
	}

	void telemetry_init()
	{
		mutex = xSemaphoreCreateMutexStatic(&storage);
		std::snprintf(
			boot,
			sizeof(boot),
			"s3-%08lx-%08lx",
			static_cast<unsigned long>(esp_random()),
			static_cast<unsigned long>(esp_random()));
		telemetry_collect(true);
	}

	void telemetry_collect(bool details)
	{
		static uint32_t previous_idle[2]{}, previous_wall = 0;
		const uint32_t wall = static_cast<uint32_t>(esp_timer_get_time()), elapsed = wall - previous_wall;
		double core_cpu[2]{};
		for(int core = 0; core < 2; core++)
		{
			TaskStatus_t idle_status{};
			vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core), &idle_status, pdFALSE, eInvalid);
			const uint32_t idle = idle_status.ulRunTimeCounter;
			core_cpu[core] = previous_wall && elapsed
			                     ? std::clamp(
				100.0 *
				(1 - static_cast<double>(static_cast<uint32_t>(idle - previous_idle[core])) /
				elapsed),
				0.0,
				100.0)
			                     : 0;
			previous_idle[core] = idle;
		}
		previous_wall = wall;
		const double cpu = (core_cpu[0] + core_cpu[1]) / 2;
		unsigned count = 0, total_tasks = uxTaskGetNumberOfTasks();
		configRUN_TIME_COUNTER_TYPE total_runtime = 0;
		if(details)
		{
			// Tasks owned by this demo are persistent; copy SDK task-name views immediately.
			vTaskSuspendAll();
			count = uxTaskGetSystemState(task_status, 32, &total_runtime);
			for(unsigned i = 0; i < count; i++)
			{
				auto &source = task_status[i];
				auto &target = task_copy[i];
				target = {};
				target.id = source.xTaskNumber;
				std::strncpy(target.name, source.pcTaskName, sizeof(target.name) - 1);
				target.state = source.eCurrentState;
				target.core = source.xCoreID;
				target.priority = source.uxCurrentPriority;
				target.stack = source.usStackHighWaterMark;
				target.previous = source.ulRunTimeCounter;
			}
			xTaskResumeAll();
		}

		Lock l;
		current.cores[0] = core_cpu[0];
		current.cores[1] = core_cpu[1];
		current.cpu = cpu;
		current.total = total_tasks;
		current.captured = esp_timer_get_time() / 1000;
		++current.seq;
		if(details)
		{
			uint32_t dt = static_cast<uint32_t>(total_runtime) - current.previous_total;
			for(unsigned i = 0; i < count; i++)
			{
				auto &task = task_copy[i];
				const Task *previous = nullptr;
				for(unsigned j = 0; j < current.count; j++)
					if(current.tasks[j].id == task.id)
					{
						previous = &current.tasks[j];
						break;
					}

				uint32_t ticks = previous ? task.previous - previous->previous : 0;
				task.runtime = previous ? previous->runtime + ticks : task.previous;
				task.cpu = current.valid && previous && dt ? std::clamp(100.0 * ticks / (2.0 * dt), 0.0, 100.0) : -1;
			}
			std::copy(task_copy, task_copy + count, current.tasks);
			current.count = count;
			current.previous_total = total_runtime;
			current.valid = count > 0;
			// Task snapshot remains stable between 1 Hz collections while summary updates at 10 Hz.
			std::snprintf(
				current.id,
				sizeof(current.id),
				"%s:%llu",
				boot,
				static_cast<unsigned long long>(current.seq));
		}
	}

	void encode_experiment(Writer &w, const Experiment &e)
	{
		w.map(14);
		w.put("running", e.running);
		w.put("kind", e.pulse ? "pulse" : "pi");
		w.put("duration_ms", e.duration);
		w.put("on_ms", e.on);
		w.put("period_ms", e.period);
		w.put("core", e.core);
		w.put("priority", e.priority);
		w.put("elapsed_ms", e.elapsed);
		w.put("progress_pct", 100.0 * e.elapsed / e.duration);
		w.put("pi", e.pi);
		w.put("iterations", e.iterations);
		w.put("active", e.active);
		w.put("sample_ms", 100);
		w.put("enabled", e.running);
		w.end();
	}

	void encode_led(Writer &w, const Led &e)
	{
		w.map(8);
		w.put("supported", e.supported);
		w.put("enabled", e.enabled);
		w.put("period_ms", e.period);
		w.put("duty_pct", e.duty);
		w.put("gpio", -1);
		w.put("active_low", true);
		w.put("level", e.level);
		w.put("reason", e.supported ? "" : "board_led_not_confirmed");
		w.end();
	}

	void encode_strip(Writer &w, const Strip &e)
	{
		w.map(12);
		w.put("driver", e.driver == 1 ? "spi" : "rmt");
		w.put("requested_driver", e.requested_driver == 1 ? "spi" : "rmt");
		w.put("driver_error", e.driver_error == ESP_OK ? "" : esp_err_to_name(e.driver_error));
		w.put("supported", e.supported);
		w.put("enabled", e.enabled);
		w.put("gpio", 6);
		w.put("count", 10);
		w.put("mode", e.mode == 1 ? "chase" : e.mode == 2 ? "rainbow" : "solid");
		w.put("color", e.color);
		w.put("brightness_pct", e.brightness);
		w.put("period_ms", e.period);
		w.key("pixels");
		w.array(10);
		for(auto c : e.pixels)
			w.value(c);
		w.end();
		w.end();
	}

	void encode_info(Writer &w)
	{
		auto state = runtime_state();
		esp_chip_info_t chip{};
		esp_chip_info(&chip);
		const auto *app = esp_app_get_description();
		w.map(8);
		w.put("v", 2);
		w.put("source", "device");
		w.put("boot_id", boot);
		w.key("chip");
		w.map(6);
		w.put("model", "ESP32-S3");
		w.put("revision_major", chip.revision / 100);
		w.put("revision_minor", chip.revision % 100);
		w.put("cores", chip.cores);
		w.put("psram", true);
		w.put("features", chip.features);
		w.end();
		w.key("firmware");
		w.map(13);
		w.put("name", app->project_name);
		w.put("version", app->version);
		w.put("idf_version", app->idf_ver);
		// This identifies MCP JSON processing, independently of CBOR telemetry or SSE text.
#if NAWSERVER_MCP
		w.put("json_backend", NaWServer::json::backendName());
		// These are the demo's actual compile-time reservations/request limit,
		// not a claim about third-party transport or whole-firmware allocations.
		w.put("json_workspace_bytes", static_cast<std::uint32_t>(NAWSERVER_JSON_WORKSPACE_BYTES));
		w.put("mcp_output_bytes", static_cast<std::uint32_t>(NAWSERVER_MCP_OUTPUT_BYTES));
		w.put("mcp_input_bytes", static_cast<std::uint32_t>(NAWSERVER_JSON_INPUT_BYTES));
#else
		w.put("json_backend", "none");
		w.put("json_workspace_bytes", 0);
		w.put("mcp_output_bytes", 0);
		w.put("mcp_input_bytes", 0);
#endif
		w.put("mcp_enabled", NAWSERVER_MCP != 0);
		w.put("http_backend", "tcp");
		w.put("board_label", "Waveshare ESP32-S3-Touch-LCD-7");
		w.put("build_date", app->date);
		w.put("build_time", app->time);
		w.put("reset_reason", static_cast<int>(esp_reset_reason()));
		w.end();
		w.key("capabilities");
		w.map(9);
		capability(w, "tasks", true);
		capability(w, "memory", true);
		capability(w, "experiments", true);
		capability(w, "led", state.led.supported, "board_led_not_confirmed");
		capability(
			w,
			"strip",
			state.strip.supported,
			state.strip.supported ? nullptr : "external_RGB_driver_not_ready");
		capability(w, "connections", false, "session_details_not_exposed");
		capability(w, "simulator_control", false, "device");
		capability(w, "sse", true);
		capability(w, "authenticated_ws", false, "pre_handshake_auth_unavailable");
		w.end();
		w.key("limits");
		w.map(4);
		w.put("frame_bytes", 4096);
		w.put("body_bytes", 4096);
		w.put("task_page_max", 8);
		w.put("max_ws_clients", network_policy.websocket_clients);
		w.end();
		w.key("details");
		encode_device_details(w);
		w.end();
	}

	void encode_display(Writer &w)
	{
		const auto s = touch_lcd7::snapshot();
		w.key("display");
		w.map(15);
		w.put("ready", s.ready);
		w.put("width", 800);
		w.put("height", 480);
		w.put("pattern", s.pattern);
		w.put("backlight", s.backlight);
		w.put("pressed", s.touch.pressed);
		w.put("x", s.touch.x);
		w.put("y", s.touch.y);
		w.put("touches", s.touches);
		w.put("touch_errors", s.touch_errors);
		w.put("renderer", "lvgl-8.3.11");
		w.put("framebuffers", 2);
		w.put("frame_swaps", s.graphics.swaps);
		w.put("frame_events", s.graphics.frame_events);
		w.put("vsyncs", s.graphics.vsyncs);
		w.end();
	}

	void encode_summary(Writer &w, unsigned clients, bool ws)
	{
		auto runtime = runtime_state();
		Lock l;
		Counters c;
		portENTER_CRITICAL(&counter_lock);
		c = counters;
		portEXIT_CRITICAL(&counter_lock);
		envelope(w, current.seq, current.captured, ws);
		w.map(16);
		w.put("uptime_ms", current.captured);
		w.put(
			"sample_interval_ms",
			runtime.experiment.running || (runtime.strip.enabled && runtime.strip.mode) ||
			touch_lcd7::snapshot().touch.pressed
			    ? 100
			    : 1000);
		w.put("cpu_busy_pct", current.cpu);
		w.key("core_busy_pct");
		w.array(2);
		w.value(current.cores[0]);
		w.value(current.cores[1]);
		w.end();
		constexpr unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
		demo_memory::encode(w);
		w.key("memory");
		w.array(2);
		w.map(6);
		w.put("id", "internal");
		w.put("label", "Внутренняя RAM");
		w.put("total_bytes", heap_caps_get_total_size(caps));
		w.put("free_bytes", heap_caps_get_free_size(caps));
		w.put("min_free_bytes", heap_caps_get_minimum_free_size(caps));
		w.put("largest_block_bytes", heap_caps_get_largest_free_block(caps));
		w.end();
		w.map(6);
		w.put("id", "psram");
		w.put("label", "PSRAM");
		w.put("total_bytes", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
		w.put("free_bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
		w.put("min_free_bytes", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
		w.put("largest_block_bytes", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
		w.end();
		w.end();
		wifi_ap_record_t ap{};
		bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
		w.key("rssi_dbm");
		if(connected)
			w.value(static_cast<int>(ap.rssi));
		else
			w.null();

		w.put("task_count", current.total);
		w.put("ws_clients", clients);
		w.put("network_status", connected ? "connected" : "disconnected");
		w.key("counters");
		w.map(6);
		w.put("http_requests", c.http);
		w.put("ws_rx_frames", c.rx_frames);
		w.put("ws_tx_frames", c.tx_frames);
		w.put("ws_rx_bytes", c.rx_bytes);
		w.put("ws_tx_bytes", c.tx_bytes);
		w.put("rejected", c.rejected);
		w.end();
		w.key("experiment");
		encode_experiment(w, runtime.experiment);
		w.key("led");
		encode_led(w, runtime.led);
		w.key("strip");
		encode_strip(w, runtime.strip);
		encode_display(w);
		w.key("network");
		encode_network(w);
		w.end();
		w.end();
	}

	bool valid_snapshot(const char *id)
	{
		Lock l;
		return !id[0] || std::strcmp(id, current.id) == 0;
	}

	void encode_tasks(Writer &w, int offset, int limit)
	{
		Lock l;
		envelope(w, current.seq, current.captured, false);
		unsigned start = std::min<unsigned>(offset, current.count),
		    end = std::min<unsigned>(start + limit, current.count);
		w.map(5);
		w.put("snapshot_id", current.id);
		w.put("offset", offset);
		w.key("next_offset");
		if(end < current.count)
			w.value(end);
		else
			w.null();

		w.put("total", current.count);
		w.key("items");
		w.array(end - start);
		for(unsigned i = start; i < end; i++)
		{
			const auto &t = current.tasks[i];
			char id[24];
			std::snprintf(id, sizeof(id), "task-%lu", static_cast<unsigned long>(t.id));
			w.map(8);
			w.put("id", id);
			w.put("name", t.name);
			w.put("state", task_state(t.state));
			w.put("priority", t.priority);
			w.key("core_affinity");
			if(t.core >= 0 && t.core < 2)
				w.value(t.core);
			else
				w.null();

			w.put("stack_min_free_bytes", t.stack);
			w.put("runtime_ticks", t.runtime);
			w.key("cpu_pct");
			if(t.cpu < 0)
				w.null();
			else
				w.value(t.cpu);

			w.end();
		}
		w.end();
		w.end();
		w.end();
	}

} // namespace lcd7_web
