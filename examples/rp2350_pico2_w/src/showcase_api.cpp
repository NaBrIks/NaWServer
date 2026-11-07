#include "showcase_api.hpp"
#include "demo_cbor.hpp"
#include "storage.hpp"
#include "web_demo.hpp"
#include "../../common/transmission_telemetry.hpp"
#include <FreeRTOS.h>
#include <task.h>
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "pico/version.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/clocks.h"
#include "lwip/ip4_addr.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <NaWServer/detail/synchronization.h>

namespace rp_demo::showcase
{
	using namespace NaWServer;
	namespace
	{
		NaWServer::detail::Mutex mutex;
		std::array<std::uint8_t, 8192> output;
		std::array<sse::Token, sse::client_capacity> subscribers{};
		std::uint32_t sequence = 0, snapshot_id = 0;
		std::atomic<std::uint32_t> http_requests{0}, rx_frames{0}, tx_frames{0}, rx_bytes{0}, tx_bytes{0};
		char boot_id[32]{};
		std::uint64_t captured_ms = 0, previous_total = 0;
		double cpu_busy = 0;
		std::array<double, configNUMBER_OF_CORES> core_busy{};
		std::int32_t rssi = 0;
		std::size_t websocket_count = 0, sse_count = 0;
		std::uint64_t bootloader_at = 0;
		struct Task
		{
			char name[configMAX_TASK_NAME_LEN];
			UBaseType_t id, priority, affinity;
			eTaskState state;
			std::uint32_t stack;
			std::uint64_t runtime;
			double cpu;
		};
		std::array<Task, 24> task_items{};
		unsigned task_count = 0;
		struct Led
		{
			bool enabled = false, level = false;
			unsigned period_ms = 1000, duty_pct = 50;
		} led;
		struct Experiment
		{
			bool running = false, pulse = false, active = false;
			unsigned duration = 5000, on = 200, period = 1000, priority = 1, core = 1;
			std::uint64_t started = 0, elapsed = 0, iterations = 0;
			double pi = 0;
		} experiment;

		void identity()
		{
			if(!boot_id[0]) std::snprintf(boot_id, sizeof(boot_id), "pico-%lu", static_cast<unsigned long>(storage().settings().boot_count));
		}
		void sample()
		{
			const auto now = time_us_64() / 1000;
			if(captured_ms && now - captured_ms < 900) return;
			std::array<TaskStatus_t, 24> current{};
			std::uint64_t total = 0;
			const auto count = uxTaskGetSystemState(current.data(), current.size(), &total);
			if(!count) return;
			const auto previous = task_items;
			const auto previous_count = task_count;
			const auto elapsed = total - previous_total;
			core_busy.fill(previous_total ? 100 : 0);
			for(unsigned index = 0; index < count; ++index)
			{
				const auto &item = current[index];
				auto &task = task_items[index];
				std::snprintf(task.name, sizeof(task.name), "%s", item.pcTaskName);
				task.id = item.xTaskNumber;
				task.priority = item.uxCurrentPriority;
				task.affinity = item.uxCoreAffinityMask;
				task.state = item.eCurrentState;
				task.stack = item.usStackHighWaterMark * sizeof(StackType_t);
				task.runtime = item.ulRunTimeCounter;
				task.cpu = 0;
				for(unsigned old = 0; old < previous_count; ++old)
				{
					if(previous[old].id == task.id && elapsed)
						task.cpu = std::clamp(100.0 * (task.runtime - previous[old].runtime) / elapsed, 0.0, 100.0);
				}
				for(unsigned core = 0; core < core_busy.size(); ++core)
					if(item.xHandle == xTaskGetIdleTaskHandleForCore(core) && previous_total) core_busy[core] = 100 - task.cpu;
			}
			cpu_busy = (core_busy[0] + core_busy[1]) / 2;
			task_count = count;
			previous_total = total;
			captured_ms = now;
			++snapshot_id;
			identity();
		}
		void envelope(CborWriter &writer, bool websocket = false)
		{
			writer.map(websocket ? 8 : 7);
			writer.put("v", 2);
			writer.put("source", "device");
			writer.put("boot_id", boot_id);
			writer.put("seq", ++sequence);
			writer.put("captured_ms", captured_ms);
			// One extra field records the actual sample clock in native microseconds.
			writer.put("clock_us", time_us_64());
			if(websocket) writer.put("type", "summary");
			writer.value("data");
		}
		SendResult send(Response &response, CborWriter &writer)
		{
			++http_requests;
			return writer.view().empty() ? response.text(500, "CBOR capacity exceeded") : response.send(200, "application/cbor", writer.view());
		}
		void writeLed(CborWriter &writer)
		{
			writer.map(7);
			writer.put("supported", true);
			writer.put("enabled", led.enabled);
			writer.put("period_ms", led.period_ms);
			writer.put("duty_pct", led.duty_pct);
			writer.put("gpio", CYW43_WL_GPIO_LED_PIN);
			writer.put("active_low", false);
			writer.put("level", led.level);
		}
		void writeExperiment(CborWriter &writer)
		{
			writer.map(14);
			writer.put("running", experiment.running);
			writer.put("kind", experiment.pulse ? "pulse" : "pi");
			writer.put("duration_ms", experiment.duration);
			writer.put("on_ms", experiment.on);
			writer.put("period_ms", experiment.period);
			writer.put("core", experiment.core);
			writer.put("priority", experiment.priority);
			writer.put("elapsed_ms", experiment.elapsed);
			writer.put("progress_pct", std::min(100.0, 100.0 * experiment.elapsed / experiment.duration));
			writer.put("pi", experiment.pi);
			writer.put("iterations", experiment.iterations);
			writer.put("active", experiment.active);
			writer.put("sample_ms", 1000);
			writer.put("active_cores", configNUMBER_OF_CORES);
		}
		void writeSummary(CborWriter &writer)
		{
			HeapStats_t heap{};
			vPortGetHeapStats(&heap);
			writer.map(16);
			writer.put("uptime_ms", time_us_64() / 1000);
			writer.put("sample_interval_ms", 100);
			writer.value("transmission"); writer.map(examples::transmission_field_count);
			examples::transmissionFields(writer, transmissionSnapshot());
			writer.put("cpu_busy_pct", cpu_busy);
			writer.value("core_busy_pct"); writer.array(core_busy.size());
			for(const auto value : core_busy) writer.value(value);
			writer.put("cpu_sum_pct", core_busy[0] + core_busy[1]);
			writer.value("memory"); writer.array(1); writer.map(7);
			writer.put("id", "internal"); writer.put("label", "FreeRTOS heap");
			writer.put("total_bytes", configTOTAL_HEAP_SIZE);
			writer.put("free_bytes", heap.xAvailableHeapSpaceInBytes);
			writer.put("min_free_bytes", heap.xMinimumEverFreeBytesRemaining);
			writer.put("largest_block_bytes", heap.xSizeOfLargestFreeBlockInBytes);
			writer.put("allocator", "FreeRTOS heap_4");
			writer.value("rssi_dbm"); if(rssi) writer.value(rssi); else writer.null();
			writer.put("task_count", task_count);
			writer.put("ws_clients", websocket_count);
			writer.put("network_status", "connected");
			writer.value("counters"); writer.map(6);
			writer.put("http_requests", http_requests.load()); writer.put("ws_rx_frames", rx_frames.load());
			writer.put("ws_tx_frames", tx_frames.load()); writer.put("ws_rx_bytes", rx_bytes.load());
			writer.put("ws_tx_bytes", tx_bytes.load()); writer.put("rejected", 0);
			writer.value("experiment"); writeExperiment(writer);
			writer.value("led"); writeLed(writer);
			writer.value("strip"); writer.map(14);
			writer.put("driver", "spi"); writer.put("requested_driver", "spi"); writer.put("driver_error", "No RGB strip attached");
			writer.put("supported", false); writer.put("enabled", false); writer.put("gpio", -1); writer.put("count", 1);
			writer.put("mode", "solid"); writer.put("color", 0); writer.put("brightness_pct", 0); writer.put("period_ms", 1000);
			writer.value("pixels"); writer.array(1); writer.value(0);
			writer.value("available_drivers"); writer.array(0); writer.put("attached", false);
			writer.value("network"); writer.map(17);
			writer.put("tcp_pcb_limit", 12); writer.put("socket_limit", 16); writer.put("reserved_sockets", 1);
			writer.put("httpd_limit", http_connection_limit); writer.put("httpd_open", websocket_count + sse_count);
			writer.put("httpd_free", http_connection_limit -
				std::min(std::size_t(http_connection_limit), websocket_count + sse_count));
			writer.put("http_reserve", NAWSERVER_DEMO_HTTP_RESERVED_REQUESTS); writer.put("ws_limit", websocket_client_limit); writer.put("ws_clients", websocket_count);
			writer.put("sse_limit", sse::client_capacity); writer.put("sse_clients", sse_count);
			writer.put("admission_rejected", 0); writer.put("last_rejection", ""); writer.put("retry_after_ms", 1000);
			writer.put("captured_ms", captured_ms); writer.put("ready", true);
			writer.put("accounting", "Persistent WS/SSE only; synchronous HTTP is transient");
		}
		void capability(CborWriter &writer, const char *name, bool supported, const char *reason)
		{
			writer.value(name); writer.map(4);
			writer.put("supported", supported); writer.put("origin", "device");
			writer.put("device_status", supported ? "ready" : "unsupported"); writer.put("reason", reason);
		}
		void workload(void *)
		{
			for(;;)
			{
				bool running, pulse; unsigned duration, on, period; std::uint64_t started, iteration; double sum;
				{
					NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
					running = experiment.running; pulse = experiment.pulse; duration = experiment.duration;
					on = experiment.on; period = experiment.period; started = experiment.started;
					iteration = experiment.iterations; sum = experiment.pi / 4;
				}
				const auto elapsed = time_us_64() / 1000 - started;
				const bool active = running && elapsed < duration && (!pulse || elapsed % period < on);
				if(active)
				{
					const auto end = time_us_64() + 2000;
					do
					{
						for(unsigned index = 0; index < 64; ++index, ++iteration)
							sum += (iteration & 1 ? -1.0 : 1.0) / (2.0 * iteration + 1.0);
					} while(time_us_64() < end);
				}
				{
					NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
					if(experiment.running && experiment.started == started)
					{
						experiment.elapsed = std::min<std::uint64_t>(elapsed, duration);
						experiment.active = active;
						experiment.iterations = iteration;
						experiment.pi = sum * 4;
						if(elapsed >= duration) experiment.running = false;
					}
				}
				vTaskDelay(pdMS_TO_TICKS(active ? 1 : 10));
			}
		}
	}

	SendResult info(Response &response)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		identity();
		char address[16], mask[16], gateway[16], mac[24];
		cyw43_arch_lwip_begin();
		const auto &netif = cyw43_state.netif[CYW43_ITF_STA];
		ip4addr_ntoa_r(netif_ip4_addr(&netif), address, sizeof(address));
		ip4addr_ntoa_r(netif_ip4_netmask(&netif), mask, sizeof(mask));
		ip4addr_ntoa_r(netif_ip4_gw(&netif), gateway, sizeof(gateway));
		cyw43_arch_lwip_end();
		const auto *m = cyw43_state.mac;
		std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
		const auto settings = storage().settings();
		CborWriter writer(output.data(), output.size());
		writer.map(8); writer.put("v", 2); writer.put("source", "device"); writer.put("boot_id", boot_id);
		writer.value("chip"); writer.map(6);
		writer.put("model", "RP2350"); writer.put("revision_major", 2); writer.put("revision_minor", 0);
		writer.put("cores", 2); writer.put("psram", false); writer.put("active_cores", configNUMBER_OF_CORES);
		writer.value("firmware"); writer.map(7);
		writer.put("name", "NaWServer Pico 2 W"); writer.put("version", "0.2.0");
		writer.put("idf_version", "Pico SDK " PICO_SDK_VERSION_STRING); writer.put("http_backend", "tcp");
		writer.put("json_backend", "none"); writer.put("mcp_enabled", false); writer.put("board_label", settings.name);
		writer.value("details"); writer.map(6);
		writer.value("hardware"); writer.map(8);
		writer.put("chip_model", "RP2350 A2"); writer.put("architecture", "ARM Cortex-M33");
		writer.put("cpu_mhz", clock_get_hz(clk_sys) / 1000000); writer.put("cores", 2);
		writer.put("active_cores", configNUMBER_OF_CORES); writer.put("wifi_24ghz", true);
		writer.put("bluetooth_le", true); writer.put("ble_started", false);
		writer.value("flash_memory"); writer.map(8);
		writer.put("flash_bytes", flash_bytes); writer.put("hp_sram_bytes", 520 * 1024);
		writer.put("heap_internal_bytes", configTOTAL_HEAP_SIZE); writer.put("psram_bytes", 0);
		writer.put("app_partition_bytes", filesystem_offset); writer.put("LittleFS_bytes", filesystem_bytes);
		writer.put("NVS_bytes", settings_bytes); writer.put("heap_allocator", "FreeRTOS heap_4; C++/lwIP use separate newlib heap");
		writer.value("mac_addresses"); writer.map(1); writer.put("wifi_sta_mac", mac);
		writer.value("network"); writer.map(7);
		writer.put("wifi_connected", true); writer.put("wifi_mode", "STA / 2.4 GHz"); writer.put("ipv4", address);
		writer.put("netmask", mask); writer.put("gateway", gateway); writer.put("ssid", settings.wifi_ssid);
		writer.put("wifi_auth_mode", "WPA2 / WPA3");
		writer.value("board"); writer.map(4);
		writer.put("board_led_ready", true); writer.put("LED_controller", "CYW43439 GPIO0 (not RP2350 GPIO0)");
		writer.put("rgb_ready", false); writer.put("USB_BOOTSEL", "COM16: b / Flash.ps1 software reset");
		writer.value("build"); writer.map(5);
		writer.put("project", "rp2350_pico2_w"); writer.put("version", "0.2.0");
		writer.put("FreeRTOS", tskKERNEL_VERSION_NUMBER); writer.put("build_date", __DATE__); writer.put("build_time", __TIME__);
		writer.value("capabilities"); writer.map(9);
		capability(writer, "tasks", true, "FreeRTOS snapshots / microsecond runtime counters");
		capability(writer, "sse", true, "Diagnostic events at 1 Hz");
		capability(writer, "led", true, "CYW43439 on-board LED");
		capability(writer, "experiments", true, "CPU0/CPU1; workload priorities 1..2; services pinned to CPU0");
		capability(writer, "rgb", false, "No RGB strip attached");
		capability(writer, "simulator_control", false, "Physical Pico 2 W");
		capability(writer, "connections", false, "Connection listing not exposed by this demo");
		capability(writer, "mcp", false, "MCP RP adapter not implemented");
		capability(writer, "storage", storage().filesystemMounted(), "LittleFS + transactional typed NVS");
		writer.value("limits"); writer.map(5);
		writer.put("frame_bytes", 4096); writer.put("body_bytes", NAWSERVER_DEMO_HTTP_BODY_BYTES); writer.put("task_page_max", 8);
		writer.put("max_ws_clients", websocket_client_limit); writer.put("active_cpu_cores", configNUMBER_OF_CORES);
		return send(response, writer);
	}
	SendResult summary(Response &response)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		sample();
		CborWriter writer(output.data(), output.size());
		envelope(writer); writeSummary(writer);
		return send(response, writer);
	}
	SendResult tasks(Request &request, Response &response)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		auto number = [](std::string_view value)
		{
			unsigned result = 0;
			for(auto character : value)
			{
				if(character < '0' || character > '9' || result > 100000000) return unsigned(-1);
				result = result * 10 + character - '0';
			}
			return result;
		};
		const auto offset = number(request.parameter("offset").value_or("0"));
		if(!offset) sample();
		char snapshot[24]; std::snprintf(snapshot, sizeof(snapshot), "%u", unsigned(snapshot_id));
		if(const auto id = request.parameter("snapshot_id"); id && *id != snapshot) return response.text(409, "Snapshot expired");
		if(offset > task_count) return response.text(400, "Invalid task offset");
		const auto limit = std::min(8u, number(request.parameter("limit").value_or("8")));
		if(!limit) return response.text(400, "Invalid task limit");
		const auto end = std::min(task_count, offset + limit);
		CborWriter writer(output.data(), output.size());
		envelope(writer); writer.map(5);
		writer.put("snapshot_id", snapshot); writer.put("offset", offset);
		writer.value("next_offset"); if(end < task_count) writer.value(end); else writer.null();
		writer.put("total", task_count); writer.value("items"); writer.array(end - offset);
		for(unsigned index = offset; index < end; ++index)
		{
			const auto &task = task_items[index]; char identifier[24];
			std::snprintf(identifier, sizeof(identifier), "t%u", unsigned(task.id));
			writer.map(8); writer.put("id", identifier); writer.put("name", task.name);
			const char *states[]{"running", "ready", "blocked", "suspended", "deleted", "invalid"};
			writer.put("state", states[std::min(unsigned(task.state), 5u)]); writer.put("priority", task.priority);
			writer.value("core_affinity");
			if(task.affinity == 1 || task.affinity == 2) writer.value(task.affinity == 1 ? 0 : 1);
			else writer.null();
			writer.put("stack_min_free_bytes", task.stack);
			writer.put("runtime_ticks", task.runtime); writer.put("cpu_pct", task.cpu);
		}
		return send(response, writer);
	}
	SendResult echo(Request &request, Response &response)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		CborWriter writer(output.data(), output.size());
		writer.map(3); writer.put("v", 2); writer.put("method", request.method() == HttpMethod::Post ? "POST" : "GET");
		writer.put("value", request.parameter("value").value_or(std::string_view{}));
		return send(response, writer);
	}
	SendResult stream(Response &response)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		CborWriter writer(output.data(), output.size());
		writer.map(3); writer.put("v", 2); writer.put("kind", "buffered"); writer.value("items"); writer.array(16);
		for(unsigned index = 0; index < 16; ++index) { writer.map(2); writer.put("index", index); writer.put("value", index * index); }
		return send(response, writer);
	}
	SendResult command(Request &request, Response &response)
	{
		CborFields fields;
		if(!fields.parse(request.body())) return response.text(400, "Expected bounded CBOR map");
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		CborWriter writer(output.data(), output.size());
		if(request.path() == "/api/v2/control")
		{
			if(fields.text("action") != "bootloader") return response.text(400, "Unknown control action");
			bootloader_at = time_us_64() / 1000 + 500;
			writer.map(1); writer.put("ok", true);
		}
		else if(request.path() == "/api/v2/led")
		{
			const auto period = fields.integer("period_ms", led.period_ms), duty = fields.integer("duty_pct", led.duty_pct);
			if(period < 20 || period > 5000 || duty < 0 || duty > 100) return response.text(400, "LED limits: 20..5000 ms / 0..100 percent");
			led.enabled = fields.boolean("enabled", led.enabled); led.period_ms = period; led.duty_pct = duty;
			writeLed(writer);
		}
		else
		{
			if(fields.text("action") == "stop") experiment.running = false;
			else if(fields.text("action") == "start")
			{
				const auto duration = fields.integer("duration_ms", 5000), on = fields.integer("on_ms", 200);
				const auto period = fields.integer("period_ms", 1000), priority = fields.integer("priority", 1);
				const auto core = fields.integer("core", 1);
				if(duration < 1000 || duration > 30000 || period < 20 || period > 5000 || on < 1 || on > period ||
					priority < 1 || priority > 2 || core < 0 || core >= configNUMBER_OF_CORES ||
					(fields.text("kind") != "pi" && fields.text("kind") != "pulse")) return response.text(400, "RP workload: core0/1, priority1..2, duration1000..30000ms");
				if(experiment.running) return response.text(409, "Workload already running");
				experiment = {true, fields.text("kind") == "pulse", false, unsigned(duration), unsigned(on), unsigned(period), unsigned(priority), unsigned(core), time_us_64() / 1000, 0, 0, 0};
			}
			else return response.text(400, "Unknown workload action");
			writeExperiment(writer);
		}
		return send(response, writer);
	}
	SendResult openEvents(sse::EventSource &events, Request &request, Response &response)
	{
		const auto opened = events.open(request, response);
		if(opened.active)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
			subscribers[opened.token.slot] = opened.token;
			return SendResult::Ok;
		}
		return response.state() == ResponseState::External ? opened.result : response.text(503, "SSE capacity reached");
	}
	void message(WebSocketClient *client, const std::uint8_t *bytes, std::size_t size, bool binary)
	{
		++rx_frames; rx_bytes += size;
		CborFields fields;
		if(!binary || !fields.parse({bytes, size}) || fields.integer("v", 0) != 2 || fields.text("type") != "echo.request" || fields.encoded("data").empty()) return;
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		CborWriter writer(output.data(), output.size());
		writer.map(4); writer.put("v", 2); writer.put("type", "echo.response"); writer.put("request_id", fields.text("request_id"));
		writer.value("data"); writer.raw(fields.encoded("data"));
		const auto encoded = writer.view();
		if(!encoded.empty() && client->send_binary(encoded.data(), encoded.size()) == WebSocketSendResult::Queued) { ++tx_frames; tx_bytes += encoded.size(); }
	}
	void poll(sse::EventSource &events, WebSocketEndpoint &socket)
	{
		static std::uint64_t next_sample = 0;
		static TaskHandle_t workload_task = nullptr;
		if(!workload_task) (void)xTaskCreateAffinitySet(workload, "demo-workload", 768, nullptr, 1, 2, &workload_task);
		bool level, previous_level;
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
			if(bootloader_at && time_us_64() / 1000 >= bootloader_at)
			{
				watchdog_disable();
				watchdog_hw->scratch[0] = 0;
				reset_usb_boot(0, 0);
			}
			level = led.enabled && (time_us_64() / 1000 % led.period_ms) * 100 < led.period_ms * led.duty_pct;
			previous_level = led.level;
			if(workload_task && uxTaskPriorityGet(workload_task) != experiment.priority) vTaskPrioritySet(workload_task, experiment.priority);
			if(workload_task && vTaskCoreAffinityGet(workload_task) != (1u << experiment.core)) vTaskCoreAffinitySet(workload_task, 1u << experiment.core);
		}
		if(level != previous_level) { cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, level); NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex); led.level = level; }
		const auto now = time_us_64() / 1000;
		if(now < next_sample) return;
		next_sample = now + 100;
		std::int32_t current_rssi = 0;
		(void)cyw43_wifi_get_rssi(&cyw43_state, &current_rssi);
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex);
		rssi = current_rssi; websocket_count = socket.count(); sse_count = events.active(); sample();
		CborWriter writer(output.data(), output.size()); envelope(writer, true); writeSummary(writer);
		const auto encoded = writer.view();
		if(websocket_count && !encoded.empty())
		{
			const auto sent = socket.send_binary(encoded.data(), encoded.size());
			tx_frames += sent.queued; tx_bytes += sent.queued * encoded.size();
		}
		static std::uint64_t next_sse = 0;
		if(now < next_sse) return;

		next_sse = now + 1000;
		char data[320], identifier[64];
		std::snprintf(data, sizeof(data), "{\"v\":2,\"source\":\"device\",\"boot_id\":\"%s\",\"seq\":%lu,\"captured_ms\":%llu,\"cpu_busy_pct\":%.2f,\"free_bytes\":%u,\"task_count\":%u}",
			boot_id, static_cast<unsigned long>(sequence), static_cast<unsigned long long>(captured_ms), cpu_busy, unsigned(xPortGetFreeHeapSize()), task_count);
		std::snprintf(identifier, sizeof(identifier), "%s:%lu", boot_id, static_cast<unsigned long>(sequence));
		sse::Frame frame;
		if(sse::encode({data, "diagnostic", identifier, 1000}, frame) == sse::EncodeResult::Ok)
			for(const auto token : subscribers) (void)events.publish(token, frame);
	}
}
