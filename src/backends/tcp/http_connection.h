// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include "session_io.h"
#include "http/wire/request_parser.h"

namespace NaWServer::tcp::detail
{

	struct HttpWorkspace
	{
		char *head;
		std::size_t head_capacity;
		http::wire::HeaderEntry *entries;
		std::size_t entry_capacity;
		// Distinct storage: must not overlap head/entries or their parser views.
		std::uint8_t *body = nullptr;
		std::size_t body_capacity = 0;
	};

	enum class ConnectionDisposition { Http, Close, Drop, WebSocket, External };
	enum class UpgradeDisposition { NotRequested, Http, Close, WebSocket };

	// Private protocol handoff for this synchronous connection only. No callback,
	// parser view or HTTP stack object is retained by either upgraded owner.
	class HttpConnectionBindings
	{
	public:
		virtual ~HttpConnectionBindings() = default;
		virtual void bindRequestState(HttpRequestState *) {}
		virtual void retainRequestResources(HttpRequestState &) {}
		virtual void *extension(const void *key) const = 0;
		virtual UpgradeDisposition tryUpgrade(HttpExchange &exchange,
		    const http::wire::RequestParser &parser, ByteView prefix, SessionIo &io) = 0;
		virtual bool callbackComplete() = 0;
	};

	// All parser views, Request/Response and the exchange end before this returns.
	// Optional body storage prepares one complete borrowed body; absent storage
	// retains dispatchHttp's owned-body fallback for standalone callers.
	ConnectionDisposition executeHttpConnection(SessionIo &io, HttpWorkspace workspace,
	    HttpRequestHandler &handler, const HttpLimits &limits, HttpConnectionBindings *bindings = nullptr);
	// Headers/body are already bounded and owned by the caller. Callbacks remain
	// synchronous; the optional response sink consumes/copies their borrowed data.
	ConnectionDisposition executePreparedHttpConnection(SessionIo &io, http::wire::RequestParser &parser,
	    ByteView body, HttpRequestHandler &handler, HttpConnectionBindings *bindings,
	    ResponseTransport response, HttpRequestState &state);

} // namespace NaWServer::tcp::detail
