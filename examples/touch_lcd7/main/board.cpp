// SPDX-License-Identifier: MIT AND CC0-1.0
// SPDX-FileCopyrightText: 2026 Oleg Kozhevnikov
// SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
// Board reset/init parameters follow the CC0 Waveshare/Espressif example.
// See docs/readme_device_ru.md and the root THIRD_PARTY_NOTICES.md.

#include "board.hpp"
#include "graphics.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace touch_lcd7
{
	static constexpr int width = 800, height = 480;
	// Favor RGB stability alongside HTTP/Wi-Fi over refresh rate in this demo.
	// Espressif's RGB drift guidance: lower PCLK and increase the SRAM bounce buffer.
	// 16 rows divide 480 exactly; two RGB565 buffers use 51200 B (+19200 B vs 10 rows).
	static constexpr int pixel_clock_hz = 12000000, bounce_buffer_rows = 16;
	static_assert(height % bounce_buffer_rows == 0, "Whole bounce buffers per frame required");
	static esp_lcd_panel_handle_t panel;
	static esp_lcd_touch_handle_t touch_controller;

	// Wiring/timings: Waveshare ESP32-S3-Touch-LCD-7 (not 7B).
	// Source references and hardware limits are documented in docs/readme_device_ru.md.
	static esp_err_t expander_write(uint8_t address, uint8_t value)
	{
		return i2c_master_write_to_device(I2C_NUM_0, address, &value, 1, pdMS_TO_TICKS(100));
	}

	esp_err_t set_backlight(bool enabled)
	{
		// Preserve TP reset, LCD reset, SD select and USB selector levels.
		return expander_write(0x38, enabled ? 0x1e : 0x1a);
	}

	esp_err_t initialize_board()
	{
		i2c_config_t bus{};
		bus.mode = I2C_MODE_MASTER;
		bus.sda_io_num = 8;
		bus.scl_io_num = 9;
		bus.sda_pullup_en = GPIO_PULLUP_ENABLE;
		bus.scl_pullup_en = GPIO_PULLUP_ENABLE;
		bus.master.clk_speed = 400000;
		auto result = i2c_param_config(I2C_NUM_0, &bus);
		if(result != ESP_OK)
			return result;

		result = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
		if(result != ESP_OK)
			return result;

		result = expander_write(0x24, 0x01);
		if(result != ESP_OK)
			return result;

		// Vendor GT911 reset/address-selection sequence: EXIO1 reset, GPIO4 interrupt.
		result = gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
		if(result != ESP_OK)
			return result;

		result = expander_write(0x38, 0x2c);
		if(result != ESP_OK)
			return result;

		vTaskDelay(pdMS_TO_TICKS(100));
		gpio_set_level(GPIO_NUM_4, 0);
		vTaskDelay(pdMS_TO_TICKS(100));
		result = expander_write(0x38, 0x2e);
		if(result != ESP_OK)
			return result;

		vTaskDelay(pdMS_TO_TICKS(200));
		result = gpio_set_direction(GPIO_NUM_4, GPIO_MODE_INPUT);
		if(result != ESP_OK)
			return result;

		esp_lcd_panel_io_i2c_config_t touch_io_config{};
		touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
		touch_io_config.control_phase_bytes = 1;
		touch_io_config.lcd_cmd_bits = 16;
		touch_io_config.flags.disable_control_phase = 1;
		esp_lcd_panel_io_handle_t touch_io = nullptr;
		result = esp_lcd_new_panel_io_i2c(
			static_cast<esp_lcd_i2c_bus_handle_t>(I2C_NUM_0),
			&touch_io_config,
			&touch_io);
		if(result != ESP_OK)
			return result;

		esp_lcd_touch_config_t touch_config{};
		touch_config.x_max = width;
		touch_config.y_max = height;
		touch_config.rst_gpio_num = GPIO_NUM_NC;
		touch_config.int_gpio_num = GPIO_NUM_NC;
		result = esp_lcd_touch_new_i2c_gt911(touch_io, &touch_config, &touch_controller);
		if(result != ESP_OK)
			return result;

		esp_lcd_rgb_panel_config_t config{};
		config.clk_src = LCD_CLK_SRC_DEFAULT;
		config.timings.pclk_hz = pixel_clock_hz;
		config.timings.h_res = width;
		config.timings.v_res = height;
		config.timings.hsync_pulse_width = 4;
		config.timings.hsync_back_porch = 8;
		config.timings.hsync_front_porch = 8;
		config.timings.vsync_pulse_width = 4;
		config.timings.vsync_back_porch = 8;
		config.timings.vsync_front_porch = 8;
		config.timings.flags.pclk_active_neg = 1;
		config.data_width = 16;
		config.bits_per_pixel = 16;
		config.num_fbs = 2;
		config.bounce_buffer_size_px = width * bounce_buffer_rows;
		config.dma_burst_size = 64;
		config.hsync_gpio_num = 46;
		config.vsync_gpio_num = 3;
		config.de_gpio_num = 5;
		config.pclk_gpio_num = 7;
		config.disp_gpio_num = -1;
		constexpr int data_pins[]{14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40};
		std::copy(std::begin(data_pins), std::end(data_pins), config.data_gpio_nums);
		config.flags.fb_in_psram = 1;
		result = esp_lcd_new_rgb_panel(&config, &panel);
		if(result != ESP_OK)
			return result;

		result = esp_lcd_panel_reset(panel);
		if(result != ESP_OK)
			return result;

		result = esp_lcd_panel_init(panel);
		if(result != ESP_OK)
			return result;

		result = initialize_graphics(panel);
		if(result != ESP_OK)
			return result;

		return set_backlight(true);
	}

	esp_err_t read_touch(Touch &touch)
	{
		const auto result = esp_lcd_touch_read_data(touch_controller);
		if(result != ESP_OK)
			return result;

		esp_lcd_touch_point_data_t point{};
		uint8_t count = 0;
		const auto data_result = esp_lcd_touch_get_data(touch_controller, &point, &count, 1);
		if(data_result != ESP_OK)
			return data_result;

		touch.pressed = count > 0;
		touch.x = std::min<uint16_t>(point.x, width - 1);
		touch.y = std::min<uint16_t>(point.y, height - 1);
		return ESP_OK;
	}

} // namespace touch_lcd7
