// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/api_keys_idf.h>
#include <nvs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace { unsigned calls = 0; bool enabled = false; bool ready() { return enabled; } }
esp_err_t nvs_open(const char *, int, nvs_handle_t *) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, std::size_t *) { return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, std::size_t) { return 2; }
esp_err_t nvs_commit(nvs_handle_t) { return 2; }
void nvs_close(nvs_handle_t) {}
void esp_fill_random(void *bytes, std::size_t size) { ++calls; std::memset(bytes, 0xa5, size); }
int main()
{
	using namespace NaWServer::keys;
	std::uint8_t bytes[32]{}; IdfRandom missing, checked(ready);
	if(missing.fill(bytes, sizeof(bytes)) || checked.fill(bytes, sizeof(bytes)) || calls) return 1;
	enabled = true;
	if(!checked.fill(bytes, sizeof(bytes)) || calls != 1 || bytes[0] != 0xa5 || checked.fill(nullptr, 1)) return 2;
	IdfStorage storage; std::size_t size = 9;
	if(storage.read(0, bytes, sizeof(bytes), size) != ReadStatus::Missing || size || storage.write(0, bytes, sizeof(bytes))) return 3;
	std::puts("API_KEYS_IDF_RANDOM_PASS");
}
