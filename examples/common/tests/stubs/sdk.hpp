// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int CHIP_FEATURE_WIFI_BGN = 1, CHIP_FEATURE_BLE = 2, CHIP_FEATURE_BT = 4, CHIP_FEATURE_IEEE802154 = 8,
    CHIP_FEATURE_EMB_FLASH = 16;
constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_DMA = 4, MALLOC_CAP_SPIRAM = 8;
#define SOC_USB_SERIAL_JTAG_SUPPORTED 1
#if CONFIG_IDF_TARGET_ESP32C6
#define SOC_WIFI_HE_SUPPORT 1
#define SOC_IEEE802154_SUPPORTED 1
#endif
inline bool test_available = true;
struct esp_chip_info_t
{
	int revision = 1, cores = CONFIG_IDF_TARGET_ESP32S3 ? 2 : 1;
	unsigned features = CHIP_FEATURE_WIFI_BGN | CHIP_FEATURE_BLE;
};
inline void esp_chip_info(esp_chip_info_t *chip)
{
	*chip = {};
}

struct esp_app_desc_t
{
	const char *project_name = "demo", *version = "test", *idf_ver = "5.4.0", *date = "Oct 20 2026", *time = "12:00:00";
	unsigned secure_version = 0;
	uint8_t app_elf_sha256[32]{};
};
inline const esp_app_desc_t *esp_app_get_description()
{
	static esp_app_desc_t app;
	return &app;
}

struct esp_partition_t
{
	const char *label = "factory";
	unsigned size = 3145728;
};
inline const esp_partition_t *esp_ota_get_running_partition()
{
	static esp_partition_t partition;
	return test_available ? &partition : nullptr;
}

inline int esp_flash_get_physical_size(void *, uint32_t *value)
{
	*value = 4194304;
	return test_available ? 0 : 1;
}

inline int esp_flash_read_id(void *, uint32_t *value)
{
	*value = 0xEF4016;
	return test_available ? 0 : 1;
}

inline unsigned heap_caps_get_total_size(unsigned caps)
{
	return caps == MALLOC_CAP_SPIRAM ? (CONFIG_IDF_TARGET_ESP32S3 ? 8388608 : 0) : 327680;
}

enum esp_mac_type_t
{
	ESP_MAC_EFUSE_FACTORY,
	ESP_MAC_EFUSE_CUSTOM,
	ESP_MAC_BASE,
	ESP_MAC_WIFI_STA,
	ESP_MAC_WIFI_SOFTAP,
	ESP_MAC_BT,
	ESP_MAC_ETH,
	ESP_MAC_IEEE802154
};
inline int esp_read_mac(uint8_t *bytes, esp_mac_type_t type)
{
	if(!test_available || type == ESP_MAC_EFUSE_CUSTOM)
		return 1;

	for(unsigned i = 0; i < 8; i++)
		bytes[i] = uint8_t(i + 1);
	return 0;
}

struct esp_ip4_addr_t
{
	uint32_t addr = 0;
};
struct esp_netif_ip_info_t
{
	esp_ip4_addr_t ip, netmask, gw;
};
struct esp_netif_t
{
};
inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *)
{
	static esp_netif_t netif;
	return test_available ? &netif : nullptr;
}

inline int esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *ip)
{
	ip->ip.addr = 1;
	return 0;
}

#define IPSTR "%u.%u.%u.%u"
#define IP2STR(value) 192u, 168u, 1u, unsigned((value)->addr)
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(value) (value)[0], (value)[1], (value)[2], (value)[3], (value)[4], (value)[5]
struct wifi_ap_record_t
{
	uint8_t ssid[33]{};
	uint8_t bssid[6]{1, 2, 3, 4, 5, 6};
	int primary = 6, rssi = -53, authmode = 3;
};
inline int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{
	std::memset(ap->ssid, 'S', sizeof(ap->ssid));
	return test_available ? 0 : 1;
}

enum wifi_mode_t
{
	WIFI_MODE_NULL,
	WIFI_MODE_STA,
	WIFI_MODE_AP,
	WIFI_MODE_APSTA
};
inline int esp_wifi_get_mode(wifi_mode_t *mode)
{
	*mode = WIFI_MODE_APSTA;
	return test_available ? 0 : 1;
}

inline int esp_clk_cpu_freq()
{
	return 160000000;
}

inline int esp_clk_xtal_freq()
{
	return 40000000;
}

inline int esp_clk_apb_freq()
{
	return 80000000;
}

inline int esp_reset_reason()
{
	return 1;
}
