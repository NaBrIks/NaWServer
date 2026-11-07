// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/sse.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/backends/esp_idf_tcp.h>
#include <NaWServer/backends/tcp.h>
#include "checks.hpp"
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/websocket.h>
#include <NaWServer/handler_middleware.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if __has_include("wifi_secret.hpp")
#include "wifi_secret.hpp" // existing ignored test network configuration; never printed
#else
#define NAWSERVER_DEMO_WIFI_SSID ""
#define NAWSERVER_DEMO_WIFI_PASSWORD ""
#endif
using namespace NaWServer;
using namespace NaWServer::sse;
namespace
{
	tcp::IdfTransport transport;
	TcpOptions tcp_options()
	{
		TcpOptions options;
		options.bind_address = "0.0.0.0";
		options.port = 80;
		return options;
	}
	TcpHttpBackend backend(transport, tcp_options());
	TcpAdapter event_backend(backend, Limits{4, 3, 2304, 8192, 500, 1000, 3000});
	EventSource adapter(event_backend);
	// Isolated low-level rollback probe. Public EventSource hides reserve/activate;
	// this second test-only instance is never used to publish or service streams.
	TcpAdapter rollback_probe(backend, Limits{1, 1, 768, 768, 500, 1000, 3000});
	AdapterStats combined_stats()
	{
		auto state = adapter.stats();
		const auto probe = rollback_probe.stats();
		state.active += probe.active;
		state.queued_bytes += probe.queued_bytes;
		state.begun += probe.begun;
		state.completed += probe.completed;
		state.rolled_back += probe.rolled_back;
		return state;
	}

	std::array<Token, client_capacity> tokens{}; // only HTTP callback accesses this table
	std::atomic<bool> stop_requested{false};
	std::uint64_t producer_max_us = 0;
	FixedBasicAuthenticator users("test", "test-only", 1);
	FixedBasicAuthenticator forbidden_user("test", "test-only", 0);
	AuthMiddleware auth(users), forbidden_auth(forbidden_user);
	AccessMiddleware access_check;
	auto protected_chain = makeHeaderChain(auth, access_check);
	auto forbidden_chain = makeHeaderChain(forbidden_auth, access_check);
	constexpr Header defaults[] = {{"X-SSE-Test", "default"}, {"X-Common", "server"}};
	constexpr ResponseOptions stream_options{ContentType::Default, CachePolicy::NoStore, HeaderFlags::NoSniff};
	[[gnu::noinline]] bool queue_connected(Token token, const LastEventId &last_id)
	{
		Frame first;
		const auto id = std::string_view(last_id.bytes.data(), last_id.size);
		const auto value = last_id.present ? id : std::string_view("<absent>");
		return encode({value, "connected", {}, {}}, first) == EncodeResult::Ok &&
		       adapter.publish(token, first) == Publish::Queued;
	}

	SendResult open_event(Request &request, Response &response)
	{
		const std::string local = "owned callback value";
		if(response.header("x-common", local) != SendResult::Ok)
			return response.error();

		const auto opened = adapter.open(request, response);
		if(opened.result == SendResult::Ok)
		{
			tokens[opened.token.slot] = opened.token;
			// Busy is an explicit publication refusal, so close instead of pretending
			// that this fixture's initial event was delivered.
			if(!queue_connected(opened.token, opened.last_id))
			{
				response.fail();
				return SendResult::TransportError;
			}

			return SendResult::Ok;
		}

		if(response.state() == ResponseState::External)
			return opened.result;

		const int status = (opened.admission == Admit::InvalidLastId || opened.admission == Admit::InvalidRequest)
		                       ? 400
		                       : 503;
		return response.text(status, "SSE admission rejected");
	}

	Result open_context(Context &ctx)
	{
		return responseResult(open_event(ctx.request, ctx.response));
	}

	struct AfterError
	{
		Result operator()(Context &, Next &next)
		{
			(void)next();
			return Result::Error;
		}

	} after_error;
	auto error_chain = makeChain(after_error);
	SendResult duplicate(Request &request, Response &response)
	{
		if(open_event(request, response) != SendResult::Ok)
			return response.error();

		return response.text(200, "must never appear in SSE");
	}

	SendResult rollback_fixture(Request &request, Response &response)
	{
		std::array<Header, Response::max_headers> resolved{};
		std::size_t count = 0;
		if(response.resolveHeaders(resolved, count) != SendResult::Ok)
			return response.error();

		Frame preface;
		if(encode_headers(resolved.data(), count, preface) != SendResult::Ok)
			return SendResult::InvalidArgument;

		{
			sse::Backend::Reservation reservation;
			if(rollback_probe.reserve(request, preface, reservation) != Admit::Owned)
				return response.text(503, "reserve rejected");

			// Scoped reservation is deliberately not claimed/activated. Its destructor
			// releases the owned reservation and restores this ordinary response path.
		}
		return response.text(409, "reservation rolled back");
	}

	SendResult publish(Request &request, Response &response)
	{
		const auto kind = request.parameter("case").value_or("utf8");
		Frame frame;
		EncodeResult encoded = EncodeResult::InvalidField;
		if(kind == "utf8")
			encoded = encode({u8"Русский 中文 日本語 🙂\nline\n", "reading", "boot:42", 500}, frame);

		if(kind == "empty")
			encoded = encode({"", "reading", "", {}}, frame);

		if(kind == "nul")
			encoded = encode({std::string_view("a\0b", 3), "reading", {}, {}}, frame);

		if(kind == "large")
			encoded = encode({std::string(700, 'z'), "reading", {}, {}}, frame);

		if(encoded != EncodeResult::Ok)
			return response.text(400, "unknown event fixture");

		std::array<unsigned, 7> results{};
		const auto target = request.parameter("slot");
		int selected = -1;
		if(target && target->size() == 1 && (*target)[0] >= '0' && (*target)[0] <= '3')
			selected = (*target)[0] - '0';

		unsigned burst = 1;
		if(auto value = request.parameter("burst");
		    value && value->size() == 1 && (*value)[0] >= '1' && (*value)[0] <= '4')
			burst = (*value)[0] - '0';

		for(unsigned repeat = 0; repeat < burst; ++repeat)
			for(std::size_t i = 0; i < tokens.size(); ++i)
			{
				if(selected >= 0 && static_cast<int>(i) != selected)
					continue;

				const auto started = esp_timer_get_time();
				const auto result = adapter.publish(tokens[i], frame);
				producer_max_us = std::max(producer_max_us, static_cast<std::uint64_t>(esp_timer_get_time() - started));
				++results[static_cast<std::size_t>(result)];
			}
		TextBuffer<256> output;
		output.printf(
			"{\"queued\":%u,\"closed\":%u,\"oversize\":%u,\"overflow\":%u,\"global\":%u,\"busy\":%u}",
			results[0],
			results[1],
			results[2],
			results[3],
			results[4],
			results[5]);
		return response.text(200, output);
	}

	SendResult health(Response &response)
	{
		const auto state = combined_stats();
		TextBuffer<512> output;
		output.printf(
			"{\"active\":%u,\"pending\":%u,\"begun\":%u,\"completed\":%u,\"rollback\":%u,\"sent\":%u,\"stall\":%u,"
			"\"overflow\":%u,\"heap\":%lu,\"producer_us\":%llu,\"stack_min\":%u}",
			static_cast<unsigned>(state.active),
			static_cast<unsigned>(state.queued_bytes),
			static_cast<unsigned>(state.begun),
			static_cast<unsigned>(state.completed),
			static_cast<unsigned>(state.rolled_back),
			static_cast<unsigned>(state.sent_frames),
			static_cast<unsigned>(state.closed_by_reason[static_cast<std::size_t>(CloseReason::Stall)]),
			static_cast<unsigned>(state.closed_by_reason[static_cast<std::size_t>(CloseReason::Overflow)]),
			static_cast<unsigned long>(esp_get_free_heap_size()),
			static_cast<unsigned long long>(producer_max_us),
			static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
		return response.send(
			200,
			"application/json",
			{reinterpret_cast<const uint8_t *>(output.view().data()), output.view().size()});
	}

	SendResult close_all(Response &response)
	{
		bool accepted = true;
		for(auto token : tokens)
			accepted = adapter.close(token) && accepted;
		return response.text(accepted ? 200 : 503, accepted ? "closing" : "retry close");
	}

	SendResult stop(Response &response)
	{
		const auto result = response.text(200, "owner will drain and stop");
		if(result == SendResult::Ok)
			stop_requested.store(true, std::memory_order_release);

		return result;
	}

	const char browser_html[] =
	    R"HTML(<!doctype html><meta charset="utf-8"><title>SSE RUNNING</title><pre id="result">RUNNING</pre><script>
(async()=>{let es;try{es=new EventSource('/events/open');let connected;const first=new Promise((r,j)=>{connected=r;es.addEventListener('connected',r,{once:true});es.onerror=()=>j(Error('SSE disconnected'))});await first;
const cases=[['utf8','Русский 中文 日本語 🙂\nline\n','boot:42'],['empty','',''],['nul','a\u0000b','']];
for(const [kind,data,id]of cases){const event=new Promise((r,j)=>{const timer=setTimeout(()=>j(Error('event timeout')),6000);es.addEventListener('reading',e=>{clearTimeout(timer);r(e)},{once:true})});const reply=await fetch('/control/publish?case='+kind,{method:'POST',headers:{Authorization:'Basic dGVzdDp0ZXN0LW9ubHk='}});if(!reply.ok)throw Error('publish failed');const e=await event;if(e.data!==data||e.lastEventId!==id)throw Error('payload/id mismatch: '+kind)}
es.close();document.title='SSE PASS 3';document.getElementById('result').textContent='PASS: UTF-8/multiline, empty/id reset, embedded NUL';}
catch(e){if(es)es.close();document.title='SSE FAIL';document.getElementById('result').textContent='FAIL: '+e.message}})();</script>)HTML";
	HandlerStaticUriText browser_page("/browser", browser_html);
	constexpr std::array<std::uint8_t, 4096> asset{};
	HandlerStaticUriBin asset_route("/asset", asset.data(), asset.size());
	WebSocketEndpoint websocket("/ws");
	Route events_route("/events", HttpMethod::Get, open_event, {}, AccessPolicy::require(1), stream_options, protected_chain);
	Route context_route(
		"/events/context",
		HttpMethod::Get,
		open_context, {}, AccessPolicy::require(1), stream_options, protected_chain);
	Route public_events("/events/open", HttpMethod::Get, open_event, {}, AccessPolicy::public_access(), stream_options);
	Route forbidden_route(
		"/events/forbidden",
		HttpMethod::Get,
		open_event, {}, AccessPolicy::require(1), stream_options, forbidden_chain);
	Route error_route("/events/error", HttpMethod::Get, open_context, error_chain, AccessPolicy::require(1), stream_options, protected_chain);
	Route duplicate_route(
		"/events/duplicate",
		HttpMethod::Get,
		duplicate, {}, AccessPolicy::require(1), stream_options, protected_chain);
	Route rollback_route("/events/rollback", HttpMethod::Get, rollback_fixture, {}, AccessPolicy::require(1), {}, protected_chain);
	Route publish_route("/control/publish", HttpMethod::Post, publish, {}, AccessPolicy::require(1), {}, protected_chain);
	Route close_route("/control/close", HttpMethod::Post, close_all, {}, AccessPolicy::require(1), {}, protected_chain);
	Route stop_route("/control/stop", HttpMethod::Post, stop, {}, AccessPolicy::require(1), {}, protected_chain);
	Route health_route("/health", HttpMethod::Get, health);
	HandlerBase *const routes[] = {
		&browser_page,
		&asset_route,
		&websocket,
		&events_route,
		&context_route,
		&public_events,
		&forbidden_route,
		&error_route,
		&duplicate_route,
		&rollback_route,
		&publish_route,
		&close_route,
		&stop_route,
		&health_route};
	HttpServer server(backend, routes, {}, DefaultHeaders(defaults));
	void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
	{
		if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
			esp_wifi_connect();

		if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
		{
			const auto *event = static_cast<ip_event_got_ip_t *>(data);
			ESP_LOGI("sse_lab", "SSE_READY ip=" IPSTR, IP2STR(&event->ip_info.ip));
		}
	}

} // namespace
extern "C" void app_main()
{
	static_assert(__cplusplus == 201703L, "C++17 required");
	vTaskDelay(pdMS_TO_TICKS(5000));
	std::printf("SSE_INTEGRATION_COMMON passed=%zu sizeof_adapter=%zu\n", checks(), sizeof(adapter));
	ESP_ERROR_CHECK(nvs_flash_init()); // no erase fallback; existing owner network only
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	if(!adapter.start())
		std::abort();

	if(!rollback_probe.start())
		std::abort();

	websocket.onMessage(
		[](WebSocketClient *client, const uint8_t *bytes, std::size_t size, bool binary)
	{
		if(binary)
			(void)client->send_binary(bytes, size);
		else
		{
			std::string text(reinterpret_cast<const char *>(bytes), size);
			(void)client->send(text.c_str());
		}
	});
	if(transport.start().status != tcp::Status::Ok) std::abort();
	if(server.begin() != HttpError::Ok) std::abort();
	if(!std::strlen(NAWSERVER_DEMO_WIFI_SSID))
	{
		ESP_LOGE("sse_lab", "No ignored Wi-Fi configuration");
		std::abort();
	}

	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi{};
	strlcpy(reinterpret_cast<char *>(wifi.sta.ssid), NAWSERVER_DEMO_WIFI_SSID, sizeof(wifi.sta.ssid));
	strlcpy(reinterpret_cast<char *>(wifi.sta.password), NAWSERVER_DEMO_WIFI_PASSWORD, sizeof(wifi.sta.password));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	ESP_ERROR_CHECK(esp_wifi_start());
	while(!stop_requested.load(std::memory_order_acquire))
		vTaskDelay(pdMS_TO_TICKS(100));
	// Only the application owner task stops services; never the URI callback.
	if(!adapter.stop(5000))
		std::abort();

	if(!rollback_probe.stop(5000))
		std::abort();

	if(server.end() != HttpError::Ok) std::abort();
	const auto state = combined_stats();
	if(state.active || state.queued_bytes || state.completed != state.begun || adapter.close_errors() ||
	    rollback_probe.close_errors())
		std::abort();

	std::printf(
		"SSE_INTEGRATION_DONE begun=%u completed=%u rollback=%u active=%u pending=%u producer_us=%llu heap=%lu\n",
		static_cast<unsigned>(state.begun),
		static_cast<unsigned>(state.completed),
		static_cast<unsigned>(state.rolled_back),
		static_cast<unsigned>(state.active),
		static_cast<unsigned>(state.queued_bytes),
		static_cast<unsigned long long>(producer_max_us),
		static_cast<unsigned long>(esp_get_free_heap_size()));
	for(;;)
		vTaskDelay(pdMS_TO_TICKS(1000));
}
