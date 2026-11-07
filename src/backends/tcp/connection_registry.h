// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include "features.h"
#include "connection_slots.h"
#if NAWSERVER_TCP_SSE
#include "sse_stream.h"
#endif
#include <NaWServer/detail/synchronization.h>

namespace NaWServer::tcp::detail
{

	struct CloseState
	{
		CloseResult result;
		Owner previous_owner = Owner::None;
		bool attempted = false;
		bool sse_registered = false;
		bool abort = false;
	};

	class SseLease;
	class WebSocketConnection;

	// Only bounded table/state updates hold this lock. Native operations are made
	// by the one current I/O owner after releasing it; stop waits for those owners.
	class ConnectionRegistry final
#if NAWSERVER_TCP_SSE
		: public TcpSseLeaseProvider
#endif
	{
		friend class SseLease;
		friend class WebSocketConnection;
		Transport &transport_;
#if NAWSERVER_TCP_SSE
		const void *identity_;
#endif
		ConnectionSlot *storage_;
		CloseState *close_states_;
		std::size_t capacity_;
		ConnectionSlots slots_;
		mutable NaWServer::detail::Mutex mutex_;
#if NAWSERVER_TCP_SSE
		std::size_t sse_registrations_ = 0;
		std::size_t sse_close_errors_ = 0;
		std::uint64_t sse_cleanup_epoch_ = 0;
#endif
		bool admission_closed_ = false;

	public:
		ConnectionRegistry(Transport &transport, const void *identity,
		    ConnectionSlot *storage, CloseState *close_states, std::size_t capacity);
		ConnectionRegistry(const ConnectionRegistry &) = delete;
		ConnectionRegistry &operator=(const ConnectionRegistry &) = delete;

		Token claim(Handle &source, Owner owner);
		Handle get(Token token, Owner owner) const;
		Handle releaseHttp(Token token);
		bool transfer(Token token, Owner previous, Owner next);
		CloseResult close(Token token, Owner owner, bool abort = false);
		std::size_t active() const;
		bool beginStop();
		bool cleanupAfterJoin();
#if NAWSERVER_TCP_SSE
		const void *identity() const override;
		std::shared_ptr<TcpSseLease> retainSse(Token token) override;
		void retrySseClose();
		std::size_t sseCloseErrors() const;
#endif
	};

} // namespace NaWServer::tcp::detail
