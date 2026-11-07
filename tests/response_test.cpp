// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/response.h>
#include "auth/basic_auth.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
using namespace NaWServer;
namespace
{
	unsigned checks;
#define CHECK(test)                                                                                                    \
		do                                                                                                                 \
		{                                                                                                                  \
			++checks;                                                                                                      \
			if(!(test))                                                                                                    \
			{                                                                                                              \
				std::printf("FAIL response line %d: %s\n", __LINE__, #test);                                               \
				std::abort();                                                                                              \
			}                                                                                                              \
		} while(0)
	struct Capture
	{
		unsigned calls{};
		int status{};
		const uint8_t *body_address{};
		std::size_t length{};
		char type[64]{};
		Header fields[Response::max_headers]{};
		char names[Response::max_headers][48]{};
		char values[Response::max_headers][128]{};
		std::size_t count{};
		SendResult result{SendResult::Ok};
		static SendResult
		send(void *object, int status, const char *mime, const Header *headers, std::size_t count, ByteView body)
		{
			auto &self = *static_cast<Capture *>(object);
			++self.calls;
			self.status = status;
			self.body_address = body.data();
			self.length = body.size();
			std::snprintf(self.type, sizeof(self.type), "%s", mime);
			self.count = count;
			for(std::size_t i = 0; i < count; ++i)
			{
				std::snprintf(self.names[i], sizeof(self.names[i]), "%s", headers[i].name);
				std::snprintf(self.values[i], sizeof(self.values[i]), "%s", headers[i].value);
				self.fields[i] = {self.names[i], self.values[i]};
			}
			return self.result;
		}

		ResponseTransport transport()
		{
			return {this, send};
		}

		const char *get(const char *name) const
		{
			for(std::size_t i = 0; i < count; ++i)
				if(std::strcmp(fields[i].name, name) == 0)
					return fields[i].value;

			return "";
		}

	};
} // namespace
void run_response_checks()
{
	checks = 0;
	static_assert(!std::is_copy_constructible_v<Response> && !std::is_move_constructible_v<Response>);
	constexpr Header common[]{{"X-Common", "default"}, {"Cache-Control", "public"}};
	constexpr ResponseOptions json{ContentType::Json, CachePolicy::NoStore, HeaderFlags::NoSniff};
	static Capture capture;
	{
		Response response(capture.transport(), DefaultHeaders{common}, json);
		char local[] = "private";
		CHECK(response.header("cache-control", local) == SendResult::Ok);
		local[0] = 'X';
		CHECK(capture.calls == 0 && response.state() == ResponseState::Preparing);
		const uint8_t binary[]{0, 255, 1, 0};
		CHECK(response.bytes(201, binary, sizeof(binary)) == SendResult::Ok);
		CHECK(capture.body_address == binary && capture.length == sizeof(binary));
		CHECK(capture.status == 201 && std::strcmp(capture.type, "application/json") == 0);
		CHECK(std::strcmp(capture.get("cache-control"), "private") == 0);
		CHECK(std::strcmp(capture.get("X-Content-Type-Options"), "nosniff") == 0);
		CHECK(std::strcmp(capture.get("X-Common"), "default") == 0 && capture.count == 3);
		CHECK(response.state() == ResponseState::Complete && !response.failed());
		CHECK(response.text(200, "duplicate") == SendResult::AlreadySent && capture.calls == 1);
		CHECK(response.failed());
	}
	capture = {};
	{
		Response response(capture.transport());
		CHECK(response.empty() == SendResult::Ok && capture.status == 204 && capture.length == 0);
		CHECK(response.header("X-Late", "value") == SendResult::AlreadySent);
	}
	capture = {};
	{
		Response response(capture.transport());
		CHECK(response.redirect("/health") == SendResult::Ok && capture.status == 302);
		CHECK(std::strcmp(capture.get("Location"), "/health") == 0);
	}
	capture = {};
	{
		Response response(capture.transport());
		CHECK(response.header("X-Test", "line\r\nInjected: yes") == SendResult::InvalidArgument);
		CHECK(response.text(200, "must not send") == SendResult::InvalidArgument && capture.calls == 0);
	}
	for(const char *name :
	    {"", "bad name", "Content-Length", "transfer-encoding", "Connection", "Content-Type", "cOnNeCtIoN"})
	{
		Response response(capture.transport());
		CHECK(response.header(name, "value") == SendResult::InvalidArgument);
	}
	{
		Response response(capture.transport());
		CHECK(response.header(std::string_view("X\0Bad", 5), "value") == SendResult::InvalidArgument);
	}
	{
		Response response(capture.transport());
		CHECK(response.header("X-Bad", std::string_view("a\0b", 3)) == SendResult::InvalidArgument);
	}
	{
		Response response(capture.transport());
		char huge[Response::header_storage + 1];
		std::memset(huge, 'x', sizeof(huge));
		CHECK(response.header("X", std::string_view(huge, sizeof(huge))) == SendResult::BufferTooSmall);
		CHECK(!response.started() && response.failed());
	}
	{
		Response response(capture.transport());
		char name[24];
		for(unsigned i = 0; i < Response::max_headers; ++i)
		{
			std::snprintf(name, sizeof(name), "X-%u", i);
			CHECK(response.header(name, "v") == SendResult::Ok);
		}
		CHECK(response.header("X-Overflow", "v") == SendResult::HeaderLimit);
	}
	{
		Response response(capture.transport());
		CHECK(response.bytes(200, nullptr, 1) == SendResult::InvalidArgument);
	}
	{
		Response response(capture.transport());
		CHECK(response.text(204, "body") == SendResult::InvalidArgument);
	}
	{
		Response response(capture.transport());
		CHECK(response.text(101, "upgrade") == SendResult::InvalidArgument);
	}
	capture = {};
	capture.result = SendResult::TransportError;
	{
		Response response(capture.transport());
		CHECK(response.text(200, "payload") == SendResult::TransportError);
		CHECK(response.state() == ResponseState::Failed);
		CHECK(response.empty() == SendResult::AlreadySent && capture.calls == 1);
	}
	{
		Response response({});
		CHECK(response.empty() == SendResult::TransportError);
	}
	{
		TextBuffer<32> buffer;
		CHECK(buffer.print("value=") && buffer.printf("%d", 42) && buffer.print(1.5f));
		CHECK(buffer.view() == "value=421.500000" && buffer.valid());
		TextBuffer<4> small;
		CHECK(small.print("abc"));
		CHECK(!small.print("d") && small.view() == "abc");
		CHECK(!small.printf("%s", "x"));
		TextBuffer<4> formatted;
		CHECK(formatted.print("a"));
		CHECK(!formatted.printf("%s", "long"));
		CHECK(formatted.view() == "a" && !formatted.valid());
	}
	{
		Response response(capture.transport());
		unsigned aborted = 0;
		const ExternalControl control{
			&aborted,
			[](void *state)
			{
				++*static_cast<unsigned *>(state);
			}};
		CHECK(response.claimExternal(control) == SendResult::Ok);
		CHECK(response.state() == ResponseState::External);
		CHECK(response.text(200, "must not send") == SendResult::AlreadySent);
		CHECK(aborted == 1 && response.state() == ResponseState::External);
		response.fail();
		CHECK(aborted == 2 && response.state() == ResponseState::External);
	}
	{
		Response response(capture.transport());
		TextBuffer<2> invalid;
		CHECK(!invalid.print("too long"));
		CHECK(response.text(200, invalid) == SendResult::BufferTooSmall);
		CHECK(!response.started());
	}
	{
		Capture closed;
		Response response(
			closed.transport(),
			{},
			{ContentType::Default, CachePolicy::NoStore, HeaderFlags::NoSniff, ConnectionPolicy::Close});
		CHECK(response.header("Retry-After", "2") == SendResult::Ok);
		CHECK(response.text(503, "busy") == SendResult::Ok);
		bool connection = false, retry = false;
		for(std::size_t i = 0; i < closed.count; ++i)
		{
			if(std::strcmp(closed.fields[i].name, "Connection") == 0)
				connection = std::strcmp(closed.fields[i].value, "close") == 0;

			if(std::strcmp(closed.fields[i].name, "Retry-After") == 0)
				retry = std::strcmp(closed.fields[i].value, "2") == 0;
		}
		CHECK(connection && retry && closed.status == 503);
	}
	CHECK(verifyBasicAuthorization("Basic dGVzdDp0ZXN0LW9ubHk=", "test", "test-only"));
	CHECK(verifyBasicAuthorization("bAsIc  dGVzdDp0ZXN0LW9ubHk=", "test", "test-only"));
	CHECK(!verifyBasicAuthorization("Basic dGVzdDp0ZXN0LW9ubHk=", "test", "wrong"));
	CHECK(!verifyBasicAuthorization("Basic dGVzdDp0ZXN0LW9ubHk=", "", "test-only"));
	CHECK(!verifyBasicAuthorization("Basic dGVzdDp0ZXN0LW9ubHk=", "test", ""));
	for(const char *bad :
	    {"",
	     "Basic",
	     "Bearer dGVzdDp0ZXN0LW9ubHk=",
	     "Basic dGVzdDp0ZXN0LW9ubHk",
	     "Basic ====",
	     "Basic dGVzdDp0ZXN0LW9ubHk=AAAA",
	     "Basic dGVzdDp0ZXN0LW9ubHl=",
	     "Basic dGVzdDp0ZXN0LW9ubHk= "})
		CHECK(!verifyBasicAuthorization(bad, "test", "test-only"));
	CHECK(verifyBasicAuthorization("Basic YTpi", "a", "b"));
	CHECK(verifyBasicAuthorization("Basic YTpiYw==", "a", "bc"));
	CHECK(verifyBasicAuthorization("Basic YTpiY2Q=", "a", "bcd"));
	CHECK(!verifyBasicAuthorization("Basic YTpiYx==", "a", "bc"));
	CHECK(!verifyBasicAuthorization("Basic YTpiY2R=", "a", "bcd"));
	std::printf("PASS: %u response/basic checks; Response=%u bytes\n", checks, static_cast<unsigned>(sizeof(Response)));
}

#ifndef ESP_PLATFORM
int main()
{
	run_response_checks();
}

#endif
