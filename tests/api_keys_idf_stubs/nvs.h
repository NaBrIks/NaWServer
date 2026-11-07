#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
using nvs_handle_t = std::uint32_t;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_NVS_NOT_FOUND = 1;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, std::size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, std::size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
