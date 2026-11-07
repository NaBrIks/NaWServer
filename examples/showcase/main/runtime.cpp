// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "runtime.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#ifndef NAWSERVER_DEMO_LED_GPIO
#define NAWSERVER_DEMO_LED_GPIO -1
#endif
namespace showcase
{
	namespace
	{
		StaticSemaphore_t mutex_storage;
		SemaphoreHandle_t mutex;
		State state;
		TaskHandle_t bench_task, output_task;
		esp_timer_handle_t led_timer;
		led_strip_handle_t strip_handle;
		uint32_t experiment_generation = 0, output_generation = 0;
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
		int64_t now_ms()
		{
			return esp_timer_get_time() / 1000;
		}

		[[maybe_unused]] void led_wake(void *)
		{
			if(output_task)
				xTaskNotifyGive(output_task);
		}

		esp_err_t create_strip(int driver)
		{
			led_strip_config_t config{};
			config.strip_gpio_num = 2;
			config.max_leds = 10;
			config.led_model = LED_MODEL_WS2812;
			config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
			esp_err_t result;
			if(driver == 0)
			{
				led_strip_rmt_config_t rmt{};
				rmt.clk_src = RMT_CLK_SRC_DEFAULT;
				rmt.resolution_hz = 10000000;
				rmt.mem_block_symbols = 48;
				rmt.flags.with_dma = false;
				result = led_strip_new_rmt_device(&config, &rmt, &strip_handle);
			}
			else
			{
				led_strip_spi_config_t spi{};
				spi.clk_src = SPI_CLK_SRC_DEFAULT;
				spi.spi_bus = SPI2_HOST;
				spi.flags.with_dma = true;
				result = led_strip_new_spi_device(&config, &spi, &strip_handle);
			}

			if(result == ESP_OK)
			{
				result = led_strip_clear(strip_handle);
				if(result != ESP_OK)
				{
					led_strip_del(strip_handle);
					strip_handle = nullptr;
				}
			}

			return result;
		}

		void output_worker(void *)
		{
			uint32_t previous_generation = UINT32_MAX;
			int previous_mode = -1;
			int64_t previous_frame = -1;
			for(;;)
			{
				State snapshot;
				uint32_t generation;
				{
					Lock l;
					snapshot = state;
					generation = output_generation;
				}
				const int64_t now = now_ms();
				if(snapshot.led.supported)
				{
					esp_timer_stop(led_timer);
					const auto &led = snapshot.led;
					const int phase = now % led.period;
					const int on = led.period * led.duty / 100;
					bool level = led.enabled && phase < on;
					gpio_set_level(static_cast<gpio_num_t>(NAWSERVER_DEMO_LED_GPIO), level ? 0 : 1);
					{
						Lock l;
						state.led.level = level;
					}
					if(led.enabled && led.duty > 0 && led.duty < 100)
					{
						int delay = phase < on ? on - phase : led.period - phase;
						esp_timer_start_once(led_timer, std::max(1, delay) * 1000LL);
					}
				}

				// This worker alone owns the handle. Refresh is synchronous, so deletion
				// and recreation cannot race a frame or an HTTP callback.
				if(snapshot.strip.requested_driver != snapshot.strip.driver ||
				    ((!strip_handle || !snapshot.strip.supported) && generation != previous_generation))
				{
					const int previous_driver = snapshot.strip.driver, target = snapshot.strip.requested_driver;
					esp_err_t result = strip_handle ? led_strip_del(strip_handle) : ESP_OK;
					if(result == ESP_OK)
					{
						strip_handle = nullptr;
						result = create_strip(target);
						if(result == ESP_OK)
							snapshot.strip.driver = target;
						else if(target != previous_driver)
						{
							auto restored = create_strip(previous_driver);
							if(restored != ESP_OK)
								ESP_LOGE("showcase", "WS2812 fallback failed: %s", esp_err_to_name(restored));
						}
					}

					snapshot.strip.supported = strip_handle != nullptr;
					{
						Lock l;
						state.strip.driver = snapshot.strip.driver;
						state.strip.supported = snapshot.strip.supported;
						state.strip.driver_error = result;
						if(result != ESP_OK && generation == output_generation)
							state.strip.requested_driver = snapshot.strip.driver;
					}
					if(result != ESP_OK)
						ESP_LOGE("showcase", "WS2812 driver switch failed: %s", esp_err_to_name(result));

					previous_frame = -1;
				}

				if(snapshot.strip.supported && strip_handle)
				{
					strip_pixels(snapshot.strip, now);
					bool animate = snapshot.strip.enabled && snapshot.strip.mode != 0;
					int64_t frame = animate ? (snapshot.strip.mode == 1 ? now / snapshot.strip.period : now / 20) : 0;
					if(generation != previous_generation || snapshot.strip.mode != previous_mode ||
					    frame != previous_frame)
					{
						esp_err_t result = ESP_OK;
						for(int i = 0; i < 10 && result == ESP_OK; i++)
						{
							auto c = snapshot.strip.pixels[i];
							result = led_strip_set_pixel(strip_handle, i, (c >> 16) & 255, (c >> 8) & 255, c & 255);
						}
						if(result == ESP_OK)
							result = led_strip_refresh(strip_handle);

						{
							Lock l;
							if(result == ESP_OK)
								std::copy(
									std::begin(snapshot.strip.pixels),
									std::end(snapshot.strip.pixels),
									state.strip.pixels);
							else
							{
								state.strip.supported = false;
								state.strip.enabled = false;
								state.strip.driver_error = result;
							}
						}
						if(result != ESP_OK)
							ESP_LOGE("showcase", "WS2812 output failed: %s", esp_err_to_name(result));

						previous_generation = generation;
						previous_mode = snapshot.strip.mode;
						previous_frame = frame;
					}
				}

				// RMT and LED timing live here, never in HTTPD. Notification applies changes promptly.
				bool animate = snapshot.strip.supported && snapshot.strip.enabled && snapshot.strip.mode != 0;
				ulTaskNotifyTake(pdTRUE, animate ? pdMS_TO_TICKS(10) : portMAX_DELAY);
			}
		}

		void bench_worker(void *)
		{
			for(;;)
			{
				ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
				Experiment local;
				uint32_t generation;
				{
					Lock l;
					local = state.experiment;
					generation = experiment_generation;
				}
				if(!local.running)
					continue;

				vTaskPrioritySet(nullptr, local.priority);
				int64_t published = esp_timer_get_time(), last_yield = published;
				for(;;)
				{
					int64_t now = esp_timer_get_time();
					local.elapsed = std::min<int64_t>(local.duration, now / 1000 - local.start);
					bool done = local.elapsed >= local.duration;
					local.active = !done && (!local.pulse || local.elapsed % local.period < local.on);
					if(local.active)
					{
						// Bounded computation blocks; pi is published every quarter second.
						for(unsigned i = 0; i < 256; i++)
						{
							local.pi += (local.iterations % 2 ? -4.0 : 4.0) / (2.0 * local.iterations + 1);
							++local.iterations;
						}
					}

					if(done || now - published >= 250000 || local.pulse)
					{
						Lock l;
						if(generation != experiment_generation || !state.experiment.running)
							break;

						state.experiment.elapsed = local.elapsed;
						state.experiment.active = local.active;
						if(done || now - published >= 250000)
						{
							state.experiment.pi = local.pi;
							state.experiment.iterations = local.iterations;
							published = now;
						}

						if(done)
						{
							state.experiment.running = false;
							state.experiment.active = false;
							break;
						}
					}

					if(!local.active)
					{
						vTaskDelay(pdMS_TO_TICKS(1));
					}
					else if(now - last_yield >= 10000)
					{
						vTaskDelay(1);
						last_yield = esp_timer_get_time();
					}

					// A bounded yield keeps IDLE/watchdog alive even at priority 5.
					{
						Lock l;
						if(generation != experiment_generation || !state.experiment.running)
							break;
					}
				}
				vTaskPrioritySet(nullptr, 1);
			}
		}

	} // namespace
	void strip_pixels(Strip &s, int64_t milliseconds)
	{
		for(int i = 0; i < 10; i++)
		{
			uint32_t color = s.color;
			if(!s.enabled || (s.mode == 1 && i != (milliseconds / s.period) % 10))
			{
				s.pixels[i] = 0;
				continue;
			}

			if(s.mode == 2)
			{
				double h = std::fmod(i / 10.0 + milliseconds / static_cast<double>(s.period), 1.0) * 6;
				double x = 1 - std::abs(std::fmod(h, 2.0) - 1);
				double r = 0, g = 0, b = 0;
				switch(static_cast<int>(h))
				{
				case 0:
					r = 1;
					g = x;
					break;
				case 1:
					r = x;
					g = 1;
					break;
				case 2:
					g = 1;
					b = x;
					break;
				case 3:
					g = x;
					b = 1;
					break;
				case 4:
					r = x;
					b = 1;
					break;
				default:
					r = 1;
					b = x;
				}
				color = (static_cast<int>(r * 255) << 16) | (static_cast<int>(g * 255) << 8) |
				    static_cast<int>(b * 255);
			}

			s.pixels[i] = ((((color >> 16) & 255) * s.brightness / 100) << 16) |
			    ((((color >> 8) & 255) * s.brightness / 100) << 8) | ((color & 255) * s.brightness / 100);
		}
	}

	State runtime_state()
	{
		Lock l;
		auto copy = state;
		if(copy.experiment.running)
		{
			copy.experiment.elapsed = std::min<int64_t>(copy.experiment.duration, now_ms() - copy.experiment.start);
			copy.experiment.active = !copy.experiment.pulse ||
			    copy.experiment.elapsed % copy.experiment.period < copy.experiment.on;
		}

		return copy;
	}

	bool set_experiment(const ExperimentCommand &c)
	{
		Lock l;
		if(c.stop)
		{
			state.experiment.running = false;
			state.experiment.active = false;
			++experiment_generation;
			return true;
		}

		if(state.experiment.running || c.core != 0 || c.priority < 1 || c.priority > 5 || c.duration < 1000 ||
		    c.duration > 30000 || c.period < 20 || c.period > 5000 || c.on < 1 || c.on > c.period)
			return false;

		state.experiment = {};
		auto &e = state.experiment;
		e.running = true;
		e.pulse = c.pulse;
		e.duration = c.duration;
		e.on = c.on;
		e.period = c.period;
		e.priority = c.priority;
		e.core = c.core;
		e.start = now_ms();
		++experiment_generation;
		xTaskNotifyGive(bench_task);
		return true;
	}

	bool set_led(bool enabled, int period, int duty)
	{
		Lock l;
		if(!state.led.supported || period < 20 || period > 5000 || duty < 0 || duty > 100)
			return false;

		state.led.enabled = enabled;
		state.led.period = period;
		state.led.duty = duty;
		++output_generation;
		xTaskNotifyGive(output_task);
		return true;
	}

	bool set_strip(bool enabled, int mode, int color, int brightness, int period, int driver)
	{
		Lock l;
		if(mode < 0 || mode > 2 || color < 0 || color > 0xffffff || brightness < 0 || brightness > 100 || period < 20 ||
		    period > 5000 || driver < -1 || driver > 1)
			return false;

		state.strip.enabled = enabled;
		state.strip.mode = mode;
		state.strip.color = color;
		state.strip.brightness = brightness;
		state.strip.period = period;
		if(driver >= 0)
			state.strip.requested_driver = driver;

		++output_generation;
		xTaskNotifyGive(output_task);
		return true;
	}

	esp_err_t runtime_init()
	{
		mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
		if(!mutex)
			return ESP_ERR_NO_MEM;

#if NAWSERVER_DEMO_LED_GPIO >= 0
		{
			gpio_config_t config{};
			config.pin_bit_mask = 1ULL << static_cast<unsigned>(NAWSERVER_DEMO_LED_GPIO);
			config.mode = GPIO_MODE_OUTPUT;
			ESP_ERROR_CHECK(gpio_config(&config));
			gpio_set_level(static_cast<gpio_num_t>(NAWSERVER_DEMO_LED_GPIO), 1);
			esp_timer_create_args_t args{};
			args.callback = led_wake;
			args.name = "showcase_led";
			ESP_ERROR_CHECK(esp_timer_create(&args, &led_timer));
			state.led.supported = true;
		}
#endif
		auto result = create_strip(0);
		state.strip.supported = result == ESP_OK;
		state.strip.driver_error = result;
		if(result != ESP_OK)
			ESP_LOGE("showcase", "WS2812 not ready: %s", esp_err_to_name(result));

		if(xTaskCreatePinnedToCore(bench_worker, "bench_worker", 4096, nullptr, 1, &bench_task, 0) != pdPASS)
			return ESP_ERR_NO_MEM;

		if(xTaskCreatePinnedToCore(output_worker, "showcase_io", 4096, nullptr, 4, &output_task, 0) != pdPASS)
			return ESP_ERR_NO_MEM;

		return ESP_OK;
	}

} // namespace showcase
