// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/api_keys_idf.h>
#if NAWSERVER_API_KEYS
#include <nvs.h>
#include <esp_random.h>
namespace NaWServer::keys
{
	ReadStatus IdfStorage::read(unsigned record, void *bytes, std::size_t capacity, std::size_t &size)
	{
		size = 0; if(record > 1 || !namespace_ || !bytes) return ReadStatus::Error;
		nvs_handle_t handle; const auto opened = nvs_open(namespace_, NVS_READONLY, &handle);
		if(opened == ESP_ERR_NVS_NOT_FOUND) return ReadStatus::Missing;
		if(opened != ESP_OK) return ReadStatus::Error;
		size = capacity; const auto result = nvs_get_blob(handle, record ? "snapshot1" : "snapshot0", bytes, &size); nvs_close(handle);
		if(result == ESP_ERR_NVS_NOT_FOUND) { size = 0; return ReadStatus::Missing; }
		return result == ESP_OK ? ReadStatus::Ok : ReadStatus::Error;
	}
	bool IdfStorage::write(unsigned record, const void *bytes, std::size_t size)
	{
		if(record > 1 || !namespace_ || !bytes || !size) return false;
		nvs_handle_t handle; if(nvs_open(namespace_, NVS_READWRITE, &handle) != ESP_OK) return false;
		const auto result = nvs_set_blob(handle, record ? "snapshot1" : "snapshot0", bytes, size);
		const bool ok = result == ESP_OK && nvs_commit(handle) == ESP_OK; nvs_close(handle); return ok;
	}
	bool IdfRandom::fill(std::uint8_t *bytes, std::size_t size)
	{
		if((!bytes && size) || !entropy_ready_ || !entropy_ready_()) return false;
		// ESP-IDF requires an active RF entropy source (Wi-Fi/BT) for true random output.
		esp_fill_random(bytes, size); return true;
	}
}
#endif
