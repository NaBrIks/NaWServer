// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "graphics.hpp"
#include "board.hpp"
#include "lvgl.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
namespace touch_lcd7
{
	static constexpr int width = 800, height = 480;
	static esp_lcd_panel_handle_t display_panel;
	static StaticSemaphore_t event_storage;
	static SemaphoreHandle_t display_event;
	static uint32_t frame_events, vsync_events, swaps;
	static lv_disp_draw_buf_t draw_buffers;
	static lv_disp_drv_t display_driver;
	static lv_indev_drv_t pointer_driver;
	static lv_obj_t *pattern_area, *coordinates, *cursor, *touch_status;
	static lv_obj_t *network_address;
	static char displayed_network_address[16]{};
	static unsigned current_pattern = 0;
	static unsigned current_touches = 0;
	static Touch pointer;
	static int64_t last_tick_us;
	static bool IRAM_ATTR frame_complete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *)
	{
		__atomic_fetch_add(&frame_events, 1u, __ATOMIC_RELEASE);
		BaseType_t wake = pdFALSE;
		xSemaphoreGiveFromISR(display_event, &wake);
		return wake == pdTRUE;
	}

	static bool IRAM_ATTR vsync_complete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *)
	{
		__atomic_fetch_add(&vsync_events, 1u, __ATOMIC_RELEASE);
		BaseType_t wake = pdFALSE;
		xSemaphoreGiveFromISR(display_event, &wake);
		return wake == pdTRUE;
	}

	static uint32_t read_counter(const uint32_t &counter)
	{
		return __atomic_load_n(&counter, __ATOMIC_ACQUIRE);
	}

	static void wait_for_change(const uint32_t &counter, uint32_t previous)
	{
		const TickType_t started = xTaskGetTickCount();
		while(read_counter(counter) == previous)
		{
			if(xTaskGetTickCount() - started >= pdMS_TO_TICKS(500))
			{
				// Fail closed: never let LVGL reuse a buffer still owned by scanout.
				ESP_LOGE("lcd7_graphics", "LCD frame synchronization timed out");
				abort();
			}

			xSemaphoreTake(display_event, pdMS_TO_TICKS(50));
		}
	}

	static void flush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *buffer)
	{
		ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(display_panel, 0, 0, width, height, buffer));
		// IDF 5.4 bounce mode adopts cur_fb_index at on_frame_buf_complete.
		// Capture AFTER submitting: even an ISR racing with submission cannot release
		// the previous framebuffer too early. Then wait for the following VSYNC.
		wait_for_change(frame_events, read_counter(frame_events));
		wait_for_change(vsync_events, read_counter(vsync_events));
		++swaps;
		lv_disp_flush_ready(driver);
	}

	static void read_pointer(lv_indev_drv_t *, lv_indev_data_t *data)
	{
		data->point.x = pointer.x;
		data->point.y = pointer.y;
		data->state = pointer.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	}

	static void draw_pattern(lv_event_t *event)
	{
		auto *context = lv_event_get_draw_ctx(event);
		lv_area_t area;
		lv_obj_get_coords(lv_event_get_target(event), &area);
		lv_draw_rect_dsc_t rectangle;
		lv_draw_rect_dsc_init(&rectangle);
		rectangle.bg_opa = LV_OPA_COVER;
		rectangle.border_width = 0;
		constexpr uint32_t colors[]{0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x00ffff, 0xff00ff, 0xffffff, 0x000000};
		if(current_pattern == 0)
		{
			for(int i = 0; i < 8; ++i)
			{
				lv_area_t stripe{
					static_cast<lv_coord_t>(area.x1 + i * 100),
					area.y1,
					static_cast<lv_coord_t>(area.x1 + (i + 1) * 100 - 1),
					area.y2};
				rectangle.bg_color = lv_color_hex(colors[i]);
				lv_draw_rect(context, &rectangle, &stripe);
			}
		}
		else if(current_pattern == 1)
		{
			for(int y = 0; y < 250; y += 25)
				for(int x = 0; x < 800; x += 25)
				{
					lv_area_t cell{
						static_cast<lv_coord_t>(area.x1 + x),
						static_cast<lv_coord_t>(area.y1 + y),
						static_cast<lv_coord_t>(area.x1 + x + 24),
						static_cast<lv_coord_t>(area.y1 + y + 24)};
					rectangle.bg_color = lv_color_hex(((x / 25 + y / 25) % 2) ? 0xffffff : 0);
					lv_draw_rect(context, &rectangle, &cell);
				}
		}
		else
		{
			rectangle.bg_color = lv_color_hex(colors[std::min(current_pattern - 2, 2u)]);
			lv_draw_rect(context, &rectangle, &area);
		}
	}

	static lv_obj_t *label(lv_obj_t *parent, int x, int y, const char *text)
	{
		auto *object = lv_label_create(parent);
		lv_label_set_text(object, text);
		lv_obj_set_pos(object, x, y);
		return object;
	}

	esp_err_t initialize_graphics(esp_lcd_panel_handle_t panel)
	{
		static_assert(sizeof(lv_color_t) == 2, "RGB565 required");
		display_panel = panel;
		display_event = xSemaphoreCreateBinaryStatic(&event_storage);
		if(!display_event)
			return ESP_ERR_NO_MEM;

		esp_lcd_rgb_panel_event_callbacks_t callbacks{};
		callbacks.on_frame_buf_complete = frame_complete;
		callbacks.on_vsync = vsync_complete;
		auto result = esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, nullptr);
		if(result != ESP_OK)
			return result;

		void *front = nullptr, *back = nullptr;
		result = esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &front, &back);
		if(result != ESP_OK)
			return result;

		lv_init();
		// IDF starts scanning buffer 0; the FIRST LVGL render must target buffer 1.
		lv_disp_draw_buf_init(&draw_buffers, back, front, width * height);
		lv_disp_drv_init(&display_driver);
		display_driver.hor_res = width;
		display_driver.ver_res = height;
		display_driver.draw_buf = &draw_buffers;
		display_driver.flush_cb = flush;
		display_driver.full_refresh = 1;
		if(!lv_disp_drv_register(&display_driver))
			return ESP_ERR_NO_MEM;

		lv_indev_drv_init(&pointer_driver);
		pointer_driver.type = LV_INDEV_TYPE_POINTER;
		pointer_driver.read_cb = read_pointer;
		if(!lv_indev_drv_register(&pointer_driver))
			return ESP_ERR_NO_MEM;

		auto *screen = lv_scr_act();
		lv_obj_set_style_bg_color(screen, lv_color_hex(0x111b29), 0);
		lv_obj_set_style_text_color(screen, lv_color_white(), 0);
		lv_obj_set_style_text_font(screen, &lv_font_montserrat_20, 0);
		lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
		auto *title = label(screen, 24, 18, "LCD 7  /  LVGL  /  S3");
		lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
		network_address = label(screen, 480, 25, "IP: offline");
		lv_obj_set_width(network_address, 296);
		lv_obj_set_style_text_align(network_address, LV_TEXT_ALIGN_RIGHT, 0);
		label(screen, 24, 63, "Touch the top strip to change the pattern");
		pattern_area = lv_obj_create(screen);
		lv_obj_remove_style_all(pattern_area);
		lv_obj_set_pos(pattern_area, 0, 110);
		lv_obj_set_size(pattern_area, width, 250);
		lv_obj_clear_flag(pattern_area, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(pattern_area, draw_pattern, LV_EVENT_DRAW_MAIN, nullptr);
		coordinates = label(screen, 24, 390, "X: 0   Y: 0   Touch: 0");
		touch_status = label(screen, 24, 432, "READY  /  RGB565  /  Double buffer + VSYNC");
		lv_obj_set_style_text_color(touch_status, lv_color_hex(0x69e6bd), 0);
		cursor = lv_obj_create(screen);
		lv_obj_remove_style_all(cursor);
		lv_obj_set_size(cursor, 24, 24);
		lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(cursor, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color(cursor, lv_color_black(), 0);
		lv_obj_set_style_border_width(cursor, 3, 0);
		lv_obj_clear_flag(cursor, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);
		last_tick_us = esp_timer_get_time();
		return ESP_OK;
	}

	void display_network_address(const char *address)
	{
		if(std::strcmp(displayed_network_address, address) == 0)
			return;

		std::snprintf(displayed_network_address, sizeof(displayed_network_address), "%s", address);
		lv_label_set_text_fmt(network_address, "IP: %s", address[0] ? address : "offline");
		ESP_LOGI("lcd7_graphics", "LCD IP: %s", address[0] ? address : "offline");
	}

	void draw_screen(unsigned pattern, const Touch &touch, unsigned touches)
	{
		if(pattern != current_pattern)
		{
			current_pattern = pattern;
			lv_obj_invalidate(pattern_area);
		}

		if(touch.x != pointer.x || touch.y != pointer.y || touches != current_touches)
			lv_label_set_text_fmt(coordinates, "X: %u   Y: %u   Touch: %u", touch.x, touch.y, touches);

		if(touch.pressed)
		{
			lv_obj_clear_flag(cursor, LV_OBJ_FLAG_HIDDEN);
			lv_obj_set_pos(cursor, touch.x - 12, touch.y - 12);
		}
		else
			lv_obj_add_flag(cursor, LV_OBJ_FLAG_HIDDEN);

		pointer = touch;
		current_touches = touches;
	}

	void process_graphics()
	{
		const int64_t now = esp_timer_get_time();
		const auto elapsed_ms = static_cast<uint32_t>((now - last_tick_us) / 1000);
		lv_tick_inc(elapsed_ms);
		last_tick_us += elapsed_ms * 1000LL;
		lv_timer_handler();
	}

	GraphicsStats graphics_stats()
	{
		return {swaps, read_counter(frame_events), read_counter(vsync_events)};
	}

} // namespace touch_lcd7
