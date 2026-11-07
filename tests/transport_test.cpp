// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/handler_static.h>
#include <NaWServer/response.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
namespace { unsigned checks = 0; }
#define CHECK(condition) do { ++checks; if(!(condition)) { std::printf("FAIL static transport line %d: %s\n", __LINE__, #condition); std::abort(); } } while(false)
using namespace NaWServer;
namespace
{
	struct Capture
	{
		unsigned calls{};
		std::string mime, encoding, body;
		static SendResult
		send(void *context, int, const char *mime, const Header *headers, std::size_t count, ByteView body)
		{
			auto &self = *static_cast<Capture *>(context);
			++self.calls;
			self.mime = mime;
			self.encoding.clear();
			self.body.assign(body.size() ? reinterpret_cast<const char *>(body.data()) : "", body.size());
			for(std::size_t i = 0; i < count; ++i)
				if(!std::strcmp(headers[i].name, "Content-Encoding"))
					self.encoding = headers[i].value;

			return SendResult::Ok;
		}

		ResponseTransport transport()
		{
			return {this, send};
		}

	};
	void static_checks()
	{
		Capture capture;
		const uint8_t binary[]{0, 255, 0};
		HandlerStaticUriBin raw("/raw", binary, sizeof(binary));
		{
			Response response(capture.transport());
			static_cast<HandlerBase &>(raw).handleRequest(nullptr, response);
		}
		CHECK(capture.mime == "application/octet-stream" && capture.encoding.empty());
		CHECK(capture.body == std::string(reinterpret_cast<const char *>(binary), sizeof(binary)));
		HandlerStaticUriBin gzip("/app.js", binary, sizeof(binary), "text/javascript", true);
		{
			Response response(capture.transport());
			static_cast<HandlerBase &>(gzip).handleRequest(nullptr, response);
		}
		CHECK(capture.mime == "text/javascript" && capture.encoding == "gzip");
		HandlerStaticUriText css("/style.css", "a{}", 3, "text/css");
		{
			Response response(capture.transport());
			static_cast<HandlerBase &>(css).handleRequest(nullptr, response);
		}
		CHECK(capture.mime == "text/css" && capture.encoding.empty() && capture.body == "a{}");
		HandlerStaticUriText html("/", "hello");
		{
			Response response(capture.transport());
			static_cast<HandlerBase &>(html).handleRequest(nullptr, response);
		}
		CHECK(capture.mime == "text/html");
		for(const char *invalid : {static_cast<const char *>(nullptr), "", "text/plain\r\nX: bad"})
		{
			const auto before = capture.calls;
			HandlerStaticUriBin bad("/bad", binary, sizeof(binary), invalid);
			Response response(capture.transport());
			static_cast<HandlerBase &>(bad).handleRequest(nullptr, response);
			CHECK(response.failed() && capture.calls == before);
		}
	}


}
int main()
{
	static_checks();
	std::printf("PASS static transport: %u checks\n", checks);
}
