// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/handler_middleware.h>
#include <NaWServer/handler_static.h>
#include <NaWServer/request.h>
#include <NaWServer/route.h>
#include <fstream>
#include <iterator>
#include <string>

namespace NaWServer::demo
{

	inline std::string loadAsset(const char *name)
	{
		std::ifstream file(std::string(NAWSERVER_HTTP_TEST_ASSETS) + "/" + name, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	}

	inline SendResult healthResponse(Response &response)
	{
#if defined(_WIN32)
		constexpr char body[] = "{\"backend\":\"windows\",\"http\":\"ok\"}";
#else
		constexpr char body[] = "{\"backend\":\"linux\",\"http\":\"ok\"}";
#endif
		return response.send(200, "application/json", {reinterpret_cast<const uint8_t *>(body), sizeof(body) - 1});
	}

	inline SendResult echoResponse(Request &request, Response &response)
	{
		return response.send(200, "application/octet-stream", request.body());
	}

	inline SendResult parameterResponse(Request &request, Response &response)
	{
		const auto value = request.parameter("value").value_or("");
		return response.send(200, "text/plain", {reinterpret_cast<const uint8_t *>(value.data()), value.size()});
	}

	// One application composition for both the existing and the new wire backend.
	// Asset strings and convenience APIs allocate in this host demo, outside engine.
	class HttpRoutes
	{
		const std::string html_;
		const std::string css_;
		const std::string javascript_;
		const std::string configuration_;
		const std::string mcp_client_;
		const std::string mcp_panel_;

	public:
		HandlerStaticUriText index;
		HandlerStaticUriText stylesheet;
		HandlerStaticUriText script;
		HandlerStaticUriText profile;
		HandlerStaticUriText mcp_client;
		HandlerStaticUriText mcp_panel;
		FixedBasicAuthenticator credentials;
		AuthMiddleware authentication;
		AccessMiddleware access;
		HeaderChain<AuthMiddleware, AccessMiddleware> chain;
		Route health;
		Route echo;
		Route parameters;
		Route private_route;
		Route forbidden_route;

		explicit HttpRoutes(const char *configuration)
		  : html_(loadAsset("index.html")), css_(loadAsset("http-test.css")),
		    javascript_(loadAsset("http-test.js")), configuration_(loadAsset(configuration)),
		    mcp_client_(loadAsset("mcp-client.js")), mcp_panel_(loadAsset("mcp-panel.js")),
		    index("/", html_.data(), static_cast<std::ptrdiff_t>(html_.size())),
		    stylesheet("/http-test.css", css_.data(), static_cast<std::ptrdiff_t>(css_.size()),
		        "text/css; charset=utf-8"),
		    script("/http-test.js", javascript_.data(), static_cast<std::ptrdiff_t>(javascript_.size()),
		        "text/javascript; charset=utf-8"),
		    profile("/http-test-config.json", configuration_.data(), static_cast<std::ptrdiff_t>(configuration_.size()),
		        "application/json"),
		    mcp_client("/mcp-client.js", mcp_client_.data(), static_cast<std::ptrdiff_t>(mcp_client_.size()),
		        "text/javascript; charset=utf-8"),
		    mcp_panel("/mcp-panel.js", mcp_panel_.data(), static_cast<std::ptrdiff_t>(mcp_panel_.size()),
		        "text/javascript; charset=utf-8"),
		    credentials("test", "test-only", 1), authentication(credentials), chain(authentication, access),
		    health("/health", HttpMethod::Get, healthResponse), echo("/echo", HttpMethod::Post, echoResponse),
		    parameters("/parameters", Route::any_method, parameterResponse),
		    private_route("/private", HttpMethod::Get, healthResponse, {}, AccessPolicy::require(1), {}, chain),
		    forbidden_route("/forbidden", HttpMethod::Get, healthResponse, {}, AccessPolicy::require(2), {}, chain)
		{
		}

		bool ready() const
		{
			return !html_.empty() && !css_.empty() && !javascript_.empty() && !configuration_.empty() &&
			       !mcp_client_.empty() && !mcp_panel_.empty();
		}

	};

} // namespace NaWServer::demo
