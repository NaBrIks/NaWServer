// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "application.hpp"

#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/websocket.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstdio>
using namespace NaWServer;
namespace touch_lcd7
{
	static SendResult status(Response &response)
	{
		const auto value = snapshot();
		char json[512];
		const int size = std::snprintf(
			json,
			sizeof(json),
			"{\"demo\":\"touch-lcd7\",\"source\":\"device\",\"ready\":%s,\"width\":800,\"height\":480,"
			"\"pattern\":%u,\"backlight\":%s,\"pressed\":%s,\"x\":%u,\"y\":%u,\"touches\":%u,"
			"\"touch_errors\":%u,\"uptime_ms\":%llu,\"heap_free\":%u,\"psram_free\":%u,"
			"\"renderer\":\"lvgl-8.3.11\",\"framebuffers\":2,\"frame_swaps\":%u,\"frame_events\":%u,\"vsyncs\":%u}",
			value.ready ? "true" : "false",
			value.pattern,
			value.backlight ? "true" : "false",
			value.touch.pressed ? "true" : "false",
			value.touch.x,
			value.touch.y,
			value.touches,
			value.touch_errors,
			static_cast<unsigned long long>(esp_timer_get_time() / 1000),
			static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
			static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
			static_cast<unsigned>(value.graphics.swaps),
			static_cast<unsigned>(value.graphics.frame_events),
			static_cast<unsigned>(value.graphics.vsyncs));
		if(size < 0 || static_cast<size_t>(size) >= sizeof(json))
			return SendResult::BufferTooSmall;

		return response.send(
			200,
			"application/json",
			{reinterpret_cast<const uint8_t *>(json), static_cast<size_t>(size)});
	}

	static SendResult control(Request &request, Response &response)
	{
		// Require same-origin browser fetch semantics; reject cross-origin/form commands.
		const auto marker = request.header("X-LCD7-Command");
		if(!marker || *marker != "1")
			return response.text(403, "Command header required");

		const auto site = request.header("Sec-Fetch-Site");
		if(site && *site != "same-origin" && *site != "none")
			return response.text(403, "Same origin required");

		if(!snapshot().ready)
			return response.text(503, "Board not ready");

		const auto body = request.body();
		if(body.empty())
			return response.text(400, "Command required");

		const std::string_view command(reinterpret_cast<const char *>(body.data()), body.size());
		Command selected;
		if(command == "bars")
			selected = Command::Bars;
		else if(command == "grid")
			selected = Command::Grid;
		else if(command == "red")
			selected = Command::Red;
		else if(command == "green")
			selected = Command::Green;
		else if(command == "blue")
			selected = Command::Blue;
		else if(command == "on")
			selected = Command::BacklightOn;
		else if(command == "off")
			selected = Command::BacklightOff;
		else
			return response.text(400, "Unknown command");

		return submit(selected) ? response.text(202, "Queued") : response.text(503, "Command queue full");
	}

	static Route status_route("/api/status", HttpMethod::Get, status);
	static Route control_route("/api/display", HttpMethod::Post, control);
	HandlerBase *status_handler()
	{
		return &status_route;
	}

	HandlerBase *control_handler()
	{
		return &control_route;
	}

} // namespace touch_lcd7
