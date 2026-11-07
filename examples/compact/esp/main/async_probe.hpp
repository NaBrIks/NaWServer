// SPDX-License-Identifier: MIT
#pragma once

#include <NaWServer/response.h>
#include <NaWServer/pipeline.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>

namespace compact::readme
{

	// Optional application workload, outside the HTTP/TX workers. The timer only
	// notifies: arithmetic runs with normal interrupts and preemption enabled.
	class AsyncProbe
	{
		static constexpr std::uint64_t period_us = 10000;
		static constexpr std::array<unsigned, 13> bounds{50, 100, 200, 500, 1000, 2000, 3000, 5000, 8000, 10000, 20000, 50000, 100000};
		struct Metrics
		{
			unsigned epoch = 0, cycles = 0, misses = 0, skipped = 0, late_max = 0, work_max = 0;
			std::array<unsigned, bounds.size() + 1> histogram{};
		};
		struct Slot
		{
			AsyncProbe *owner = nullptr;
			unsigned core = 0;
			std::atomic<unsigned> iterations_per_percent{0};
			TaskHandle_t task = nullptr;
			esp_timer_handle_t timer = nullptr;
			std::atomic<std::uint64_t> origin{0};
			Metrics metrics;
		};
		std::array<Slot, configNUM_CORES> slots_{};
		std::atomic<unsigned> requested_{0};
		portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;

		static unsigned arithmetic(unsigned count, unsigned word)
		{
			for(unsigned i = 0; i < count; ++i)
			{
				word ^= word << 13; word ^= word >> 17; word ^= word << 5;
				word = word * 1664525u + 1013904223u;
			}
			return word;
		}

		static void timer(void *context)
		{
			auto &slot = *static_cast<Slot *>(context);
			xTaskNotifyGive(slot.task);
		}

		static void task(void *context)
		{
			auto &slot = *static_cast<Slot *>(context);
			auto &self = *slot.owner;
			unsigned word = 1234567u + slot.core;
			volatile unsigned checksum = word;
			const auto calibration_start = esp_timer_get_time();
			word = arithmetic(16000, word);
			checksum = word;
			const auto elapsed = std::max<std::int64_t>(1, esp_timer_get_time() - calibration_start);
			slot.iterations_per_percent.store(unsigned(16000ull * (period_us / 100) / elapsed));
			// A volatile sink prevents the optimizer from removing the calibrated job.
			(void)checksum;
			std::uint64_t sequence = 0;
			unsigned previous = ~0u;
			for(;;)
			{
				const auto notifications = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
				sequence += notifications;
				const auto configuration = self.requested_.load();
				const auto mode = configuration & 255u;
				const unsigned selected_core = mode == 4 ? 1 : 0;
				const unsigned priority = slot.core == selected_core && mode >= 3 ? 6 : 1;
				const unsigned duty = mode == 0 ? 0 : mode == 1 ? 50 : 80;
				if(configuration != previous)
				{
					vTaskPrioritySet(nullptr, priority);
					portENTER_CRITICAL(&self.mutex_);
					slot.metrics = {}; slot.metrics.epoch = configuration >> 8;
					portEXIT_CRITICAL(&self.mutex_);
					previous = configuration;
				}
				const auto due = slot.origin.load() + sequence * period_us;
				const auto started = std::uint64_t(esp_timer_get_time());
				const auto lateness = unsigned(started > due ? std::min<std::uint64_t>(started - due, UINT32_MAX) : 0);
				if(slot.core == selected_core && duty)
				{
					// The same iteration count follows the workload to another core.
					word = arithmetic(self.slots_[0].iterations_per_percent.load() * duty, word);
					checksum = word;
				}
				const auto finished = std::uint64_t(esp_timer_get_time());
				const auto work = unsigned(std::min<std::uint64_t>(finished - started, UINT32_MAX));
				const auto bucket = std::lower_bound(bounds.begin(), bounds.end(), lateness) - bounds.begin();
				portENTER_CRITICAL(&self.mutex_);
				auto &metrics = slot.metrics;
				++metrics.cycles; ++metrics.histogram[bucket];
				metrics.skipped += notifications - 1;
				if(finished > due + period_us) ++metrics.misses;
				metrics.late_max = std::max(metrics.late_max, lateness);
				metrics.work_max = std::max(metrics.work_max, work);
				portEXIT_CRITICAL(&self.mutex_);
			}
		}

	public:
		bool start()
		{
			for(unsigned core = 0; core < slots_.size(); ++core)
			{
				auto &slot = slots_[core]; slot.owner = this; slot.core = core;
				if(xTaskCreatePinnedToCore(task, core ? "app_probe_1" : "app_probe_0", 3072, &slot, 1, &slot.task, core) != pdPASS) return false;
				esp_timer_create_args_t arguments{};
				arguments.callback = timer; arguments.arg = &slot; arguments.name = core ? "probe_1" : "probe_0";
				if(esp_timer_create(&arguments, &slot.timer) != ESP_OK) return false;
				slot.origin.store(esp_timer_get_time());
				if(esp_timer_start_periodic(slot.timer, period_us) != ESP_OK) return false;
			}
			return true;
		}

		bool configure(unsigned mode)
		{
			if(mode > 4 || (mode == 4 && slots_.size() < 2)) return false;
			const auto epoch = (requested_.load() >> 8) + 1;
			requested_.store((epoch << 8) | mode);
			return true;
		}

		NaWServer::Result snapshot(NaWServer::Context &context)
		{
			std::array<Metrics, configNUM_CORES> values;
			portENTER_CRITICAL(&mutex_);
			for(unsigned i = 0; i < slots_.size(); ++i) values[i] = slots_[i].metrics;
			portEXIT_CRITICAL(&mutex_);
			char bytes[1024];
			const auto requested = requested_.load();
			std::size_t used = std::snprintf(bytes, sizeof(bytes), "{\"mode\":%u,\"epoch\":%u,\"period_us\":10000,\"http_priority\":5,\"sse_priority\":4,\"cores\":[", requested & 255u, requested >> 8);
			for(unsigned i = 0; i < slots_.size(); ++i)
			{
				const auto &value = values[i];
				unsigned cumulative = 0, percentile = 0;
				for(unsigned j = 0; j < value.histogram.size(); ++j)
				{
					cumulative += value.histogram[j];
					if(cumulative * 100ull >= value.cycles * 99ull)
					{ percentile = j < bounds.size() ? bounds[j] : UINT32_MAX; break; }
				}
				const auto count = std::snprintf(bytes + used, sizeof(bytes) - used,
				    "%s{\"core\":%u,\"epoch\":%u,\"cycles\":%u,\"misses\":%u,\"skipped\":%u,\"late_p99_bound_us\":%u,\"late_max_us\":%u,\"work_max_us\":%u,\"iterations_per_percent\":%u}",
				    i ? "," : "", i, value.epoch, value.cycles, value.misses, value.skipped, percentile, value.late_max, value.work_max, slots_[i].iterations_per_percent.load());
				if(count < 0 || std::size_t(count) >= sizeof(bytes) - used) return NaWServer::Result::Error;
				used += count;
			}
			bytes[used++] = ']'; bytes[used++] = '}';
			return NaWServer::responseResult(context.response.send(200, "application/json", {reinterpret_cast<const std::uint8_t *>(bytes), used}));
		}
	};

	inline AsyncProbe &applicationProbe()
	{
		static AsyncProbe value;
		return value;
	}

}
