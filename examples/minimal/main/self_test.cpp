// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "sdkconfig.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <NaWServer/form_parameters.h>
#include <NaWServer/http_server.h>
#include "../../../tools/url_decode/utf8_fixtures.hpp"

namespace
{
	int connect_local()
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if(fd < 0)
			return -1;

		timeval timeout{3, 0};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(80);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
		{
			close(fd);
			return -1;
		}

		return fd;
	}

	bool write_all(int fd, const std::string &data)
	{
		size_t sent = 0;
		while(sent < data.size())
		{
			int n = send(fd, data.data() + sent, data.size() - sent, 0);
			if(n <= 0)
				return false;

			sent += n;
		}
		return true;
	}

	std::string request(
		const std::string &path,
		const char *method = "GET",
		const char *auth = nullptr,
		const std::string &body = "",
		bool partial = false,
		const char *content_type = "application/x-www-form-urlencoded")
	{
		int fd = connect_local();
		if(fd < 0)
			return "";

		std::string headers = std::string(method) + " " + path +
		    " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
		if(auth)
			headers += std::string("Authorization: Basic ") + auth + "\r\n";

		if(!body.empty())
		{
			if(content_type)
				headers += std::string("Content-Type: ") + content_type + "\r\n";

			headers += "Content-Length: " + std::to_string(body.size()) + "\r\n";
		}

		if(!write_all(fd, headers + "\r\n"))
		{
			close(fd);
			return "";
		}

		if(partial)
		{
			write_all(fd, body.substr(0, 4));
			vTaskDelay(pdMS_TO_TICKS(80));
			write_all(fd, body.substr(4));
		}
		else
			write_all(fd, body);

		std::string response;
		char buf[256];
		int n;
		while((n = recv(fd, buf, sizeof(buf), 0)) > 0 && response.size() < 8192)
		{
			response.append(buf, n);
			auto end = response.find("\r\n\r\n");
			auto length = response.find("Content-Length: ");
			if(end != std::string::npos && length != std::string::npos && length < end)
			{
				auto size = std::strtoul(response.c_str() + length + sizeof("Content-Length: ") - 1, nullptr, 10);
				if(response.size() >= end + 4 + size)
					break;
			}
		}
		close(fd);
		return response;
	}

	bool is_status(const std::string &response, int status)
	{
		return response.rfind("HTTP/1.1 " + std::to_string(status), 0) == 0;
	}

	bool has_body(const std::string &response, const std::string &expected)
	{
		const auto split = response.find("\r\n\r\n");
		return is_status(response, 200) && split != std::string::npos && response.substr(split + 4) == expected;
	}

	bool read_exact(int fd, uint8_t *data, size_t size)
	{
		size_t pos = 0;
		while(pos < size)
		{
			int n = recv(fd, data + pos, size - pos, 0);
			if(n <= 0)
				return false;

			pos += n;
		}
		return true;
	}

	bool ws_echo(bool binary, size_t length)
	{
		int fd = connect_local();
		if(fd < 0)
			return false;

		auto finish = [fd](bool result)
			{
				close(fd);
				return result;
			};
		if(!write_all(
				fd,
				"GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
				"dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"))
			return finish(false);

		std::string headers;
		uint8_t c;
		while(headers.size() < 2048 && headers.find("\r\n\r\n") == std::string::npos)
		{
			if(!read_exact(fd, &c, 1))
				return finish(false);

			headers += char(c);
		}
		if(!is_status(headers, 101))
			return finish(false);

		std::string payload(length, 'x');
		if(binary)
			for(size_t i = 0; i < length; ++i)
				payload[i] = char(i % 256);

		// A zero mask is legal; clients must set MASK and provide four mask bytes.
		std::string frame;
		frame += char(binary ? 0x82 : 0x81);
		if(length < 126)
			frame += char(0x80 | length);
		else
		{
			frame += char(0xfe);
			frame += char(length >> 8);
			frame += char(length);
		}

		frame.append(4, '\0');
		frame += payload;
		if(!write_all(fd, frame))
			return finish(false);

		uint8_t prefix[2];
		if(!read_exact(fd, prefix, 2))
			return finish(false);

		size_t received = prefix[1] & 127;
		if(received == 126)
		{
			uint8_t size[2];
			if(!read_exact(fd, size, 2))
				return finish(false);

			received = (size[0] << 8) | size[1];
		}

		if(received != length || (prefix[0] & 15) != (binary ? 2 : 1))
			return finish(false);

		std::string reply(received, '\0');
		if(!read_exact(fd, reinterpret_cast<uint8_t *>(reply.data()), reply.size()))
			return finish(false);

		return finish(reply == payload);
	}

} // namespace

int run_parameter_tests();
void run_loopback_tests(NaWServer::HttpServer &server)
{
	ESP_LOGI("selftest", "decoder=%s cplusplus=%lu", NaWServer::urlDecodeBackend(), (unsigned long)__cplusplus);
	unsigned passed = 0, failed = 0;
	auto check = [&](const char *name, bool result)
		{
			if(result)
				++passed;
			else
				++failed;

			ESP_LOGI("selftest", "%s %s", result ? "PASS" : "FAIL", name);
		};
	check("production decoder/property tests", run_parameter_tests() == 0);
	auto health = request("/health");
	check(
		"selected backend header",
		health.find(std::string("X-Url-Decoder: ") + NaWServer::urlDecodeBackend() + "\r\n") != std::string::npos);
	check("HTTP health", is_status(request("/health"), 200));
	check("HTTP 404", is_status(request("/missing"), 404));
	auto echo = request("/echo?value=a%20b%2Bc");
	check("URL decoding", is_status(echo, 200) && echo.find("\r\n\r\na b+c") != std::string::npos);
	check("Basic missing", is_status(request("/private"), 401));
	check("Basic empty", is_status(request("/private", "GET", ""), 401));
	check("Basic prefix", is_status(request("/private", "GET", "dGVz"), 401));
	check("Basic valid", is_status(request("/private", "GET", "dGVzdDp0ZXN0LW9ubHk="), 200));
	check("DELETE auth", is_status(request("/private", "DELETE"), 401));
	auto post = request("/echo", "POST", nullptr, "value=partial+post", true);
	check("partial POST", is_status(post, 200) && post.find("\r\n\r\npartial post") != std::string::npos);
	check("query decoded NUL", has_body(request("/echo?value=A%00B"), std::string("A\0B", 3)));
	check("raw NUL HTTP target rejected", is_status(request(std::string("/echo?value=A\0B", 15)), 400));
	check("query encoded key", has_body(request("/echo?v%61lue=works"), "works"));
	check("query empty value", has_body(request("/echo?value="), ""));
	check("query flag", has_body(request("/echo?value"), ""));
	check("query first duplicate", has_body(request("/echo?value=first&value=second"), "first"));
	check("query case sensitive", has_body(request("/echo?VALUE=wrong&value=right"), "right"));
	check("query separators decoded once", has_body(request("/echo?value=%2520%26%3D%2B+"), "%20&=+ "));
	check("form decoded NUL", has_body(request("/echo", "POST", nullptr, "value=A%00B"), std::string("A\0B", 3)));
	check(
		"form raw NUL length",
		has_body(request("/echo", "POST", nullptr, std::string("value=A\0B", 9)), std::string("A\0B", 3)));
	check(
		"form NUL before next pair",
		has_body(request("/echo", "POST", nullptr, std::string("other=A\0B&value=tail", 20)), "tail"));
	check("form first duplicate", has_body(request("/echo", "POST", nullptr, "value=first&value=second"), "first"));
	check("form overrides query", has_body(request("/echo?value=query", "POST", nullptr, "value=form"), "form"));
	check("empty form overrides query", has_body(request("/echo?value=query", "POST", nullptr, "value="), ""));
	check("form fallback query", has_body(request("/echo?value=query", "POST", nullptr, "other=form"), "query"));
	check(
		"form media type case/charset",
		has_body(
			request("/echo", "POST", nullptr, "value=works", false, "Application/X-WWW-Form-Urlencoded; charset=UTF-8"),
			"works"));
	for(const char *invalid : {"%", "%0", "%GG", "ok&ignored=%GG", "ok&value=%GG"})
	{
		auto path = std::string("/echo?value=") + invalid;
		check("invalid query rejected", is_status(request(path.c_str()), 400));
		check(
			"invalid form rejected",
			is_status(request("/echo?value=fallback", "POST", nullptr, std::string("value=") + invalid), 400));
	}
	check("invalid key rejected", is_status(request("/echo?%GG=value"), 400));
	check(
		"invalid query not hidden by form",
		is_status(request("/echo?ignored=%GG", "POST", nullptr, "value=valid"), 400));
	check(
		"non-form percent body ignored",
		has_body(
			request("/echo?value=query", "POST", nullptr, "value=%GG", false, "application/octet-stream"),
			"query"));
	check(
		"JSON body ignored",
		has_body(request("/echo?value=query", "POST", nullptr, "value=%GG", false, "application/json"), "query"));
	check(
		"untyped body ignored",
		has_body(request("/echo?value=query", "POST", nullptr, "value=%GG", false, nullptr), "query"));
	for(const auto &fixture : urlbench::unicode_fixtures())
	{
		check(
			"long Unicode form raw",
			has_body(request("/echo", "POST", nullptr, "value=" + fixture.raw), fixture.raw));
		check(
			"long Unicode form percent",
			has_body(request("/echo", "POST", nullptr, "value=" + fixture.percent), fixture.raw));
		// Keep query within the SDK's existing 512-byte URI bound, at a complete
		// UTF-8 code-point boundary; the long 3072-byte form exercises body length.
		size_t length = 120;
		while((static_cast<unsigned char>(fixture.raw[length]) & 0xc0) == 0x80)
			--length;
		const auto short_text = fixture.raw.substr(0, length);
		std::string raw_path = "/echo?value=";
		for(unsigned char b : short_text)
		{
			// A literal ASCII space cannot occur in an HTTP request-target.
			if(b == ' ')
				raw_path += "%20";
			else
				raw_path += static_cast<char>(b);
		}
		auto pct_path = std::string("/echo?value=") + urlbench::percent_encode_all(short_text);
		check("raw Unicode HTTP target accepted by shared parser", has_body(request(raw_path.c_str()), short_text));
		check("Unicode query percent", has_body(request(pct_path.c_str()), short_text));
	}
	check("HTTP health after malformed requests", is_status(request("/health"), 200));
	check("body limit", is_status(request("/echo", "POST", nullptr, std::string(4097, 'x')), 400));
	check("WS text 128", ws_echo(false, 128));
	check("WS empty text", ws_echo(false, 0));
	check("WS binary 4096 without PSRAM", ws_echo(true, 4096));
	ESP_LOGI(
		"selftest",
		"RESULT passed=%u failed=%u free_heap=%lu",
		passed,
		failed,
		(unsigned long)esp_get_free_heap_size());
	unsigned lifecycle_passed = 0, lifecycle_failed = 0;
	auto lifecycle_check = [&](const char *name, bool result)
		{
			if(result) ++lifecycle_passed;
			else ++lifecycle_failed;

			ESP_LOGI("lifecycle", "%s %s", result ? "PASS" : "FAIL", name);
		};
	for(unsigned cycle = 0; cycle < 3; ++cycle)
	{
		ESP_LOGI("lifecycle", "cycle=%u", cycle + 1);
		const auto stopped = server.end();
		lifecycle_check("end releases shared server ownership",
		    stopped == NaWServer::HttpError::Ok && !server.running() && !server.cleanupRequired());
		if(stopped != NaWServer::HttpError::Ok) break;

		lifecycle_check("repeated end", server.end() == NaWServer::HttpError::Ok);
		const int stopped_socket = connect_local();
		lifecycle_check("HTTP listener closed", stopped_socket < 0);
		if(stopped_socket >= 0) close(stopped_socket);

		const auto started = server.begin();
		lifecycle_check("begin restores shared server ownership",
		    started == NaWServer::HttpError::Ok && server.running() && !server.cleanupRequired());
		if(started != NaWServer::HttpError::Ok) break;

		lifecycle_check("repeated begin", server.begin() == NaWServer::HttpError::Ok);
		const int listening_socket = connect_local();
		lifecycle_check("HTTP listener restored", listening_socket >= 0);
		if(listening_socket >= 0) close(listening_socket);

		lifecycle_check("HTTP after restart", is_status(request("/health"), 200));
		lifecycle_check("HTTP auth after restart", is_status(request("/private"), 401));
		lifecycle_check("WS text after restart", ws_echo(false, 128));
		lifecycle_check("WS binary after restart", ws_echo(true, 4096));
	}
	ESP_LOGI("lifecycle", "RESULT passed=%u failed=%u running=%u free_heap=%lu",
	    lifecycle_passed, lifecycle_failed, server.running() ? 1U : 0U,
	    (unsigned long)esp_get_free_heap_size());
}
