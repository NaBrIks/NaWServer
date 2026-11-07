// SPDX-License-Identifier: MIT
#include "sdkconfig.h"
#include "../../common/demo.hpp"
#include "../../platform/esp.h"
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp.h>
#if NAWSERVER_DEMO_README_PROFILE
#include "readme_profile.hpp"
#endif
#if NAWSERVER_DEMO_MEMORY_LINK_PROFILE
#include "memory_link_profile.hpp"
#endif
#if NAWSERVER_DEMO_MEMORY_BREAKDOWN
#include "memory_breakdown.hpp"
#endif
#if NAWSERVER_DEMO_TX_BULK_PROBE
#include "tx_bulk_probe.hpp"
#endif
#if NAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS
#include "esp_transport_probe.hpp"
#endif
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_system.h>
#if NAWSERVER_DEMO_MEMORY_PROBE || NAWSERVER_DEMO_MEMORY_SAMPLER
#include <esp_heap_caps.h>
#include <esp_timer.h>
#endif
#if NAWSERVER_DEMO_MEMORY_PROBE
#include <esp_http_server.h>
#endif
#if NAWSERVER_DEMO_MEMORY_PROBE || NAWSERVER_DEMO_FULL_STOP_PROBE
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcpip.h>
#include <atomic>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <fcntl.h>
#include <unistd.h>
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif
#if __has_include("wifi_secret.hpp")
#include "wifi_secret.hpp"
#else
#define NAWSERVER_DEMO_WIFI_SSID ""
#define NAWSERVER_DEMO_WIFI_PASSWORD ""
#endif

namespace
{
EventGroupHandle_t network_events;
constexpr EventBits_t got_address=1;
#if NAWSERVER_DEMO_MEMORY_PROBE || NAWSERVER_DEMO_MEMORY_SAMPLER
void memoryProbe(const char *stage, unsigned cycle)
{
 const auto caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
 const auto free=heap_caps_get_free_size(caps);
 const auto total=heap_caps_get_total_size(caps);
 const auto largest=heap_caps_get_largest_free_block(caps);
 const auto psram_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT;
 printf("MEMORY_PROBE stage=%s cycle=%u free=%u total=%u largest=%u internal_min=%u psram_free=%u psram_total=%u psram_largest=%u psram_min=%u\n",
  stage,cycle,unsigned(free),unsigned(total),unsigned(largest),unsigned(heap_caps_get_minimum_free_size(caps)),
  unsigned(heap_caps_get_free_size(psram_caps)),unsigned(heap_caps_get_total_size(psram_caps)),
  unsigned(heap_caps_get_largest_free_block(psram_caps)),unsigned(heap_caps_get_minimum_free_size(psram_caps)));
}
#endif
#if NAWSERVER_DEMO_MEMORY_PROBE || NAWSERVER_DEMO_FULL_STOP_PROBE
struct TcpProbeState
{
 std::atomic<bool> done{false};
 unsigned active=0, time_wait=0, time_wait_80=0, time_wait_81=0, bound=0;
};
TcpProbeState tcp_probe;
void sampleTcpState(void *)
{
 tcp_probe.active=0;tcp_probe.time_wait=0;tcp_probe.time_wait_80=0;tcp_probe.time_wait_81=0;tcp_probe.bound=0;
 for(auto *p=tcp_active_pcbs;p;p=p->next)++tcp_probe.active;
 for(auto *p=tcp_tw_pcbs;p;p=p->next)
 {
  ++tcp_probe.time_wait;
  if(p->local_port==80)++tcp_probe.time_wait_80;
  if(p->local_port==81)++tcp_probe.time_wait_81;
 }
 for(auto *p=tcp_bound_pcbs;p;p=p->next)++tcp_probe.bound;
 tcp_probe.done.store(true,std::memory_order_release);
}
void reportTcpState(const char *stage,unsigned seconds)
{
 tcp_probe.done.store(false,std::memory_order_release);
 if(tcpip_callback(sampleTcpState,nullptr)!=ERR_OK){printf("TCP_PROBE stage=%s error=queue\n",stage);return;}
 for(unsigned n=0;n<100&&!tcp_probe.done.load(std::memory_order_acquire);++n)vTaskDelay(pdMS_TO_TICKS(10));
 if(!tcp_probe.done.load(std::memory_order_acquire)){printf("TCP_PROBE stage=%s error=timeout\n",stage);return;}
 vTaskDelay(pdMS_TO_TICKS(20));
 printf("TCP_PROBE stage=%s elapsed=%u active=%u time_wait=%u tw_port80=%u tw_port81=%u bound=%u\n",stage,seconds,tcp_probe.active,tcp_probe.time_wait,tcp_probe.time_wait_80,tcp_probe.time_wait_81,tcp_probe.bound);
}
#endif
#if NAWSERVER_DEMO_MEMORY_PROBE
esp_err_t nativeBenchmark(httpd_req_t *request)
{
 memoryProbe("native_get_active",0);
 httpd_resp_set_type(request,"text/plain");
 httpd_resp_set_hdr(request,"Connection","close");
 const auto result=httpd_resp_send(request,"OK",2);
 memoryProbe("native_get_sent",0);
 return result;
}
void measureNativeHttpd()
{
 // Keep Wi-Fi, platform and task settings the same throughout; this measures
 // a native server with one short route, not the complete compact feature set.
 for(unsigned cycle=0;cycle<3;++cycle)
 {
  vTaskDelay(pdMS_TO_TICKS(1000));
  memoryProbe("native_before",cycle);
  httpd_config_t config=HTTPD_DEFAULT_CONFIG();
  config.server_port=81;
  config.stack_size=8192;
  // lwIP has seven sockets; IDF HTTPD reserves three of them internally.
  config.max_open_sockets=4;
  httpd_handle_t handle=nullptr;
  const auto result=httpd_start(&handle,&config);
  printf("MEMORY_PROBE stage=native_start_result cycle=%u result=%d\n",cycle,int(result));
  if(result!=ESP_OK)std::abort();
  httpd_uri_t route{};route.uri="/benchmark";route.method=HTTP_GET;route.handler=nativeBenchmark;
  ESP_ERROR_CHECK(httpd_register_uri_handler(handle,&route));
  vTaskDelay(pdMS_TO_TICKS(1000));
  memoryProbe("native_started",cycle);
  vTaskDelay(pdMS_TO_TICKS(4000));
  memoryProbe("native_get_drained",cycle);
  ESP_ERROR_CHECK(httpd_stop(handle));
  vTaskDelay(pdMS_TO_TICKS(1000));
  memoryProbe("native_stopped",cycle);
 }
 reportTcpState("native_stopped",0);
}
class EmptyHandler final : public NaWServer::HttpRequestHandler
{
public:
 NaWServer::ExchangeDisposition handle(NaWServer::HttpExchange &exchange) override
 {
  if(exchange.head().target!="/benchmark")return NaWServer::ExchangeDisposition::Drop;
  memoryProbe("naw_get_active",0);
  NaWServer::Response response(exchange.responseTransport());
  const auto result=response.text(200,"OK");
  memoryProbe("naw_get_sent",0);
  return result==NaWServer::SendResult::Ok ? NaWServer::ExchangeDisposition::Complete : NaWServer::ExchangeDisposition::Drop;
 }
};
#endif
void networkEvent(void *,esp_event_base_t base,std::int32_t id,void *)
{
 if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_START)(void)esp_wifi_connect();
 if(base==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){xEventGroupClearBits(network_events,got_address);(void)esp_wifi_connect();}
 if(base==IP_EVENT&&id==IP_EVENT_STA_GOT_IP)xEventGroupSetBits(network_events,got_address);
}
void connectNetwork()
{
 ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());
 network_events=xEventGroupCreate();if(!network_events)std::abort();
 esp_netif_create_default_wifi_sta();wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
 ESP_ERROR_CHECK(esp_wifi_init(&init));ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
 ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
 ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,networkEvent,nullptr));
 ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,networkEvent,nullptr));
 wifi_config_t wifi{};
 strlcpy(reinterpret_cast<char*>(wifi.sta.ssid),NAWSERVER_DEMO_WIFI_SSID,sizeof(wifi.sta.ssid));
 strlcpy(reinterpret_cast<char*>(wifi.sta.password),NAWSERVER_DEMO_WIFI_PASSWORD,sizeof(wifi.sta.password));
 ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&wifi));
 ESP_ERROR_CHECK(esp_wifi_start());
}
}
extern "C" void app_main()
{
 setvbuf(stdout,nullptr,_IONBF,0);
 ESP_ERROR_CHECK(nvs_flash_init()); // Never erase another application's settings.
 connectNetwork();
#if NAWSERVER_DEMO_MEMORY_LINK_PROFILE
 compact::measurement::linkProfile();
#else
#if NAWSERVER_DEMO_MEMORY_BREAKDOWN
 const auto measurement_ready=xEventGroupWaitBits(network_events,got_address,pdFALSE,pdFALSE,pdMS_TO_TICKS(60000));
 if(!(measurement_ready&got_address))esp_restart();
 vTaskDelay(pdMS_TO_TICKS(3000));
 compact::measurement::snapshot("network_ready");
#endif
 static compact::EspPlatform platform;
#if NAWSERVER_DEMO_MEMORY_BREAKDOWN
 compact::measurement::snapshot("platform_constructed");
#endif
 if(!platform.initialize()){printf("COMPACT key/settings initialization failed\n");esp_restart();}
#if NAWSERVER_DEMO_MEMORY_BREAKDOWN
 compact::measurement::snapshot("platform_initialized");
#endif
 const auto ready=xEventGroupWaitBits(network_events,got_address,pdFALSE,pdFALSE,pdMS_TO_TICKS(60000));
 if(!(ready&got_address)){printf("COMPACT network timeout\n");esp_restart();}
#if NAWSERVER_DEMO_README_PROFILE
 compact::readme::run(platform);
#endif
#if NAWSERVER_DEMO_MEMORY_BREAKDOWN
 compact::measurement::run(platform);
#endif
 #if NAWSERVER_DEMO_MEMORY_PROBE
 vTaskDelay(pdMS_TO_TICKS(5000));
 for(unsigned cycle=0;cycle<3;++cycle){memoryProbe("baseline",cycle);vTaskDelay(pdMS_TO_TICKS(1000));}
 measureNativeHttpd();
 memoryProbe("naw_before_transport",0);
 #endif
#if NAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS
 static compact::TransportProbe transport;
#else
 static NaWServer::tcp::IdfTransport transport;
#endif
 if(transport.start().status!=NaWServer::tcp::Status::Ok)esp_restart();
 #if NAWSERVER_DEMO_MEMORY_PROBE
 memoryProbe("naw_transport_started",0);
 auto benchmark_options=compact::Demo::options();benchmark_options.port=81;
 auto *benchmark_backend=new(std::nothrow) NaWServer::TcpHttpBackend(transport,benchmark_options);
 if(!benchmark_backend)std::abort();
 memoryProbe("naw_backend_constructed",0);
 {
  EmptyHandler empty;
  for(unsigned cycle=0;cycle<3;++cycle)
  {
   if(benchmark_backend->start(empty,compact::Demo::limits())!=NaWServer::HttpError::Ok)std::abort();
   vTaskDelay(pdMS_TO_TICKS(1000));
   memoryProbe("naw_empty_started",cycle);
   vTaskDelay(pdMS_TO_TICKS(4000));
   memoryProbe("naw_get_drained",cycle);
   if(benchmark_backend->stop()!=NaWServer::HttpError::Ok)std::abort();
   vTaskDelay(pdMS_TO_TICKS(1000));
   memoryProbe("naw_empty_stopped",cycle);
  }
  delete benchmark_backend;
  memoryProbe("naw_backend_deleted",0);
  // Stop leaves no listener here. Observe whether network/allocator state
  // settles; a short-lived reduction alone does not establish a leak.
  reportTcpState("naw_stopped",0);
  for(unsigned interval=1;interval<=15;++interval)
  {
   vTaskDelay(pdMS_TO_TICKS(10000));
   memoryProbe("naw_stopped_settle",interval*10);
   if(interval%3==0)reportTcpState("naw_stopped",interval*10);
  }
 }
 #endif
 #if NAWSERVER_DEMO_MEMORY_SAMPLER
 memoryProbe("sampler_before_backend",0);
 #endif
 static NaWServer::TcpHttpBackend backend(transport,compact::Demo::options());
 #if NAWSERVER_DEMO_MEMORY_PROBE || NAWSERVER_DEMO_MEMORY_SAMPLER
 memoryProbe("naw_production_backend_constructed",0);
 #endif
#if NAWSERVER_DEMO_TX_BULK_PROBE
 compact::txBulkProbe().bind(backend);
#endif
 static compact::Demo demo(backend,platform);
 #if NAWSERVER_DEMO_MEMORY_PROBE || NAWSERVER_DEMO_MEMORY_SAMPLER
 memoryProbe("naw_demo_constructed",0);
 #endif
 if(!demo.start()){printf("COMPACT server start failed\n");esp_restart();}
 #if NAWSERVER_DEMO_MEMORY_PROBE
 for(unsigned cycle=0;cycle<3;++cycle){vTaskDelay(pdMS_TO_TICKS(1000));memoryProbe("naw_started",cycle);}
 #elif NAWSERVER_DEMO_MEMORY_SAMPLER
 memoryProbe("naw_started",0);
 #endif
#if NAWSERVER_DEMO_FULL_STOP_PROBE
	// The serial observer may issue public read-only HTTP requests here. No
	// producer is running in app_main yet, and durable settings remain untouched.
	puts("FULL_DEMO_PROBE_READY");
	vTaskDelay(pdMS_TO_TICKS(5000));
	unsigned idle_samples = 0;
	for(unsigned attempt = 0; attempt < 100 && idle_samples < 10; ++attempt)
	{
		const auto transmission = backend.transmissionStats();
		const bool idle = !demo.activeStreams() && transport.ownedHandles() == 1 && !transmission.queued_jobs &&
		    !transmission.active_jobs && !transmission.filling_jobs;
		idle_samples = idle ? idle_samples + 1 : 0;
		vTaskDelay(pdMS_TO_TICKS(100));
	}
	const auto transmission = backend.transmissionStats();
	const bool idle = idle_samples == 10 && !demo.activeStreams() && transport.ownedHandles() == 1 &&
	    !transmission.queued_jobs && !transmission.active_jobs && !transmission.filling_jobs;
	if(!idle)
	{
		// A browser/local status monitor request may still be active. Skip this optional
		// measurement instead of closing a user-owned active session for a number.
		puts("FULL_DEMO_STOP status=inconclusive reason=busy");
	}
	else
	{
		memoryProbe("full_demo_before_stop", 0);
		const bool stopped = demo.stop();
		const auto remaining_handles = transport.ownedHandles();
		const auto remaining_buffers = backend.bufferMemoryStats().used_bytes;
		printf("FULL_DEMO_STOP status=%s handles=%u buffer_bytes=%u\n",
		    stopped && !remaining_handles && !remaining_buffers ? "ok" : "failed",
		    unsigned(remaining_handles), unsigned(remaining_buffers));
		memoryProbe("full_demo_stopped", 0);
		reportTcpState("full_demo_stopped", 0);
		if(!stopped || remaining_handles || remaining_buffers) std::abort();

		if(!demo.start()) std::abort();

		vTaskDelay(pdMS_TO_TICKS(1000));
		memoryProbe("full_demo_restarted", 0);
	}
#endif
 #if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
 usb_serial_jtag_driver_config_t console_config{128,128};
 if(!usb_serial_jtag_is_driver_installed())ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&console_config));
 usb_serial_jtag_vfs_use_driver();
#endif
 fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL,0)|O_NONBLOCK);
 printf("COMPACT_READY platform=%s firmware=%s slot=%s max_firmware=%u\n",platform.status().platform,platform.status().firmware,platform.status().running_slot,unsigned(platform.maximumFirmwareBytes()));
#if NAWSERVER_DEMO_MEMORY_SAMPLER
 memoryProbe("sampler_ready",0);
#endif
 for(;;)
 {
  demo.tick();
#if NAWSERVER_DEMO_MEMORY_SAMPLER
  // Diagnostic-only sampling stays outside library and ISR/flash critical paths.
  static std::uint64_t next_sample=0;
  const auto now=static_cast<std::uint64_t>(esp_timer_get_time()/1000);
  if(now>=next_sample){
   next_sample=now+1000;memoryProbe("runtime_sample",unsigned(now/1000));
#if NAWSERVER_DEMO_NATIVE_ACCEPT_DIAGNOSTICS
   transport.sample(unsigned(now/1000));
   const auto buffers=backend.bufferMemoryStats();
   printf("BUFFER_PROBE cycle=%u used=%u peak=%u limit=%u budget_rejections=%u allocation_failures=%u blocked=%u\n",
    unsigned(now/1000),unsigned(buffers.used_bytes),unsigned(buffers.peak_bytes),unsigned(buffers.limit_bytes),
    unsigned(buffers.budget_rejections),unsigned(buffers.allocation_failures),unsigned(buffers.blocked));
#endif
  }
#endif
  vTaskDelay(pdMS_TO_TICKS(20));
 }
#endif
}
