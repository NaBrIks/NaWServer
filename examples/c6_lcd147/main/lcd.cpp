// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

// Waveshare ST7789T initialization parameters adapted from the vendor example
// in ESP32-BLETPMS-and-more/Example/ESP32-C6-LCD-1.47-Test.
// Uses synchronous SPI: the worker owns both the device and its bounded buffer.
#include "lcd.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace c6_demo
{
	namespace
	{
		constexpr int width = 172, height = 320, x_offset = 34;
		constexpr gpio_num_t chip_select = GPIO_NUM_14, data_command = GPIO_NUM_15, reset = GPIO_NUM_21,
		    backlight = GPIO_NUM_22;
		spi_device_handle_t device = nullptr;
		std::atomic<bool> initialized{false};
		DMA_ATTR uint8_t pixels[width * 8 * 2];
		esp_err_t transmit(bool data, const void *bytes, size_t length)
		{
			if(!length)
				return ESP_OK;

			gpio_set_level(data_command, data);
			spi_transaction_t transaction{};
			transaction.length = length * 8;
			transaction.tx_buffer = bytes;
			return spi_device_transmit(device, &transaction);
		}

		esp_err_t command(uint8_t code, std::initializer_list<uint8_t> parameters = {})
		{
			auto result = transmit(false, &code, 1);
			return result == ESP_OK ? transmit(true, parameters.begin(), parameters.size()) : result;
		}

		esp_err_t draw(unsigned pattern, unsigned frame)
		{
			constexpr uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff, 0x0000};
			for(int y = 0; y < height; y += 8)
			{
				for(int row = 0; row < 8; row++)
					for(int x = 0; x < width; x++)
					{
						uint16_t color = pattern < 5 ? colors[pattern] : colors[std::min(4, x * 5 / width)];
						if(pattern == 5)
						{
							if(x % 20 == 0 || (y + row) % 20 == 0)
								color = 0xffff;

							if(x == 0 || x == width - 1 || y + row == 0 || y + row == height - 1)
								color = 0xffff;

							if((y + row) / 8 == static_cast<int>(frame % 40))
								color = 0x07ff;
						}

						const int position = (row * width + x) * 2;
						pixels[position] = color >> 8;
						pixels[position + 1] = color;
					}
				auto result = command(0x2a, {0, x_offset, 0, x_offset + width - 1});
				if(result != ESP_OK)
					return result;

				result = command(
					0x2b,
					{static_cast<uint8_t>(y >> 8),
					 static_cast<uint8_t>(y),
					 static_cast<uint8_t>((y + 7) >> 8),
					 static_cast<uint8_t>(y + 7)});
				if(result != ESP_OK)
					return result;

				result = command(0x2c);
				if(result != ESP_OK)
					return result;

				result = transmit(true, pixels, sizeof(pixels));
				if(result != ESP_OK)
					return result;
			}
			return ESP_OK;
		}

		void worker(void *)
		{
			unsigned frame = 0;
			TickType_t wake = xTaskGetTickCount();
			for(;;)
			{
				const unsigned pattern = (frame / 20) % 6;
				const auto result = (frame % 20 == 0 || pattern == 5) ? draw(pattern, frame) : ESP_OK;
				if(frame % 20 == 0)
					ESP_LOGI(
						"lcd147",
						"pattern=%u (0:R 1:G 2:B 3:W 4:black 5:grid), SPI=%s; visual check required",
						pattern,
						esp_err_to_name(result));

				if(result != ESP_OK)
				{
					initialized = false;
					ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
					ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
					vTaskDelete(nullptr);
					return;
				}

				++frame;
				vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
			}
		}

	} // namespace
	esp_err_t lcd_start()
	{
		gpio_config_t gpio{};
		gpio.pin_bit_mask = (1ULL << data_command) | (1ULL << reset) | (1ULL << backlight) | (1ULL << GPIO_NUM_4);
		gpio.mode = GPIO_MODE_OUTPUT;
		ESP_RETURN_ON_ERROR(gpio_config(&gpio), "lcd147", "GPIO");
		gpio_set_level(backlight, 0);
		gpio_set_level(GPIO_NUM_4, 1); // SD deselected on shared SPI bus.
		spi_bus_config_t bus{};
		bus.mosi_io_num = 6;
		bus.miso_io_num = -1;
		bus.sclk_io_num = 7;
		bus.quadwp_io_num = -1;
		bus.quadhd_io_num = -1;
		bus.max_transfer_sz = sizeof(pixels);
		ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), "lcd147", "SPI bus");
		spi_device_interface_config_t configuration{};
		configuration.clock_speed_hz = 12 * 1000 * 1000;
		configuration.mode = 0;
		configuration.spics_io_num = chip_select;
		configuration.queue_size = 1;
		ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &configuration, &device), "lcd147", "SPI device");
		gpio_set_level(reset, 0);
		vTaskDelay(pdMS_TO_TICKS(20));
		gpio_set_level(reset, 1);
		vTaskDelay(pdMS_TO_TICKS(120));
		ESP_RETURN_ON_ERROR(command(0x11), "lcd147", "sleep out");
		vTaskDelay(pdMS_TO_TICKS(120));
		ESP_RETURN_ON_ERROR(command(0x36, {0x48}), "lcd147", "MADCTL");
		ESP_RETURN_ON_ERROR(command(0x3a, {0x55}), "lcd147", "RGB565");
		ESP_RETURN_ON_ERROR(command(0xb0, {0x00, 0xe8}), "lcd147", "RAM control");
		ESP_RETURN_ON_ERROR(command(0xb2, {0x0c, 0x0c, 0x00, 0x33, 0x33}), "lcd147", "porch");
		ESP_RETURN_ON_ERROR(command(0xb7, {0x75}), "lcd147", "gate");
		ESP_RETURN_ON_ERROR(command(0xbb, {0x1a}), "lcd147", "VCOM");
		ESP_RETURN_ON_ERROR(command(0xc0, {0x80}), "lcd147", "LCM");
		ESP_RETURN_ON_ERROR(command(0xc2, {0x01, 0xff}), "lcd147", "VDV enable");
		ESP_RETURN_ON_ERROR(command(0xc3, {0x13}), "lcd147", "VRH");
		ESP_RETURN_ON_ERROR(command(0xc4, {0x20}), "lcd147", "VDV");
		ESP_RETURN_ON_ERROR(command(0xc6, {0x0f}), "lcd147", "frame rate");
		ESP_RETURN_ON_ERROR(command(0xd0, {0xa4, 0xa1}), "lcd147", "power");
		ESP_RETURN_ON_ERROR(
			command(0xe0, {0xd0, 0x0d, 0x14, 0x0d, 0x0d, 0x09, 0x38, 0x44, 0x4e, 0x3a, 0x17, 0x18, 0x2f, 0x30}),
			"lcd147",
			"gamma+");
		ESP_RETURN_ON_ERROR(
			command(0xe1, {0xd0, 0x09, 0x0f, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4d, 0x38, 0x15, 0x16, 0x2c, 0x2e}),
			"lcd147",
			"gamma-");
		ESP_RETURN_ON_ERROR(command(0x21), "lcd147", "inversion");
		ESP_RETURN_ON_ERROR(draw(4, 0), "lcd147", "initial clear");
		ESP_RETURN_ON_ERROR(command(0x29), "lcd147", "display on");
		ledc_timer_config_t timer{};
		timer.speed_mode = LEDC_LOW_SPEED_MODE;
		timer.duty_resolution = LEDC_TIMER_10_BIT;
		timer.timer_num = LEDC_TIMER_0;
		timer.freq_hz = 5000;
		timer.clk_cfg = LEDC_AUTO_CLK;
		ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "lcd147", "backlight timer");
		ledc_channel_config_t channel{};
		channel.gpio_num = backlight;
		channel.speed_mode = LEDC_LOW_SPEED_MODE;
		channel.channel = LEDC_CHANNEL_0;
		channel.timer_sel = LEDC_TIMER_0;
		channel.duty = 307;
		ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), "lcd147", "30 percent backlight");
		initialized = true;
		if(xTaskCreate(worker, "lcd_test", 3072, nullptr, 1, nullptr) != pdPASS)
		{
			initialized = false;
			ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
			ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
			return ESP_ERR_NO_MEM;
		}

		return ESP_OK;
	}

	bool lcd_ready()
	{
		return initialized.load();
	}

} // namespace c6_demo
