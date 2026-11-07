// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include "connection_slots.h"
#include <NaWServer/sse_backend.h>

namespace NaWServer
{

	class TcpHttpBackend;

	namespace tcp::detail
	{

		// A runtime-owned TCP lifetime. The concrete lease keeps its registration
		// until destruction, including after native close and before final SSE ACK.
		// activate only commits the handoff; no native I/O until callbackComplete.
		class TcpSseLease : public sse::Stream
		{
		public:
			virtual void callbackComplete() = 0;
		};

		class TcpSseLeaseProvider
		{
		public:
			virtual ~TcpSseLeaseProvider() = default;
			virtual const void *identity() const = 0;
			virtual std::shared_ptr<TcpSseLease> retainSse(Token token) = 0;
		};

		// Stack exchange glue only. No Request/Response/Next or parser views escape
		// retain(); the returned lease owns its independent token/lifetime instead.
		class TcpSseExchange final : public sse::Exchange
		{
			TcpSseLeaseProvider &provider_;
			Token token_;
			std::shared_ptr<TcpSseLease> lease_;
			bool retain_attempted_ = false;
			bool callback_complete_ = false;
			bool external_ = false;

		public:
			TcpSseExchange(TcpSseLeaseProvider &provider, Token token);
			~TcpSseExchange() override;
			TcpSseExchange(const TcpSseExchange &) = delete;
			TcpSseExchange &operator=(const TcpSseExchange &) = delete;
			TcpSseExchange(TcpSseExchange &&) = delete;
			TcpSseExchange &operator=(TcpSseExchange &&) = delete;
			const void *owner() const override;
			std::shared_ptr<sse::Stream> retain() override;
			// Capture ownership before opening the callback gate: the SSE owner may
			// close immediately afterwards. true forbids HTTP finish/close of this fd.
			bool callbackComplete();
		};

		// Concrete Runtime hooks, callable before begin() and after successful drain.
		// Only an explicit adapter stop advances the retained-WouldBlock retry epoch.
		void retryTcpSseClose(TcpHttpBackend &backend) noexcept;
		std::size_t tcpSseCloseErrors(const TcpHttpBackend &backend) noexcept;

	} // namespace tcp::detail

} // namespace NaWServer
