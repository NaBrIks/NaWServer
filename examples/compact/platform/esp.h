// SPDX-License-Identifier: MIT
#pragma once
#include <NaWServer/request_body_stream.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "../common/platform.hpp"
#include <NaWServer/api_keys_idf.h>
#include <array>
#include <NaWServer/detail/synchronization.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef NAWSERVER_DEMO_CPU_STATS
#define NAWSERVER_DEMO_CPU_STATS 1
#endif
#ifndef NAWSERVER_DEMO_CPU_SAMPLE_INTERVAL_MS
#define NAWSERVER_DEMO_CPU_SAMPLE_INTERVAL_MS 1000
#endif
static_assert(NAWSERVER_DEMO_CPU_SAMPLE_INTERVAL_MS > 0, "CPU sampling interval must be positive");

namespace compact
{
class EspOta
{
 esp_ota_handle_t handle_=0;
 const esp_partition_t *target_=nullptr;
 std::size_t expected_=0,written_=0;
 bool active_=false;
 bool staged_=false;
 bool digest_active_=false;
 mbedtls_sha256_context digest_{};
 std::array<std::uint8_t,32> expected_digest_{};
 std::atomic<std::uint64_t> reboot_at_{0};
 static NaWServer::BodyConsumeResult consume(void *,NaWServer::ByteView);
 static bool finish(void *);
 static void cancel(void *);
public:
 NaWServer::BodyReceiver begin(std::size_t bytes,std::string_view sha256);
 std::size_t maximum() const;
 const char *runningSlot() const;
 const char *nextSlot() const;
 bool confirm();
 bool rollback();
 void poll();
 void scheduleRestart();
};
class EspPlatform final : public Platform
{
 NaWServer::keys::IdfStorage storage_{"naw_compact"};
 NaWServer::keys::IdfRandom random_;
 NaWServer::keys::Store keys_{storage_,random_};
 NaWServer::detail::Mutex keys_mutex_;
 Settings settings_{};
 NaWServer::detail::Mutex settings_mutex_;
 EspOta ota_;
 bool confirmed_=false;
 std::uint64_t confirm_at_=0;
#if NAWSERVER_DEMO_CPU_STATS
 NaWServer::detail::Mutex cpu_mutex_;
 std::array<std::uint64_t,2> previous_idle_{};
 std::uint64_t cpu_sample_at_us_=0;
 std::uint64_t cpu_busy_us_=0,cpu_sample_us_=0;
 double cpu_percent_=0;
 bool cpu_available_=false;
 void sampleCpu();
#endif
 static bool entropyReady();
public:
 EspPlatform():random_(entropyReady){}
 bool initialize();
 bool issueKey(NaWServer::keys::Secret &secret)override;
 NaWServer::keys::Store &keys()override{return keys_;}
 bool authorized(std::string_view secret)override{NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(keys_mutex_);return keys_.authenticate(secret).has_value();}
 std::uint32_t keyRevision()override{NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(keys_mutex_);return keys_.metadata(0).revision;}
 Settings settings()override{NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(settings_mutex_);return settings_;}
 bool configure(const Settings &)override;
 Status status()override;
 std::uint64_t milliseconds()override;
 std::size_t maximumFirmwareBytes()override{return ota_.maximum();}
 const char *firmwareFormat()override{return "bin";}
 NaWServer::BodyReceiver beginFirmware(std::size_t bytes,std::string_view hash)override{return ota_.begin(bytes,hash);}
 void scheduleRestart()override{ota_.scheduleRestart();}
 void poll()override;
};}
