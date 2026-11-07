// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "application.hpp"
#include "graphics.hpp"
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_netif.h"
namespace touch_lcd7
{
	static State state;
	static StaticSemaphore_t mutex_storage;
	static SemaphoreHandle_t mutex;
	static StaticQueue_t queue_storage;
	static uint8_t queue_bytes[8 * sizeof(Command)];
	static QueueHandle_t commands;
	static void publish(const State &value)
	{
		xSemaphoreTake(mutex, portMAX_DELAY);
		state = value;
		xSemaphoreGive(mutex);
	}

	State snapshot()
	{
		xSemaphoreTake(mutex, portMAX_DELAY);
		const auto copy = state;
		xSemaphoreGive(mutex);
		return copy;
	}

	bool submit(Command command)
	{
		return commands && xQueueSend(commands, &command, 0) == pdTRUE;
	}

	static void update_network_address()
	{
		char address[16]{};
		auto *interface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
		esp_netif_ip_info_t info{};
		if(interface && esp_netif_is_netif_up(interface) && esp_netif_get_ip_info(interface, &info) == ESP_OK &&
		    info.ip.addr)
		{
			std::snprintf(address, sizeof(address), IPSTR, IP2STR(&info.ip));
		}

		display_network_address(address);
	}

	static void worker(void *)
	{
		State current;
		const auto result = initialize_board();
		if(result != ESP_OK)
		{
			ESP_LOGE("touch_lcd7", "Board initialization failed: %s", esp_err_to_name(result));
			publish(current);
			vTaskDelete(nullptr);
			return;
		}

		current.ready = true;
		draw_screen(current.pattern, current.touch, current.touches);
		process_graphics();
		current.graphics = graphics_stats();
		publish(current);
		TickType_t network_sample = xTaskGetTickCount();
		for(;;)
		{
			bool redraw = false;
			Command command;
			// Bound work per iteration, so a busy browser cannot starve touch sampling.
			for(unsigned i = 0; i < 8 && xQueueReceive(commands, &command, 0) == pdTRUE; ++i)
			{
				if(command <= Command::Blue)
				{
					current.pattern = static_cast<unsigned>(command);
					redraw = true;
				}
				else
				{
					const bool enabled = command == Command::BacklightOn;
					if(set_backlight(enabled) == ESP_OK)
						current.backlight = enabled;
				}
			}
			Touch touch;
			const auto touch_result = read_touch(touch);
			if(touch_result != ESP_OK)
				++current.touch_errors;
			else
			{
				if(touch.pressed && !current.touch.pressed)
				{
					++current.touches;
					if(touch.y < 100)
						current.pattern = (current.pattern + 1) % 5;
				}

				redraw = redraw || touch.pressed != current.touch.pressed ||
				    (touch.pressed && (touch.x != current.touch.x || touch.y != current.touch.y));
				current.touch = touch;
			}

			if(redraw)
				draw_screen(current.pattern, current.touch, current.touches);

			if(xTaskGetTickCount() - network_sample >= pdMS_TO_TICKS(1000))
			{
				network_sample = xTaskGetTickCount();
				update_network_address();
			}

			process_graphics();
			current.graphics = graphics_stats();
			publish(current);
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	bool start_application()
	{
		mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
		commands = xQueueCreateStatic(8, sizeof(Command), queue_bytes, &queue_storage);
		return mutex && commands && xTaskCreate(worker, "lcd7", 6144, nullptr, 3, nullptr) == pdPASS;
	}

} // namespace touch_lcd7
