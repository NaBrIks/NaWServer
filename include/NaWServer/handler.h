// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/http_backend.h>
#include <NaWServer/pipeline.h>
#include <string>
#include <utility>

namespace NaWServer
{
	class Request;
	class Response;
	class HandlerBase;
	class WebSocketEndpoint;
	enum class RouteMatch { Exact, Prefix };
	struct RouteDescriptor
	{
		const char *path;
		HttpMethod method = HttpMethod::Any;
		RouteMatch match = RouteMatch::Exact;
	};
	struct RouteDescriptors
	{
		const RouteDescriptor *data = nullptr;
		std::size_t count = 0;
	};
	class IHandlerContainer
	{
	public:
		virtual void add_handler(HandlerBase *handler) = 0;
		virtual ~IHandlerContainer() = default;
	};

	// Transport endpoint interface. HTTP policies and middleware belong to Route;
	// native WS retains a separate upgrade/frame dispatch path.
	class HandlerBase
	{
	public:
		HandlerBase() = default;
		explicit HandlerBase(IHandlerContainer &container)
		{
			container.add_handler(this);
		}

		virtual ~HandlerBase() = default;
		virtual const char *websocket_uri() const
		{
			return nullptr;
		}

		// Optional two-phase admission capability. Backends still dispatch the
		// original virtual handleRequest; this hook does not replace application code.
		virtual WebSocketEndpoint *websocketEndpoint()
		{
			return nullptr;
		}

		virtual HttpError validate() const
		{
			return HttpError::Ok;
		}

		virtual RouteDescriptors routes() const
		{
			return {};
		}

		virtual HeaderPipeline headerPipeline() const
		{
			return {};
		}

		virtual AccessPolicy accessPolicy() const
		{
			return {};
		}

		virtual BodyOptions bodyOptions() const
		{
			return {};
		}

		virtual void handleRequest(Request *, Response &)
		{
		}

	};

	class HandlerStaticUrl : public HandlerBase
	{
	protected:
		std::string url_;
		RouteDescriptor descriptor_;

	public:
		explicit HandlerStaticUrl(std::string url)
		  : url_(std::move(url)), descriptor_{url_.c_str()}
		{
		}

		HandlerStaticUrl(IHandlerContainer &container, std::string url)
		  : HandlerBase(container), url_(std::move(url)), descriptor_{url_.c_str()}
		{
		}

		HandlerStaticUrl(const HandlerStaticUrl &) = delete;
		HandlerStaticUrl &operator=(const HandlerStaticUrl &) = delete;

		RouteDescriptors routes() const override
		{
			return {&descriptor_, 1};
		}

	};
} // namespace NaWServer
