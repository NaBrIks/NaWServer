# Waveshare ESP32-C6-LCD-1.47 demo

Follow root AGENTS.md. Owner explicitly authorized this C6 demo and hardware
flash on 2026-10-20; the older C3-only restriction does not apply to this target.
Re-identify the actual ESP32-C6 port/chip/MAC before every flash. Never flash
another attached board. Owner explicitly authorized a complete C6 flash erase
on 2026-10-20 to start clean. Do not make flash backups.

Read and apply [docs/ui-principles.md](docs/ui-principles.md) before UI work.
Frontend sources belong to root frontend/; this demo only adds its profile.
Keep LCD/RGB pin mappings in this independent application, outside library core.
SPI2 is owned by LCD (SD deselected on GPIO4); RGB is one WS2812 on GPIO8,
using RMT only. Do not enable the showcase SPI strip backend on this board.
LCD SPI write success does not prove that a damaged screen displays pixels.
Keep backlight at 30 percent by default, never above vendor's 50 percent advice.
Credentials, serial logs, firmware and generated assets remain ignored.
