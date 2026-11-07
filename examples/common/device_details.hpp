// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
// Demo application metadata. Intentionally outside the HTTP/WS/SSE library.
#include <cstdio>
#include <cstring>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "soc/soc_caps.h"
namespace demo_info
{
	namespace detail
	{
		template <class Writer> void mac(Writer &writer, const char *name, esp_mac_type_t type, size_t length = 6)
		{
			uint8_t bytes[8]{};
			writer.key(name);
			if(esp_read_mac(bytes, type) != ESP_OK)
			{
				writer.null();
				return;
			}

			char formatted[24]{};
			for(size_t index = 0; index < length; index++)
				std::snprintf(
					formatted + index * 3,
					sizeof(formatted) - index * 3,
					index + 1 == length ? "%02X" : "%02X:",
					bytes[index]);
			writer.value(formatted);
		}

		template <class Writer>
		void address(Writer &writer, const char *name, const esp_ip4_addr_t &value, bool available)
		{
			writer.key(name);
			if(!available)
			{
				writer.null();
				return;
			}

			char text[16];
			std::snprintf(text, sizeof(text), IPSTR, IP2STR(&value));
			writer.value(text);
		}

	} // namespace detail
	struct HardwareFacts
	{
		const char *model;
		const char *architecture;
		unsigned hp_sram_bytes;
		unsigned lp_sram_bytes;
	};
// SoC properties belong to the shared collector; board package exceptions do not.
#if CONFIG_IDF_TARGET_ESP32C3
	inline constexpr HardwareFacts hardware{"ESP32-C3", "RISC-V", 400 * 1024, 8 * 1024};
#elif CONFIG_IDF_TARGET_ESP32S3
	inline constexpr HardwareFacts hardware{"ESP32-S3", "Xtensa LX7", 512 * 1024, 16 * 1024};
#elif CONFIG_IDF_TARGET_ESP32C6
	inline constexpr HardwareFacts hardware{"ESP32-C6", "RISC-V", 512 * 1024, 16 * 1024};
#else
#error Add verified SoC properties before using the demo INFO collector on this target
#endif
	template <class Writer, class BoardEncoder>
	void encode_device_details(Writer &writer, BoardEncoder encode_board, int embedded_flash_override = -1)
	{
		using detail::address;
		using detail::mac;
		esp_chip_info_t chip{};
		esp_chip_info(&chip);
		const auto *application = esp_app_get_description();
		const auto *partition = esp_ota_get_running_partition();
		uint32_t flash_size = 0, flash_id = 0;
		const bool flash_size_known = esp_flash_get_physical_size(nullptr, &flash_size) == ESP_OK;
		const bool flash_id_known = esp_flash_read_id(nullptr, &flash_id) == ESP_OK;
		writer.map(6);
		writer.key("hardware");
		writer.map(16);
		writer.put("chip_model", hardware.model);
		writer.put("architecture", hardware.architecture);
		writer.put("cpu_mhz", esp_clk_cpu_freq() / 1000000);
		writer.put("crystal_mhz", esp_clk_xtal_freq() / 1000000);
		writer.put("apb_mhz", esp_clk_apb_freq() / 1000000);
		writer.put("revision_major", chip.revision / 100);
		writer.put("revision_minor", chip.revision % 100);
		writer.put("cores", chip.cores);
		writer.put("wifi_24ghz", bool(chip.features & CHIP_FEATURE_WIFI_BGN));
#if SOC_WIFI_HE_SUPPORT
		writer.put("wifi_6", true);
#else
		writer.put("wifi_6", false);
#endif
		writer.put("bluetooth_le", bool(chip.features & CHIP_FEATURE_BLE));
		writer.put("bluetooth_classic", bool(chip.features & CHIP_FEATURE_BT));
		writer.put("ieee802154", bool(chip.features & CHIP_FEATURE_IEEE802154));
		writer.put("usb_serial_jtag", bool(SOC_USB_SERIAL_JTAG_SUPPORTED));
		writer.put("hp_sram_bytes", hardware.hp_sram_bytes);
		writer.put("lp_sram_bytes", hardware.lp_sram_bytes);
		writer.end();
		writer.key("flash_memory");
		writer.map(8);
		writer.key("flash_bytes");
		if(flash_size_known)
			writer.value(flash_size);
		else
			writer.null();

		writer.key("flash_jedec_id");
		if(flash_id_known)
		{
			char text[12];
			std::snprintf(text, sizeof(text), "0x%06lX", static_cast<unsigned long>(flash_id));
			writer.value(text);
		}
		else
			writer.null();

		writer.put(
			"embedded_flash",
			embedded_flash_override < 0 ? bool(chip.features & CHIP_FEATURE_EMB_FLASH) : bool(embedded_flash_override));
		writer.put("heap_internal_bytes", heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
		writer.put("heap_dma_bytes", heap_caps_get_total_size(MALLOC_CAP_DMA));
		writer.put("psram_bytes", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
		writer.put("app_partition", partition ? partition->label : "");
		writer.put("app_partition_bytes", partition ? partition->size : 0);
		writer.end();
		writer.key("mac_addresses");
		writer.map(8);
		mac(writer, "factory_mac", ESP_MAC_EFUSE_FACTORY);
		mac(writer, "custom_mac", ESP_MAC_EFUSE_CUSTOM);
		mac(writer, "base_mac", ESP_MAC_BASE);
		mac(writer, "wifi_sta_mac", ESP_MAC_WIFI_STA);
		mac(writer, "wifi_ap_mac", ESP_MAC_WIFI_SOFTAP);
		mac(writer, "bluetooth_mac", ESP_MAC_BT);
		mac(writer, "ethernet_mac", ESP_MAC_ETH);
#if SOC_IEEE802154_SUPPORTED
		mac(writer, "ieee802154_eui64", ESP_MAC_IEEE802154, 8);
#else
		writer.key("ieee802154_eui64");
		writer.null();
#endif
		writer.end();
		wifi_ap_record_t access_point{};
		const bool connected = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
		esp_netif_ip_info_t ip{};
		auto *interface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
		const bool has_ip = interface && esp_netif_get_ip_info(interface, &ip) == ESP_OK && ip.ip.addr;
		writer.key("network");
		writer.map(10);
		writer.put("wifi_connected", connected);
		wifi_mode_t mode{};
		writer.key("wifi_mode");
		if(esp_wifi_get_mode(&mode) != ESP_OK)
			writer.null();
		else
			writer.value(
				mode == WIFI_MODE_STA ? "STA"
				: mode == WIFI_MODE_AP ? "AP"
				: mode == WIFI_MODE_APSTA ? "AP+STA"
				                          : "OFF");

		address(writer, "ipv4", ip.ip, has_ip);
		address(writer, "netmask", ip.netmask, has_ip);
		address(writer, "gateway", ip.gw, has_ip);
		writer.key("ssid");
		if(connected)
			writer.text(
				reinterpret_cast<const char *>(access_point.ssid),
				strnlen(reinterpret_cast<const char *>(access_point.ssid), sizeof(access_point.ssid)));
		else
			writer.null();

		writer.key("ap_bssid");
		if(connected)
		{
			char text[18];
			std::snprintf(text, sizeof(text), MACSTR, MAC2STR(access_point.bssid));
			writer.value(text);
		}
		else
			writer.null();

		writer.key("channel");
		if(connected)
			writer.value(access_point.primary);
		else
			writer.null();

		writer.key("rssi_dbm");
		if(connected)
			writer.value(access_point.rssi);
		else
			writer.null();

		writer.key("wifi_auth_mode");
		if(connected)
			writer.value(static_cast<int>(access_point.authmode));
		else
			writer.null();

		writer.end();
		writer.key("board");
		encode_board(writer);
		// esp_app_get_elf_sha256 can return only CONFIG_APP_RETRIEVE_LEN_ELF_SHA
		// characters. The descriptor contains all 32 bytes, so expose the full hash.
		char sha256[65]{};
		for(size_t index = 0; index < 32; index++)
			std::snprintf(sha256 + index * 2, sizeof(sha256) - index * 2, "%02x", application->app_elf_sha256[index]);
		writer.key("build");
		writer.map(8);
		writer.put("project", application->project_name);
		writer.put("version", application->version);
		writer.put("idf_version", application->idf_ver);
		writer.put("build_date", application->date);
		writer.put("build_time", application->time);
		writer.put("secure_version", application->secure_version);
		writer.put("elf_sha256", sha256);
		writer.put("reset_reason", static_cast<int>(esp_reset_reason()));
		writer.end();
		writer.end();
	}

} // namespace demo_info
