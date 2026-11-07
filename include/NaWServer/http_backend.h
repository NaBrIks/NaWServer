// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/response.h>
#include <NaWServer/access_policy.h>
#include <NaWServer/request_body_stream.h>
#include <NaWServer/request_resource.h>
#include <cstdint>
#include <optional>
#include <string_view>

namespace NaWServer
{
	class HandlerBase;
	struct HttpRouteTable
	{
		HandlerBase *const *data = nullptr;
		std::size_t count = 0;
	};
	enum class HttpMethod { Any, Get, Post, Put, Delete, Patch, Options, Head, Unknown };
	enum class HttpError { Ok, InvalidArgument, InvalidState, Unsupported, TransportError, CleanupRequired };
	enum class HeaderReadStatus { Ok, NotFound, BufferTooSmall, InvalidArgument, TransportError };
	struct HeaderReadResult { HeaderReadStatus status; std::size_t size; };
	struct HttpRequestHead
	{
		HttpMethod method;
		std::string_view target;
		std::size_t content_length;
	};
	struct HttpLimits
	{
		std::size_t max_body_size = 4096;
		std::size_t max_header_bytes = 8192;
		std::size_t max_headers = 32;
		std::size_t max_target_size = 1024;
		std::uint16_t max_connections = 7;
		std::uint32_t exchange_timeout_ms = 5000;
	};
	enum class ExchangeDisposition { Complete, CloseConnection, Drop };
	enum class HeaderDisposition { Continue, Reply, Drop };
	struct HttpRequestState
	{
		bool prepared = false;
		HeaderDisposition disposition = HeaderDisposition::Continue;
		HandlerBase *route = nullptr;
		std::size_t route_index = 0;
#if NAWSERVER_AUTH
		Principal principal{};
		const void *authenticated_provider = nullptr;
#endif
		BodyOptions body{};
		bool body_streamed = false;
#if NAWSERVER_CLIENT_QUOTAS
		RequestResourceLease resource_lease;
#endif
		bool reservedResources() const
		{
#if NAWSERVER_CLIENT_QUOTAS
			return resource_lease.reserved();
#else
			return false;
#endif
		}

		bool reclassifyResources(RequestResource resource)
		{
#if NAWSERVER_CLIENT_QUOTAS
			return !resource_lease || resource_lease.reclassify(resource);
#else
			(void)resource;
			return true;
#endif
		}

		void releaseResources()
		{
#if NAWSERVER_CLIENT_QUOTAS
			resource_lease.reset();
#endif
		}

	};
	// Views and exchange are borrowed for the synchronous callback only.
	class HttpExchange
	{
	public:
		virtual ~HttpExchange() = default;
		virtual HttpRequestHead head() const = 0;
		// size excludes the terminating NUL; capacity must include it.
		virtual HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const = 0;
		virtual BodyReadResult readBody(uint8_t *destination, std::size_t capacity) = 0;
		// nullopt selects the owned readBody fallback. A present value, including
		// empty, is the complete body borrowed until the synchronous callback ends.
		// Its size must equal content_length; nonempty bytes require non-null data.
		virtual std::optional<ByteView> preparedBody() const
		{
			return std::nullopt;
		}

		virtual ResponseTransport responseTransport() = 0;
		virtual HttpRequestState *requestState() const
		{
			return nullptr;
		}

		// Host-order IPv4 from the native peer, never a client-supplied header.
		virtual std::optional<std::uint32_t> peerAddress() const
		{
			return std::nullopt;
		}

		// Optional platform extension. Unknown keys return nullptr.
		virtual void *extension(const void *) const
		{
			return nullptr;
		}

	};
	class HttpRequestHandler
	{
	public:
		virtual ~HttpRequestHandler() = default;
		virtual ExchangeDisposition handle(HttpExchange &exchange) = 0;
		virtual HeaderDisposition prepareHead(HttpExchange &, HttpRequestState &state)
		{
			state.prepared = true;
			return state.disposition = HeaderDisposition::Continue;
		}

		// Borrowed immutable metadata for transport-specific route registration.
		virtual HttpRouteTable transportRoutes() const
		{
			return {};
		}

	};
	// Owner calls start/stop outside handle(). Callbacks are serialized.
	// Successful stop joins callbacks and releases the borrowed handler.
	// Failed start returning CleanupRequired retains resources and requires stop.
	class HttpBackend
	{
	public:
		virtual ~HttpBackend() = default;
		virtual HttpError start(HttpRequestHandler &handler, const HttpLimits &limits) = 0;
		virtual HttpError stop() = 0;
	};
}
