// SPDX-License-Identifier: MIT
#include "esp.h"
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_app_desc.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <algorithm>
#include <limits>
#include <cstring>
#include <cstdio>
#include <unistd.h>

namespace compact
{
std::size_t EspOta::maximum() const
{
 const auto *next=esp_ota_get_next_update_partition(nullptr);
 return next?next->size:0;
}
const char *EspOta::runningSlot() const
{
 const auto *running=esp_ota_get_running_partition();
 return running?running->label:"unknown";
}
const char *EspOta::nextSlot() const
{
 const auto *next=esp_ota_get_boot_partition();
 return next?next->label:"unknown";
}
NaWServer::BodyReceiver EspOta::begin(std::size_t bytes,std::string_view sha256)
{
  if(active_||staged_||reboot_at_.load()||!bytes||sha256.size()!=64)return {};
 auto nibble=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
 for(std::size_t i=0;i<expected_digest_.size();++i){const auto hi=nibble(sha256[i*2]),lo=nibble(sha256[i*2+1]);if(hi<0||lo<0)return {};expected_digest_[i]=static_cast<std::uint8_t>((hi<<4)|lo);}
 const auto *next=esp_ota_get_next_update_partition(nullptr);
 if(!next||next==esp_ota_get_running_partition()||bytes>next->size)return {};
 esp_ota_handle_t handle=0;
 // Incremental sector erasure avoids a whole-slot blocking erase in the dispatcher.
 if(esp_ota_begin(next,OTA_WITH_SEQUENTIAL_WRITES,&handle)!=ESP_OK)return {};
  handle_=handle;target_=next;expected_=bytes;written_=0;active_=true;
 mbedtls_sha256_init(&digest_);digest_active_=true;
 if(mbedtls_sha256_starts(&digest_,0)!=0){cancel(this);return {};}
 return {this,consume,finish,cancel};
}
NaWServer::BodyConsumeResult EspOta::consume(void *context,NaWServer::ByteView bytes)
{
 auto &self=*static_cast<EspOta*>(context);
 if(!self.active_||bytes.size()>self.expected_-self.written_)return NaWServer::BodyConsumeResult::Error;
 if(mbedtls_sha256_update(&self.digest_,bytes.data(),bytes.size())!=0||esp_ota_write(self.handle_,bytes.data(),bytes.size())!=ESP_OK)return NaWServer::BodyConsumeResult::Error;
 self.written_+=bytes.size();return NaWServer::BodyConsumeResult::Accepted;
}
bool EspOta::finish(void *context)
{
 auto &self=*static_cast<EspOta*>(context);
 if(!self.active_||self.written_!=self.expected_)return false;
  std::array<std::uint8_t,32> digest{};
 const auto hashed=mbedtls_sha256_finish(&self.digest_,digest.data());
 mbedtls_sha256_free(&self.digest_);self.digest_active_=false;
 if(hashed!=0||digest!=self.expected_digest_){cancel(context);return false;}
 const auto result=esp_ota_end(self.handle_);
 self.handle_=0;self.active_=false;
 // esp_ota_end validates the complete image and frees its SDK handle, even on failure.
 if(result!=ESP_OK||esp_ota_set_boot_partition(self.target_)!=ESP_OK)return false;
 self.staged_=true;
 // The common endpoint schedules reboot only after its success reply.
 return true;
}
void EspOta::cancel(void *context)
{
 auto &self=*static_cast<EspOta*>(context);
 if(self.active_){(void)esp_ota_abort(self.handle_);self.active_=false;self.handle_=0;}
 if(self.digest_active_){mbedtls_sha256_free(&self.digest_);self.digest_active_=false;}
 self.target_=nullptr;self.expected_=self.written_=0;
}
bool EspOta::confirm()
{
 const auto *running=esp_ota_get_running_partition();esp_ota_img_states_t state;
 if(!running)return false;
 const auto status=esp_ota_get_state_partition(running,&state);
 if(status==ESP_OK&&state==ESP_OTA_IMG_PENDING_VERIFY)return esp_ota_mark_app_valid_cancel_rollback()==ESP_OK;
 return status==ESP_ERR_NOT_FOUND||status==ESP_OK;
}
bool EspOta::rollback()
{
 return esp_ota_mark_app_invalid_rollback_and_reboot()==ESP_OK;
}
void EspOta::scheduleRestart()
{
 reboot_at_=static_cast<std::uint64_t>(esp_timer_get_time()/1000)+1000;
}
void EspOta::poll()
{
 const auto at=reboot_at_.load();
 if(at&&static_cast<std::uint64_t>(esp_timer_get_time()/1000)>=at)esp_restart();
}
bool EspPlatform::entropyReady()
{
 wifi_mode_t mode=WIFI_MODE_NULL;
 return esp_wifi_get_mode(&mode)==ESP_OK&&mode!=WIFI_MODE_NULL;
}
bool EspPlatform::initialize()
{
 if(keys_.load()!=NaWServer::keys::Status::Ok)return false;
 nvs_handle_t handle;
 if(nvs_open("naw_compact",NVS_READONLY,&handle)==ESP_OK)
 {
  Settings value{};std::size_t size=sizeof(value);
  if(nvs_get_blob(handle,"settings",&value,&size)==ESP_OK&&size==sizeof(value)&&value.name[24]==0&&value.interval_ms>=250&&value.interval_ms<=60000)settings_=value;
  nvs_close(handle);
 }
 if(!keys_.metadata(0).active)
 {
  NaWServer::keys::Secret secret;
  if(!issueKey(secret))return false;
  // Provisioning explicitly reveals the one newly-created credential once over USB.
  fputs("COMPACT_KEY=",stdout);fputs(secret.value.data(),stdout);fputc('\n',stdout);secret.clear();
 }
 confirm_at_=0;
 return true;
}
bool EspPlatform::issueKey(NaWServer::keys::Secret &secret)
{
 NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(keys_mutex_);
 const auto result=keys_.metadata(0).active?keys_.rotate(0,secret):keys_.create("primary",secret);
 return result==NaWServer::keys::Status::Ok;
}
bool EspPlatform::configure(const Settings &settings)
{
 NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(settings_mutex_);
 if(!std::memchr(settings.name,0,sizeof(settings.name))||settings.interval_ms<250||settings.interval_ms>60000)return false;
 nvs_handle_t handle;if(nvs_open("naw_compact",NVS_READWRITE,&handle)!=ESP_OK)return false;
 const auto write=nvs_set_blob(handle,"settings",&settings,sizeof(settings));
 const bool saved=write==ESP_OK&&nvs_commit(handle)==ESP_OK;
 Settings readback{};std::size_t size=sizeof(readback);
 const bool checked=saved&&nvs_get_blob(handle,"settings",&readback,&size)==ESP_OK&&size==sizeof(readback)&&std::memcmp(&settings,&readback,sizeof(settings))==0;
 nvs_close(handle);if(checked)settings_=settings;return checked;
}
std::uint64_t EspPlatform::milliseconds(){return static_cast<std::uint64_t>(esp_timer_get_time()/1000);}
#if NAWSERVER_DEMO_CPU_STATS
void EspPlatform::sampleCpu()
{
#if configGENERATE_RUN_TIME_STATS && INCLUDE_xTaskGetIdleTaskHandle && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && !CONFIG_FREERTOS_SMP
 static_assert(configNUM_CORES<=2,"Extend the bounded idle counter array for this target");
 const auto now=static_cast<std::uint64_t>(esp_timer_get_time());
 if(cpu_sample_at_us_&&now-cpu_sample_at_us_<static_cast<std::uint64_t>(NAWSERVER_DEMO_CPU_SAMPLE_INTERVAL_MS)*1000)return;
 std::uint64_t idle_delta=0;
 for(unsigned core=0;core<configNUM_CORES;++core)
 {
  const auto idle=ulTaskGetIdleRunTimeCounterForCore(core);
  // Subtraction in the native type also handles a user-selected 32-bit counter.
  idle_delta+=static_cast<configRUN_TIME_COUNTER_TYPE>(idle-static_cast<configRUN_TIME_COUNTER_TYPE>(previous_idle_[core]));
  previous_idle_[core]=idle;
 }
 if(cpu_sample_at_us_)
 {
  const auto elapsed=now-cpu_sample_at_us_;
  NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(cpu_mutex_);
  // A user-selected 32-bit counter cannot describe a longer unobserved interval.
  if(elapsed>std::numeric_limits<configRUN_TIME_COUNTER_TYPE>::max())cpu_available_=false;
  else
  {
   const auto capacity=elapsed*configNUM_CORES;
   const auto busy=capacity-std::min(idle_delta,capacity);
   cpu_percent_=100.0*static_cast<double>(busy)/static_cast<double>(capacity);
   cpu_busy_us_+=busy;cpu_sample_us_+=capacity;cpu_available_=true;
  }
 }
 cpu_sample_at_us_=now;
#endif
}
#endif
Status EspPlatform::status()
{
 wifi_ap_record_t ap{};const auto *description=esp_app_get_description();
 // With PSRAM enabled, keep scarce internal SRAM distinct from external RAM.
#if CONFIG_SPIRAM
 constexpr unsigned capabilities=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
#else
 constexpr unsigned capabilities=MALLOC_CAP_8BIT;
#endif
 multi_heap_info_t heap{};heap_caps_get_info(&heap,capabilities);
 Status result;result.platform=CONFIG_IDF_TARGET;result.firmware=description->version;result.running_slot=ota_.runningSlot();result.uptime_ms=static_cast<std::uint64_t>(esp_timer_get_time()/1000);
 result.free_heap=heap.total_free_bytes;result.min_free_heap=heap.minimum_free_bytes;
 result.total_heap=heap_caps_get_total_size(capabilities);result.largest_free_block=heap.largest_free_block;result.cpu_cores=configNUM_CORES;
#if CONFIG_SPIRAM
 result.heap_scope="internal heap";
 result.secondary_heap_scope="PSRAM heap";
 result.secondary_heap_total=heap_caps_get_total_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
 result.secondary_heap_free=heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
 result.heap_scope="8-bit heap";
#endif
 // Main on-chip SRAM capacity; RTC SRAM and optional external PSRAM are separate.
 // This is not the allocatable heap denominator.
#if CONFIG_IDF_TARGET_ESP32C3
 result.physical_ram_bytes=400u*1024u;
#elif CONFIG_IDF_TARGET_ESP32S3
 result.physical_ram_bytes=512u*1024u;
#endif
#if NAWSERVER_DEMO_CPU_STATS
 {NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(cpu_mutex_);result.cpu_percent=cpu_percent_;result.cpu_available=cpu_available_;result.cpu_busy_us=cpu_busy_us_;result.cpu_sample_us=cpu_sample_us_;}
#endif
 if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK)result.rssi_dbm=ap.rssi;
 return result;
}
void EspPlatform::poll()
{
#if NAWSERVER_DEMO_STACK_DIAGNOSTICS
 static std::uint64_t next_stack_report=0;
 const auto now_stack_report=milliseconds();
 if(now_stack_report>=next_stack_report)
 {
  next_stack_report=now_stack_report+5000;
  for(const char *name : {"main", "naw_tcp_http", "naw_tcp_tx", "naw_tcp_sse"})
  {
   const auto task=xTaskGetHandle(name);
   if(task)printf("COMPACT_STACK task=%s min_free_bytes=%u\n",name,static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)));
  }
 }
#endif
#if NAWSERVER_DEMO_CPU_STATS
 sampleCpu();
#endif
  // Physical USB/UART 'k' explicitly rotates the sole credential for recovery.
 char command;
 if(read(STDIN_FILENO,&command,1)==1&&command=='k')
 {
  NaWServer::keys::Secret secret;
  if(issueKey(secret)){fputs("COMPACT_KEY=",stdout);fputs(secret.value.data(),stdout);fputc('\n',stdout);}
  else puts("COMPACT_KEY_ERROR");
  secret.clear();
 }
 // The application calls poll only after successful server startup.
 if(!confirm_at_)confirm_at_=static_cast<std::uint64_t>(esp_timer_get_time()/1000)+5000;
 if(!confirmed_&&static_cast<std::uint64_t>(esp_timer_get_time()/1000)>=confirm_at_)confirmed_=ota_.confirm();
 ota_.poll();
}}
