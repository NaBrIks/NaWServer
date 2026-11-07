// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <cstdio>
#include <cstdlib>
void require(bool condition)
{
	if(!condition)
		std::abort();
}

#include "sdk.hpp"
#if CONFIG_IDF_TARGET_ESP32C3
#include "../../showcase/main/device_details.hpp"
#include "../../showcase/main/runtime.hpp"
namespace showcase
{
	State runtime_state()
	{
		State state;
		state.strip.supported = true;
		state.strip.driver = 1;
		return state;
	}

} // namespace showcase
namespace demo = showcase;
#elif CONFIG_IDF_TARGET_ESP32S3
#include "../../touch_lcd7/main/device_details.hpp"
#include "../../touch_lcd7/main/runtime.hpp"
#include "../../touch_lcd7/main/application.hpp"
namespace lcd7_web
{
	State runtime_state()
	{
		State state;
		state.strip.supported = true;
		return state;
	}

} // namespace lcd7_web
namespace touch_lcd7
{
	State snapshot()
	{
		State state;
		state.ready = true;
		return state;
	}

} // namespace touch_lcd7
namespace demo = lcd7_web;
#else
#include "../../c6_lcd147/main/device_details.hpp"
namespace c6_demo
{
	bool lcd_ready()
	{
		return true;
	}

} // namespace c6_demo
namespace demo = c6_demo;
#endif
int main(int argc, char **argv)
{
	require(argc == 3);
	for(int available = 0; available < 2; available++)
	{
		test_available = available;
		uint8_t buffer[4096]{};
		demo::Writer writer(buffer, sizeof(buffer));
		demo::encode_device_details(writer);
		const auto size = writer.size(buffer);
		require(size > 0 && size + 1000 < sizeof(buffer));
		char path[1024];
		std::snprintf(path, sizeof(path), "%s/%s-%s.cbor", argv[1], argv[2], available ? "connected" : "unavailable");
		FILE *output = std::fopen(path, "wb");
		require(output);
		require(std::fwrite(buffer, 1, size, output) == size);
		std::fclose(output);
		uint8_t tiny[16]{};
		demo::Writer short_writer(tiny, sizeof(tiny));
		demo::encode_device_details(short_writer);
		require(short_writer.size(tiny) == 0);
		std::printf(
			"PASS %s %s: %zu bytes; overflow rejected\n",
			argv[2],
			available ? "connected" : "unavailable",
			size);
	}
}
