// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/detail/configuration_names.h>
#include <NaWServer/handler.h>
#include <NaWServer/http_backend.h>
#include <functional>

namespace NaWServer
{
	class Request;
	ExchangeDisposition dispatchHttp(HttpExchange &exchange, HandlerBase *const *handlers,
	    std::size_t count, DefaultHeaders defaults, ResponseOptions options,
	    const HttpLimits &limits, const std::function<void(Request &, Response &)> &not_found,
	    HeaderPipeline header_pipeline = {});
	HeaderDisposition prepareHttpHead(HttpExchange &exchange, HttpRequestState &state,
	    HandlerBase *const *handlers, std::size_t count, DefaultHeaders defaults,
	    ResponseOptions options, const HttpLimits &limits, HeaderPipeline header_pipeline = {},
	    bool has_not_found = false);
	class HttpServer final : public HttpRequestHandler
	{
		HttpBackend &backend_;
		HandlerBase *const *handlers_;
		std::size_t count_;
		HttpLimits limits_;
		DefaultHeaders defaults_;
		ResponseOptions options_;
		HeaderPipeline header_pipeline_;
		bool running_ = false;
		bool cleanup_required_ = false;
		std::function<void(Request &, Response &)> not_found_;
	public:
		template <std::size_t N>
		HttpServer(HttpBackend &backend, HandlerBase *const (&handlers)[N], HttpLimits limits = {},
		    DefaultHeaders defaults = {}, ResponseOptions options = {}, HeaderPipeline header_pipeline = {})
		  : backend_(backend), handlers_(handlers), count_(N), limits_(limits), defaults_(defaults), options_(options),
		    header_pipeline_(header_pipeline)
		{
		}

		~HttpServer()
		{
			(void)end();
		}

		HttpServer(const HttpServer &) = delete;
		HttpServer &operator=(const HttpServer &) = delete;
		HttpError begin();
		HttpError end();
		bool running() const
		{
			return running_;
		}

		bool cleanupRequired() const
		{
			return cleanup_required_;
		}

		void onNotFound(std::function<void(Request &, Response &)> callback);
		ExchangeDisposition handle(HttpExchange &exchange) override;
		HeaderDisposition prepareHead(HttpExchange &exchange, HttpRequestState &state) override;
		HttpError setHeaderPipeline(HeaderPipeline pipeline)
		{
			if(running_ || cleanup_required_) return HttpError::InvalidState;

			header_pipeline_ = pipeline;
			return HttpError::Ok;
		}

		HttpRouteTable transportRoutes() const override
		{
			return {handlers_, count_};
		}

	};
}
