// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp.h>
#include "connection_registry.h"
#include "fixed_buffer_arena.h"
#include "http_connection.h"
#if NAWSERVER_TCP_WEBSOCKET
#include "websocket_tx_pool.h"
#endif
#include <optional>

namespace NaWServer::tcp::detail
{

#if NAWSERVER_TCP_MANAGED
	// Opt-in bounded scheduler. Only this owner invokes application callbacks;
	// outgoing bytes are copied or explicitly immutable before a callback ends.
	class HttpConnectionManager
	{
		using Parser = http::wire::RequestParser;
		using Pool = NaWServer::detail::BufferPool;
		template <class T> using Array = NaWServer::detail::BufferArray<T>;
		template <class T> struct View
		{
			T *pointer = nullptr;
			std::size_t count = 0;
			T *get() const
			{
				return pointer;
			}

			std::size_t size() const
			{
				return count;
			}

		};
		enum class Phase { Free, Head, Body, Send, Drain, Closing };
		struct Input
		{
			Handle handle;
			Array<char> head;
			Array<http::wire::HeaderEntry> entries;
			Array<char> reply;
			HttpRequestState request;
			std::optional<Parser> parser;
			Phase phase = Phase::Free;
			std::size_t used = 0, offset = 0;
			std::uint64_t deadline = 0;
			int status = 0;
			CloseResult closed;
			bool close_attempted = false;
			bool abort = false;
			std::size_t reply_size = 0;
			std::uint16_t requests = 0;
		};
		struct Job
		{
			MemoryBlock storage, body, output_body;
			View<char> head, output_head;
			View<http::wire::HeaderEntry> entries;
			HttpConnectionManager *owner = nullptr;
			std::optional<Parser> parser;
			Token token;
			HttpRequestState request;
			BodyReceiver receiver;
			OwnedResponseSource source;
			std::size_t source_offset = 0, source_chunk_size = 0, source_chunk_offset = 0;
			std::size_t body_used = 0, body_received = 0, body_consumed = 0;
			std::size_t output_head_size = 0, offset = 0, discarded = 0;
			ByteView payload;
			std::uint64_t deadline = 0, last_progress = 0, close_deadline = 0;
			std::uint64_t sent_bytes = 0;
			Phase phase = Phase::Free;
			HttpMethod method = HttpMethod::Get;
			std::uint16_t requests = 0;
			bool persistent = false, reserved = false, direct_sent = false, immutable_payload = false;
			bool abort = false;
			std::size_t stream_client = static_cast<std::size_t>(-1);
		};
		struct Client
		{
			bool stream_active = false;
#if NAWSERVER_TCP_WEBSOCKET
			FixedBufferArena arena;
			Pool streams;
			std::optional<WebSocketTxPool> tx;
#endif
		};
		struct StreamLink
		{
			Token token;
			HttpRequestState request;
			Owner owner = Owner::None;
			std::size_t client = static_cast<std::size_t>(-1);
		};
		struct Idle { Handle handle; std::uint64_t deadline = 0; std::uint16_t requests = 0; CloseResult closed;
			          bool attempted = false; HttpRequestState request; };
		Transport &transport_;
		ConnectionRegistry &registry_;
		TcpOptions options_;
		HttpLimits limits_;
		HttpRequestHandler &handler_;
		Pool &pool_;
		MemoryManager headers_, bodies_, responses_, streams_;
		MemoryManager reserved_headers_, reserved_bodies_, reserved_responses_;
		Array<Input> inputs_;
		Array<Job> jobs_;
		Array<Client> clients_;
		Array<StreamLink> links_;
		Array<Idle> idle_;
		bool valid_ = false;
		Handle orphan_;
		CloseResult orphan_close_;
		void *context_;
		ConnectionDisposition (*dispatch_)(void *, Token, Parser &, ByteView, ResponseTransport, HttpRequestState &);
		bool readInput(Input &, std::uint64_t);
		bool work(Job &, std::uint64_t);
		void reject(Input &, int, std::uint64_t);
		bool closeInput(Input &);
		bool closeJob(Job &, std::uint64_t);
		void admit(Input &, std::size_t, std::uint64_t);
		static SendResult capture(void *, int, const char *, const Header *, std::size_t, ByteView);
		static SendResult captureInput(void *, int, const char *, const Header *, std::size_t, ByteView);
		static void completeDirect(void *);
		static SendResult captureImmutable(void *, int, const char *, const Header *, std::size_t, ByteView);
		static SendResult captureOwned(void *, int, const char *, const Header *, std::size_t, ByteView, MemoryBlock &);
		static SendResult captureSource(void *, int, const char *, const Header *, std::size_t, OwnedResponseSource &);
		static SendResult captureBody(Job &, int, const char *, const Header *, std::size_t, ByteView, bool);
		Job *find(Token);
		void releaseWork(Job &, std::uint64_t);
		void releaseStream(std::size_t);
		bool closeIdle(Idle &);
		bool pollIdle(Idle &, std::uint64_t);
		bool attach(Input &, Handle, std::uint64_t, std::uint16_t = 0);
	public:
		HttpConnectionManager(Transport &, ConnectionRegistry &, Pool &, TcpOptions, HttpLimits, HttpRequestHandler &, void *,
		    ConnectionDisposition(*)(void *, Token, Parser &, ByteView, ResponseTransport, HttpRequestState &));
		bool valid() const
		{
			return valid_;
		}

		bool accept(Handle, std::uint64_t);
		bool pump(std::uint64_t);
		bool cleanup();
		TcpMemoryStats stats() const;
#if NAWSERVER_TCP_WEBSOCKET
		WebSocketTxPool *websocketPool(Token);
#endif
	};
#else
	class HttpConnectionManager
	{
	public:
		using Dispatch = ConnectionDisposition (*)(void *, Token, http::wire::RequestParser &, ByteView,
		        ResponseTransport, HttpRequestState &);
		HttpConnectionManager(Transport &, ConnectionRegistry &, NaWServer::detail::BufferPool &, TcpOptions,
		    HttpLimits, HttpRequestHandler &, void *, Dispatch)
		{
		}

		bool valid() const
		{
			return false;
		}

		bool accept(Handle, std::uint64_t)
		{
			return false;
		}

		bool pump(std::uint64_t)
		{
			return false;
		}

		bool cleanup()
		{
			return true;
		}

		TcpMemoryStats stats() const
		{
			return {};
		}

#if NAWSERVER_TCP_WEBSOCKET
		WebSocketTxPool *websocketPool(Token)
		{
			return nullptr;
		}

#endif
	};
#endif

} // namespace NaWServer::tcp::detail
