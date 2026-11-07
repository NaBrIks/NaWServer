// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/tcp.h>
#include <NaWServer/handler.h>
#include "connection_registry.h"
#include "http_connection.h"
#include "http_connection_manager.h"
#if NAWSERVER_TCP_WEBSOCKET
#include "websocket_connection.h"
#include "websocket_tx_pool.h"
#include "websocket/wire/handshake.h"
#include "detail/ascii.h"
#endif
#include "platform/worker.h"
#include "queued_transport.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <new>

namespace NaWServer::tcp::detail
{

	namespace
	{

		bool managed(const TcpOptions &options)
		{
			return options.http_memory.enabled || options.keep_alive.enabled;
		}

		TransmissionOptions transmissionOptions(const TcpOptions &options)
		{
			auto result = options.transmission;
			if(!result.allocator.allocate && !result.allocator.release) result.allocator = options.buffer_memory.allocator;
			if(!result.metadata_allocator.allocate && !result.metadata_allocator.release)
				result.metadata_allocator = options.buffer_memory.allocator;

			if(!result.min_free_bytes) result.min_free_bytes = options.buffer_memory.min_free_bytes;

			return result;
		}

		std::size_t transmissionHandles(const TcpOptions &options, const HttpLimits &limits)
		{
			// A listener and a just-accepted handle can coexist with every live
			// session, ingress workspace and retained keep-alive connection.
			const std::size_t ingress = managed(options) ? options.managed.ingress_slots : 0;
			const std::size_t idle = options.keep_alive.enabled ? options.keep_alive.maximum_idle : 0;
			const auto maximum = std::numeric_limits<std::size_t>::max();
			std::size_t count = limits.max_connections;
			if(count > maximum - 2) return 0;
			count += 2;
			if(ingress > maximum - count) return 0;
			count += ingress;
			if(idle > maximum - count) return 0;
			return count + idle;
		}

		std::uint64_t nowMilliseconds(void *)
		{
			return tcpMilliseconds();
		}

		std::uint64_t deadlineFrom(std::uint64_t now, std::uint32_t interval)
		{
			const auto maximum = std::numeric_limits<std::uint64_t>::max();
			return interval > maximum - now ? maximum : now + interval;
		}

#if NAWSERVER_TCP_WEBSOCKET
		bool upgradeRequested(const http::wire::RequestParser &parser)
		{
			auto field = parser.headerValue("Upgrade");
			if(!field)
				return false;

			auto value = *field;
			while(!value.empty())
			{
				const auto comma = value.find(',');
				auto token = value.substr(0, comma);
				while(!token.empty() && (token.front() == ' ' || token.front() == '\t'))
					token.remove_prefix(1);

				while(!token.empty() && (token.back() == ' ' || token.back() == '\t'))
					token.remove_suffix(1);

				if(NaWServer::detail::asciiEqualIgnoreCase(token, "websocket"))
					return true;

				if(comma == value.npos)
					break;

				value.remove_prefix(comma + 1);
			}
			return false;
		}

		void rejectUpgrade(HttpExchange &exchange, int status, const char *message)
		{
			Response response(exchange.responseTransport());
			if(status == 503) (void)response.header("Retry-After", "1");

			(void)response.text(status, message);
		}

		bool writeUpgrade(SessionIo &io, std::string_view key)
		{
			char accept[29];
			if(!websocket::wire::makeWebSocketAccept(key, accept, sizeof(accept)))
				return false;

			constexpr char prefix[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
			    "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
			constexpr char suffix[] = "\r\n\r\n";
			return io.write(reinterpret_cast<const std::uint8_t *>(prefix), sizeof(prefix) - 1) &&
			       io.write(reinterpret_cast<const std::uint8_t *>(accept), 28) &&
			       io.write(reinterpret_cast<const std::uint8_t *>(suffix), sizeof(suffix) - 1);
		}

#endif

	} // namespace

	struct HttpRuntime
	{
		TcpHttpBackend &backend;
		std::unique_ptr<QueuedTransport> transmission;
		Transport &transport;
		HttpRequestHandler &handler;
		HttpLimits limits;
		TcpOptions options;
		NaWServer::detail::BufferArray<char> head_storage;
		NaWServer::detail::BufferArray<std::uint8_t> body_storage;
		NaWServer::detail::BufferArray<http::wire::HeaderEntry> entries;
		NaWServer::detail::BufferArray<ConnectionSlot> slot_storage;
		NaWServer::detail::BufferArray<CloseState> close_states;
		ConnectionRegistry registry;
#if NAWSERVER_CLIENT_QUOTAS
		struct ResourceLink { Token token; RequestResourceLease lease; };
		NaWServer::detail::BufferArray<ResourceLink> resource_links;
#endif
		std::size_t websocket_capacity;
		// Must outlive every WS control/pool lease retained below.
		std::unique_ptr<HttpConnectionManager> qos;
#if NAWSERVER_TCP_WEBSOCKET
		// Declared before sessions: every queued/active block is returned before
		// the backend-owned pool is destroyed, including failed-stop retries.
		WebSocketTxPool websocket_tx_pool;
		NaWServer::detail::BufferArray<std::shared_ptr<WebSocketConnection> > websockets;
		NaWServer::detail::BufferArray<std::shared_ptr<WebSocketConnection> > websocket_snapshot;
		NaWServer::detail::Mutex websocket_mutex;
		Worker *websocket_worker = nullptr;
		std::atomic<bool> websocket_exited{true};
#endif
		NaWServer::detail::BufferArray<Interest> interests;
		NaWServer::detail::BufferArray<Ready> ready;
		std::atomic<bool> stopping{false};
		Worker *worker = nullptr;
		Handle listener;
		Handle orphan;
		Token active;
		CloseResult listener_close;
		CloseResult orphan_close;
		bool listener_close_attempted = false;
		bool orphan_close_attempted = false;
		bool recovering = false;
		std::uint64_t retry_at = 0;
		std::uint16_t listen_port = 0;

		HttpRuntime(TcpHttpBackend *identity, Transport &native_transport, HttpRequestHandler &handler,
		    const HttpLimits &limits, TcpOptions options, bool websocket_enabled,
		    NaWServer::detail::BufferPool &buffers)
		  : backend(*identity),
		    transmission(options.transmission.enabled ? new(std::nothrow) QueuedTransport(native_transport,
		        transmissionOptions(options), transmissionHandles(options, limits)) : nullptr),
		    transport(transmission ? static_cast<Transport &>(*transmission) : native_transport),
		    handler(handler), limits(limits), options(options),
		    head_storage(buffers, managed(options) ? 0 : limits.max_header_bytes),
		    body_storage(buffers, managed(options) ? 0 : limits.max_body_size),
		    entries(buffers, managed(options) ? 0 : limits.max_headers),
		    slot_storage(buffers, limits.max_connections),
		    close_states(buffers, limits.max_connections),
		    registry(transport, identity, slot_storage.get(), close_states.get(), limits.max_connections),
#if NAWSERVER_CLIENT_QUOTAS
		    resource_links(buffers, managed(options) ? 0 : limits.max_connections),
#endif
#if NAWSERVER_TCP_WEBSOCKET
		    websocket_capacity(websocket_enabled ? std::min<std::size_t>(options.max_websocket_clients,
		        limits.max_connections) : 0),
		    websocket_tx_pool(websocket_enabled && !managed(options) && !options.transmission.enabled ?
		        options.websocket_tx_blocks : 0,
		        options.websocket_tx_block_size, &buffers, options.transmission.enabled),
		    websockets(buffers, websocket_capacity),
		    websocket_snapshot(buffers, websocket_capacity),
#else
		    websocket_capacity(0),
#endif
		    interests(buffers, websocket_capacity + 1),
		    ready(buffers, websocket_capacity + 1)
		{
#if !NAWSERVER_TCP_WEBSOCKET
			(void)websocket_enabled;
#endif
			if(managed(options))
				qos.reset(new(std::nothrow) HttpConnectionManager(transport, registry, buffers, options, limits, handler, this, dispatchQos));
		}

		bool allocated() const
		{
#if NAWSERVER_CLIENT_QUOTAS
			if(!managed(options) && !resource_links) return false;
#endif
			if(options.transmission.enabled && (!transmission || !transmission->valid())) return false;

			if((!managed(options) && (!head_storage || !body_storage || !entries)) ||
			    (managed(options) && (!qos || !qos->valid())) ||
			    !slot_storage || !close_states || !interests || !ready)
				return false;

#if NAWSERVER_TCP_WEBSOCKET
			return !websocket_capacity || (websockets && websocket_snapshot && (qos || websocket_tx_pool.valid()));
#else
			return true;
#endif
		}

		void releaseCompletedResources()
		{
#if NAWSERVER_CLIENT_QUOTAS
			for(std::size_t index = 0; index < resource_links.size(); ++index)
			{
				auto &link = resource_links[index];
				if(link.token.valid() && !registry.get(link.token, Owner::Http).valid &&
				    !registry.get(link.token, Owner::WebSocket).valid && !registry.get(link.token, Owner::Sse).valid &&
				    !registry.get(link.token, Owner::Closing).valid) link = {};
			}
#endif
		}

		void closeActive(bool abort = false)
		{
			if(!active.valid())
				return;

			const auto result = registry.close(active, Owner::Http, abort);
			if(!result.released && result.status != Status::InvalidState)
				backend.recordFailure(TcpFailure::ConnectionCleanup, result.native_error);

			if(result.released || result.status == Status::InvalidState)
				active = {};
		}

#if NAWSERVER_TCP_WEBSOCKET
		std::shared_ptr<WebSocketConnection> webSocket(std::size_t index)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(websocket_mutex);
			return websockets[index];
		}

		void closeWebSocket(std::size_t index)
		{
			auto connection = webSocket(index);
			if(!connection)
				return;

			// WS has relinquished all I/O. Release arena-backed buffers before the
			// registry close makes the identity arena eligible for QoS reclamation.
			connection->disconnect();
			const auto closed = registry.close(connection->token(), Owner::WebSocket);
			if(!closed.released && closed.status != Status::InvalidState)
				backend.recordFailure(TcpFailure::ConnectionCleanup, closed.native_error);

			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(websocket_mutex);
			websockets[index].reset();
		}

		void dispatchWebSockets()
		{
			for(std::size_t index = 0; index < websocket_capacity; ++index)
			{
				auto connection = webSocket(index);
				if(!connection) continue;

				if(!stopping.load()) connection->dispatchDeferredCallback();

				if(connection->ioFinished()) closeWebSocket(index);
			}
		}

		UpgradeDisposition upgrade(Token token, HttpExchange &exchange,
		    const http::wire::RequestParser &parser, ByteView prefix, SessionIo &io)
		{
			if(!upgradeRequested(parser))
				return UpgradeDisposition::NotRequested;

			HandlerBase *original = exchange.requestState() ? exchange.requestState()->route : nullptr;
			const auto path = parser.head().target.substr(0, parser.head().target.find('?'));
			const auto routes = handler.transportRoutes();
			for(std::size_t index = 0; !exchange.requestState() && index < routes.count; ++index)
			{
				const char *uri = routes.data[index]->websocket_uri();
				if(uri && path == uri)
				{
					original = routes.data[index];
					break;
				}
			}
			if(!original || !original->websocketEndpoint())
				return UpgradeDisposition::NotRequested;

			if(websocket::wire::validateHandshake(parser) != 0)
			{
				rejectUpgrade(exchange, 400, "Invalid WebSocket handshake");
				return UpgradeDisposition::Http;
			}

			std::size_t free_slot = 0;
			while(free_slot < websocket_capacity && webSocket(free_slot))
				++free_slot;

			auto *endpoint = original->websocketEndpoint();
			if(!endpoint || free_slot == websocket_capacity)
			{
				rejectUpgrade(exchange, 503, "WebSocket capacity unavailable");
				return UpgradeDisposition::Http;
			}

			struct ResourceReservation
			{
				HttpRequestState *state;
				bool retained = false;
				~ResourceReservation()
				{ if(state && !retained) (void)state->reclassifyResources(RequestResource::Http); }
			} resource{exchange.requestState()};
			if(resource.state && !resource.state->reclassifyResources(RequestResource::WebSocket))
			{
				rejectUpgrade(exchange, 503, "WebSocket quota unavailable");
				return UpgradeDisposition::Http;
			}
			auto *pool = &websocket_tx_pool;
			if(qos)
			{
				if(!(pool = qos->websocketPool(token)))
				{
					rejectUpgrade(exchange, 503, "WebSocket quota unavailable");
					return UpgradeDisposition::Http;
				}
			}

			auto connection = WebSocketConnection::create(registry, *pool, token, *original, *endpoint,
			        parser, limits, options.websocket_send_timeout_ms, options.close_timeout_ms,
			        options.close_drain_bytes);
			if(!connection || !connection->prepareDeferredCallbacks())
			{
				rejectUpgrade(exchange, 503, "WebSocket resources unavailable");
				return UpgradeDisposition::Http;
			}

			auto reservation = endpoint->prepare(connection);
			if(!reservation.valid())
			{
				rejectUpgrade(exchange, 503, "WebSocket admission refused");
				return UpgradeDisposition::Http;
			}

			const auto key = parser.headerValue("Sec-WebSocket-Key");
			if(!key || !writeUpgrade(io, *key) || stopping.load())
				return UpgradeDisposition::Close;

			if(!registry.transfer(token, Owner::Http, Owner::WebSocket))
				return UpgradeDisposition::Close;
			resource.retained = true;

			active = {}; // The protocol owner now owns every further close.
			if(!connection->dispatchOpening(reservation))
			{
				(void)registry.close(token, Owner::WebSocket);
				connection->disconnect();
				return UpgradeDisposition::WebSocket;
			}

			// Prefix bytes still belong to the HTTP workspace. Consume them while
			// callbacks are serialized here, then publish the complete I/O handoff.
			connection->consumePrefix(prefix);
			connection->enableDeferredCallbacks();
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(websocket_mutex);
				websockets[free_slot] = std::move(connection);
			}
			return UpgradeDisposition::WebSocket;
		}

#endif

		class Bindings final : public HttpConnectionBindings
#if NAWSERVER_TCP_SSE
			                   , public TcpSseLeaseProvider
#endif
		{
			[[maybe_unused]] HttpRuntime &runtime_;
			Token token_;
			tcp::PeerAddress peer_;
			HttpRequestState *state_ = nullptr;
#if NAWSERVER_TCP_SSE
			TcpSseExchange sse_;
#endif
		public:
			Bindings(HttpRuntime &runtime, Token token)
			  : runtime_(runtime), token_(token),
			    peer_(runtime.transport.peerAddress(runtime.registry.get(token, Owner::Http)))
#if NAWSERVER_TCP_SSE
			    , sse_(*this, token)
#endif
			{
			}

			void bindRequestState(HttpRequestState *state) override { state_ = state; }
			void retainRequestResources(HttpRequestState &state) override
			{
#if NAWSERVER_CLIENT_QUOTAS
				if(!runtime_.qos && state.resource_lease && token_.slot < runtime_.resource_links.size())
					runtime_.resource_links[token_.slot] = {token_, std::move(state.resource_lease)};
#else
				(void)state;
#endif
				state_ = nullptr;
			}

			void *extension(const void *key) const override
			{
				if(key == &tcp::peer_address_key) return const_cast<tcp::PeerAddress *>(&peer_);

#if NAWSERVER_TCP_SSE
				if(key == &sse::exchange_key) return const_cast<TcpSseExchange *>(&sse_);

#endif
				return nullptr;
			}

#if NAWSERVER_TCP_SSE
			const void *identity() const override
			{
				return runtime_.registry.identity();
			}

			std::shared_ptr<TcpSseLease> retainSse(Token token) override
			{
				if(state_ && !state_->reclassifyResources(RequestResource::EventStream)) return {};
				auto lease = runtime_.registry.retainSse(token);
				if(!lease && state_) (void)state_->reclassifyResources(RequestResource::Http);
				return lease;
			}

#endif
			UpgradeDisposition tryUpgrade(HttpExchange &exchange, const http::wire::RequestParser &parser,
			    ByteView prefix, SessionIo &io) override
			{
#if NAWSERVER_TCP_WEBSOCKET
				return runtime_.upgrade(token_, exchange, parser, prefix, io);
#else
				(void)exchange; (void)parser; (void)prefix; (void)io;
				return UpgradeDisposition::NotRequested;
#endif
			}

			bool callbackComplete() override
			{
#if NAWSERVER_TCP_SSE
				return sse_.callbackComplete();
#else
				return false;
#endif
			}

		};

		static ConnectionDisposition dispatchQos(void *context, Token token, http::wire::RequestParser &parser,
		    ByteView body, ResponseTransport response, HttpRequestState &state)
		{
			auto &runtime = *static_cast<HttpRuntime *>(context);
			SessionIo io(runtime.transport, runtime.registry.get(token, Owner::Http), runtime.stopping,
			    {nullptr, nowMilliseconds}, deadlineFrom(tcpMilliseconds(), runtime.limits.exchange_timeout_ms),
			    runtime.options.no_progress_timeout_ms, runtime.options.wait_ceiling_ms,
			    runtime.options.close_timeout_ms, runtime.options.close_drain_bytes);
			Bindings bindings(runtime, token);
			return executePreparedHttpConnection(io, parser, body, runtime.handler, &bindings, response, state);
		}

		static void entry(void *context)
		{
			static_cast<HttpRuntime *>(context)->run();
		}

		enum class AcceptDisposition { Continue, Recover, Cleanup };

		AcceptDisposition acceptReady()
		{
			const auto accepted = transport.accept(listener);
			if(accepted.status != Status::Ok && accepted.handle.valid)
			{
				backend.recordFailure(TcpFailure::Accept, accepted.native_error);
				orphan = accepted.handle;
				orphan_close = {};
				orphan_close_attempted = false;
				if(accepted.cleanup)
				{
					orphan_close = *accepted.cleanup;
					orphan_close_attempted = true;
				}

				return AcceptDisposition::Cleanup;
			}

			if(accepted.status == Status::WouldBlock || accepted.status == Status::Interrupted)
				return AcceptDisposition::Continue;

			if(accepted.status != Status::Ok || !accepted.handle.valid)
			{
				backend.recordFailure(TcpFailure::Accept, accepted.native_error);
				return AcceptDisposition::Recover;
			}

			if(qos)
				return qos->accept(accepted.handle, tcpMilliseconds()) ?
				       AcceptDisposition::Continue : AcceptDisposition::Cleanup;

			auto handle = accepted.handle;
			active = registry.claim(handle, Owner::Http);
			if(!active.valid())
			{
				// No session slot means no response budget. Close only this new handle.
				orphan_close = transport.close(handle);
				orphan_close_attempted = true;
				orphan = handle;
				if(orphan.valid) backend.recordFailure(TcpFailure::ConnectionCleanup, orphan_close.native_error);

				return orphan.valid ? AcceptDisposition::Cleanup : AcceptDisposition::Continue;
			}

			const auto deadline = deadlineFrom(tcpMilliseconds(), limits.exchange_timeout_ms);
			SessionIo io(transport, registry.get(active, Owner::Http), stopping,
			    {nullptr, nowMilliseconds}, deadline, options.no_progress_timeout_ms, options.wait_ceiling_ms,
			    options.close_timeout_ms, options.close_drain_bytes);
			HttpConnectionBindings *bindings = nullptr;
#if NAWSERVER_TCP_WEBSOCKET || NAWSERVER_TCP_SSE
			Bindings protocol_bindings(*this, active);
			bindings = &protocol_bindings;
#endif
			HttpWorkspace workspace;
			workspace.head = head_storage.get();
			workspace.head_capacity = limits.max_header_bytes;
			workspace.entries = entries.get();
			workspace.entry_capacity = limits.max_headers;
			workspace.body = body_storage.get();
			workspace.body_capacity = limits.max_body_size;
			const auto disposition = executeHttpConnection(io, workspace, handler, limits, bindings);
			if(disposition == ConnectionDisposition::External || disposition == ConnectionDisposition::WebSocket)
			{
				active = {};
				return AcceptDisposition::Continue;
			}

			if(disposition == ConnectionDisposition::Http)
				io.finish();

			closeActive(disposition == ConnectionDisposition::Drop);
			return active.valid() ? AcceptDisposition::Cleanup : AcceptDisposition::Continue;
		}

		void publishState(TcpRuntimeState state)
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(backend.diagnostics_mutex_);
			// stop() wins over an in-flight successful open; it will join this owner.
			if(!stopping.load()) backend.diagnostics_.state = state;
		}

		bool closeListener()
		{
			if(!listener.valid) return true;

			// Only documented retained WouldBlock is safe to retry. In particular,
			// never close a recycled native value after an ambiguous Fatal result.
			if(listener_close_attempted && listener_close.status != Status::WouldBlock) return false;

			listener_close = transport.close(listener);
			listener_close_attempted = true;
			if(listener_close.status != Status::Ok)
				backend.recordFailure(TcpFailure::ListenerCleanup, listener_close.native_error);

			if(!listener.valid)
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(backend.diagnostics_mutex_);
				backend.diagnostics_.port = 0;
			}

			return !listener.valid;
		}

		bool beginRecovery()
		{
			if(!options.recovery.enabled)
			{
				publishState(TcpRuntimeState::Failed);
				return false;
			}

			recovering = true;
			retry_at = deadlineFrom(tcpMilliseconds(), options.recovery.retry_delay_ms);
			publishState(TcpRuntimeState::Recovering);
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(backend.diagnostics_mutex_);
				backend.diagnostics_.port = 0;
			}
			if(!closeListener() && listener_close.status != Status::WouldBlock)
			{
				publishState(TcpRuntimeState::CleanupRequired);
				return false;
			}

			return true;
		}

		bool retryListener(std::uint64_t now)
		{
			if(!recovering || now < retry_at) return true;

			retry_at = deadlineFrom(now, options.recovery.retry_delay_ms);
			if(!closeListener())
			{
				if(listener_close.status == Status::WouldBlock) return true;

				publishState(TcpRuntimeState::CleanupRequired);
				return false;
			}

			if(stopping.load()) return true;

			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(backend.diagnostics_mutex_);
				if(backend.diagnostics_.retries != UINT32_MAX) ++backend.diagnostics_.retries;
			}
			// Reuse even an initially ephemeral port. Existing clients retain their
			// ownership; no application callback is reentered to reopen the listener.
			const auto opened = transport.openListener(backend.address_, listen_port, options.backlog);
			listener = opened.handle;
			listener_close_attempted = opened.cleanup.has_value();
			listener_close = opened.cleanup.value_or(CloseResult{});
			if(opened.status != Status::Ok || !listener.valid)
			{
				backend.recordFailure(TcpFailure::Listen, opened.native_error);
				if(listener.valid && listener_close_attempted)
				{
					backend.recordFailure(TcpFailure::ListenerCleanup, listener_close.native_error);
					if(listener_close.status == Status::WouldBlock) return true;

					publishState(TcpRuntimeState::CleanupRequired);
					return false;
				}

				if(!closeListener() && listener_close.status != Status::WouldBlock)
				{
					publishState(TcpRuntimeState::CleanupRequired);
					return false;
				}

				return true;
			}

			recovering = false;
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(backend.diagnostics_mutex_);
			backend.diagnostics_.port = opened.port;
			if(backend.diagnostics_.recoveries != UINT32_MAX) ++backend.diagnostics_.recoveries;

			if(!stopping.load()) backend.diagnostics_.state = TcpRuntimeState::Listening;

			return true;
		}

#if NAWSERVER_TCP_WEBSOCKET
		static void websocketEntry(void *context)
		{
			static_cast<HttpRuntime *>(context)->runWebSockets();
		}

		void runWebSockets()
		{
			while(!stopping.load())
			{
				{
					NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(websocket_mutex);
					for(std::size_t index = 0; index < websocket_capacity; ++index)
						websocket_snapshot[index] = websockets[index];
				}
				std::size_t count = 0;
				for(std::size_t index = 0; index < websocket_capacity; ++index)
				{
					auto &connection = websocket_snapshot[index];
					if(!connection) continue;

					if(connection->ioFinished()) { connection.reset(); continue; }

					connection->progressDeferredInput();
					const bool writing = connection->wantsWrite(tcpMilliseconds());
					const auto handle = registry.get(connection->token(), Owner::WebSocket);
					if(connection->finished() || !handle.valid)
					{
						connection->finishIo();
						connection.reset();
						continue;
					}
					const bool reading = connection->wantsRead();
					if(!reading && !writing) { connection.reset(); continue; }

					interests[count++] = {handle, reading, writing};
				}
				if(!count)
				{
					sleepTcpWorker(std::min<std::uint32_t>(20, options.wait_ceiling_ms));
					continue;
				}
				const auto selected = transport.wait(interests.get(), ready.get(), count, options.wait_ceiling_ms);
				if(selected.status == WaitStatus::Timeout || selected.status == WaitStatus::Interrupted) continue;

				if(selected.status != WaitStatus::Ready)
					backend.recordFailure(TcpFailure::Wait, selected.native_error);

				std::size_t event = 0;
				for(std::size_t index = 0; index < websocket_capacity; ++index)
				{
					auto &connection = websocket_snapshot[index];
					if(!connection) continue;

					const auto state = ready[event++];
					if(selected.status != WaitStatus::Ready || state.error)
						connection->abort();
					else
					{
						if(state.readable) connection->readReady(tcpMilliseconds());

						if(state.writable && !connection->finished()) connection->writeReady(tcpMilliseconds());
					}
					if(connection->finished()) connection->finishIo();

					connection.reset();
				}
			}
			// After publication of ioFinished no native/decoder/storage access is
			// permitted. HTTP owns serialized disconnect callbacks and final close.
			for(std::size_t index = 0; index < websocket_capacity; ++index)
			{
				auto connection = webSocket(index);
				if(connection && !connection->ioFinished()) connection->finishIo();

				websocket_snapshot[index].reset();
			}
			websocket_exited.store(true, std::memory_order_release);
		}
#endif

		void run()
		{
			while(!stopping.load())
			{
				releaseCompletedResources();
#if NAWSERVER_TCP_WEBSOCKET
				dispatchWebSockets();
#endif
				const bool qos_progress = qos && qos->pump(tcpMilliseconds());
				if(qos && !qos->valid())
				{
					backend.recordFailure(TcpFailure::ConnectionCleanup, 0);
					publishState(TcpRuntimeState::CleanupRequired);
					break;
				}
				if(!retryListener(tcpMilliseconds())) break;

				if(recovering || !listener.valid)
				{
					sleepTcpWorker(std::min<std::uint32_t>(20, options.wait_ceiling_ms));
					continue;
				}
				const Interest interest{listener, true, false};
				Ready state;
				const auto selected = transport.wait(&interest, &state, 1,
				    qos_progress ? 0 : options.wait_ceiling_ms);
				if(selected.status == WaitStatus::Timeout || selected.status == WaitStatus::Interrupted) continue;

				if(selected.status != WaitStatus::Ready)
				{
					backend.recordFailure(TcpFailure::Wait, selected.native_error);
					if(!beginRecovery()) break;

					continue;
				}
				if(stopping.load()) break;

				if(state.error)
				{
					const int native_error = state.native_error ? state.native_error : transport.listenerError(listener);
					backend.recordFailure(TcpFailure::Listener, native_error);
					if(!beginRecovery()) break;
				}
				else if(state.readable)
				{
					const auto accepted = acceptReady();
					if(accepted == AcceptDisposition::Cleanup)
					{
						publishState(TcpRuntimeState::CleanupRequired);
						break;
					}
					if(accepted == AcceptDisposition::Recover && !beginRecovery()) break;
				}
			}
			stopping.store(true);
#if NAWSERVER_TCP_WEBSOCKET
			while(!websocket_exited.load(std::memory_order_acquire)) sleepTcpWorker(1);

			dispatchWebSockets();
#endif
			if(!listener_close_attempted) (void)closeListener();
		}

		bool cleanupAfterJoin()
		{
			// Retry only a documented retained WouldBlock close, once per explicit
			// owner stop call. An ambiguous Fatal outcome never triggers blind retry.
			const bool qos_released = !qos || qos->cleanup();
			const bool connections_released = registry.cleanupAfterJoin();
			releaseCompletedResources();

			if(listener.valid) (void)closeListener();

			if(orphan.valid && (!orphan_close_attempted || orphan_close.status == Status::WouldBlock))
			{
				orphan_close = transport.close(orphan);
				orphan_close_attempted = true;
			}

			return connections_released && qos_released && !listener.valid && !orphan.valid;
		}

	};

} // namespace NaWServer::tcp::detail

namespace NaWServer
{

	TcpHttpBackend::TcpHttpBackend(tcp::Transport &transport, TcpOptions options)
	  : transport_(transport), options_(options)
	{
		if(!options.bind_address)
			return;

		for(std::size_t index = 0; index < sizeof(address_); ++index)
		{
			address_[index] = options.bind_address[index];
			if(address_[index] == '\0')
			{
				address_valid_ = index != 0;
				break;
			}
		}
		options_.bind_address = nullptr;
	}

	TcpHttpBackend::~TcpHttpBackend()
	{
		// On failed stop the owner must retain dependencies and retry. Deleting a
		// live runtime would race its callback; destruction cannot force that cleanup.
		(void)stop();
	}

	HttpError TcpHttpBackend::start(HttpRequestHandler &handler, const HttpLimits &limits)
	{
#if !NAWSERVER_TCP_MANAGED
		if(tcp::detail::managed(options_)) return HttpError::Unsupported;

#endif
		if(runtime_)
			return HttpError::InvalidState;

		const auto maximum = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
		if(!address_valid_ || options_.backlog <= 0 || !options_.no_progress_timeout_ms ||
		    !options_.wait_ceiling_ms || !options_.stop_timeout_ms || !options_.close_timeout_ms ||
		    (options_.recovery.enabled && !options_.recovery.retry_delay_ms) ||
		    !options_.close_drain_bytes || !limits.max_body_size ||
		    !limits.max_header_bytes || !limits.max_headers || !limits.max_target_size ||
		    !limits.max_connections || !limits.exchange_timeout_ms || limits.max_body_size > maximum ||
		    limits.max_header_bytes > maximum ||
		    limits.max_headers > maximum / sizeof(http::wire::HeaderEntry))
			return HttpError::InvalidArgument;

		const auto routes = handler.transportRoutes();
		if(routes.count && !routes.data)
			return HttpError::InvalidArgument;

		bool websocket_enabled = false;
		for(std::size_t index = 0; index < routes.count; ++index)
		{
			if(!routes.data[index])
				return HttpError::InvalidArgument;

			if(routes.data[index]->websocket_uri())
			{
#if NAWSERVER_TCP_WEBSOCKET
				const auto *endpoint = routes.data[index]->websocketEndpoint();
				if(!endpoint)
					return HttpError::Unsupported;

				if(endpoint->limits().max_message_size > options_.websocket_tx_block_size ||
				    !endpoint->limits().max_message_size || !endpoint->limits().max_pending_messages ||
				    !endpoint->limits().max_pending_bytes ||
				    !endpoint->limits().max_clients)
					return HttpError::InvalidArgument;

				if(options_.transmission.enabled && (options_.transmission.block_bytes < 14 ||
				    endpoint->limits().max_message_size > options_.transmission.block_bytes - 14))
					return HttpError::InvalidArgument;

				websocket_enabled = true;
#else
				return HttpError::Unsupported;
#endif
			}
		}

		if(websocket_enabled && (!options_.max_websocket_clients || !options_.websocket_send_timeout_ms ||
		    !options_.websocket_tx_blocks || !options_.websocket_tx_block_size ||
		    options_.websocket_tx_block_size > maximum / options_.websocket_tx_blocks))
			return HttpError::InvalidArgument;

		if(!options_.buffer_memory.max_bytes ||
		    (options_.buffer_memory.allocator.allocate != nullptr) !=
		    (options_.buffer_memory.allocator.release != nullptr)) return HttpError::InvalidArgument;

		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(diagnostics_mutex_);
			diagnostics_ = {};
			diagnostics_.state = TcpRuntimeState::Starting;
		}
		if(!buffers_.initialize(options_.buffer_memory, BufferGroup::Tcp))
		{
			recordFailure(TcpFailure::Storage, 0);
			setState(TcpRuntimeState::Failed);
			return HttpError::TransportError;
		}

		runtime_ = new(std::nothrow) tcp::detail::HttpRuntime(this, transport_, handler, limits, options_,
		        websocket_enabled, buffers_);
		if(!runtime_ || !runtime_->allocated())
		{
			recordFailure(TcpFailure::Storage, 0);
			setState(TcpRuntimeState::Failed);
			delete runtime_;
			runtime_ = nullptr;
			buffers_.reset();
			return HttpError::TransportError;
		}

		if(runtime_->transmission && !runtime_->transmission->start())
		{
			delete runtime_; runtime_ = nullptr; buffers_.reset();
			setState(TcpRuntimeState::Failed);
			return HttpError::TransportError;
		}
		const auto opened = runtime_->transport.openListener(address_, options_.port, options_.backlog);
		runtime_->listener = opened.handle;
		if(opened.cleanup)
		{
			runtime_->listener_close = *opened.cleanup;
			runtime_->listener_close_attempted = true;
		}

		if(opened.status != tcp::Status::Ok || !opened.handle.valid)
		{
			recordFailure(TcpFailure::Listen, opened.native_error);
			if(opened.handle.valid)
			{
				setState(TcpRuntimeState::CleanupRequired);
				return HttpError::CleanupRequired;
			}

			setState(TcpRuntimeState::Failed);
			delete runtime_;
			runtime_ = nullptr;
			buffers_.reset();
			return opened.status ==
			       tcp::Status::InvalidArgument ? HttpError::InvalidArgument : HttpError::TransportError;
		}

		runtime_->listen_port = opened.port;
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(diagnostics_mutex_);
			diagnostics_.port = opened.port;
			diagnostics_.state = TcpRuntimeState::Listening;
		}
#if NAWSERVER_TCP_WEBSOCKET
		if(runtime_->websocket_capacity)
		{
			runtime_->websocket_exited.store(false);
			runtime_->websocket_worker = tcp::detail::startTcpWorker(tcp::detail::HttpRuntime::websocketEntry,
			    runtime_, tcp::detail::WorkerRole::WebSocket);
			if(!runtime_->websocket_worker) runtime_->websocket_exited.store(true);
		}
		if(!runtime_->websocket_capacity || runtime_->websocket_worker)
#endif
			runtime_->worker = tcp::detail::startTcpWorker(tcp::detail::HttpRuntime::entry, runtime_);
		if(!runtime_->worker)
		{
			recordFailure(TcpFailure::WorkerStart, 0);
			runtime_->stopping.store(true);
#if NAWSERVER_TCP_WEBSOCKET
			if(runtime_->websocket_worker)
			{
				if(!tcp::detail::joinTcpWorker(runtime_->websocket_worker, options_.stop_timeout_ms))
				{
					setState(TcpRuntimeState::CleanupRequired);
					return HttpError::CleanupRequired;
				}
				runtime_->websocket_worker = nullptr;
			}
#endif
			runtime_->listener_close = runtime_->transport.close(runtime_->listener);
			runtime_->listener_close_attempted = true;
			if(runtime_->listener.valid)
			{
				setState(TcpRuntimeState::CleanupRequired);
				return HttpError::CleanupRequired;
			}

			delete runtime_;
			runtime_ = nullptr;
			buffers_.reset();
			{
				NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(diagnostics_mutex_);
				diagnostics_.port = 0;
				diagnostics_.state = TcpRuntimeState::Failed;
			}
			return HttpError::TransportError;
		}

		return HttpError::Ok;
	}

	TcpMemoryStats TcpHttpBackend::managedMemoryStats() const
	{
		return runtime_ && runtime_->qos ? runtime_->qos->stats() : TcpMemoryStats{};
	}

	tcp::TransmissionStats TcpHttpBackend::transmissionStats() const
	{
		return runtime_ && runtime_->transmission ? runtime_->transmission->stats() : tcp::TransmissionStats{};
	}

	HttpError TcpHttpBackend::stop()
	{
		if(!runtime_)
		{
			setState(TcpRuntimeState::Stopped);
			return HttpError::Ok;
		}

		if(tcp::detail::isTcpWorkerContext(runtime_))
			return HttpError::InvalidState;

		if(!runtime_->registry.beginStop())
			return HttpError::InvalidState;

		runtime_->stopping.store(true);
		setState(TcpRuntimeState::Stopping);
		if(runtime_->worker)
		{
			if(!tcp::detail::joinTcpWorker(runtime_->worker, options_.stop_timeout_ms))
			{
				setState(TcpRuntimeState::CleanupRequired);
				return HttpError::CleanupRequired;
			}

			runtime_->worker = nullptr;
		}

#if NAWSERVER_TCP_WEBSOCKET
		if(runtime_->websocket_worker)
		{
			if(!tcp::detail::joinTcpWorker(runtime_->websocket_worker, options_.stop_timeout_ms))
			{
				setState(TcpRuntimeState::CleanupRequired);
				return HttpError::CleanupRequired;
			}
			runtime_->websocket_worker = nullptr;
		}
#endif

		if(!runtime_->cleanupAfterJoin())
		{
			setState(TcpRuntimeState::CleanupRequired);
			return HttpError::CleanupRequired;
		}
		if(runtime_->transmission && !runtime_->transmission->stop(options_.stop_timeout_ms))
		{
			setState(TcpRuntimeState::CleanupRequired);
			return HttpError::CleanupRequired;
		}

#if NAWSERVER_TCP_SSE
		sse_close_errors_ += runtime_->registry.sseCloseErrors();
#endif
		delete runtime_;
		runtime_ = nullptr;
		buffers_.reset();
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(diagnostics_mutex_);
			diagnostics_.port = 0;
			diagnostics_.state = TcpRuntimeState::Stopped;
		}
		return HttpError::Ok;
	}

	std::uint16_t TcpHttpBackend::port() const
	{
		return diagnostics().port;
	}

	TcpDiagnostics TcpHttpBackend::diagnostics() const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(diagnostics_mutex_);
		return diagnostics_;
	}

	void TcpHttpBackend::recordFailure(TcpFailure failure, int native_error)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(diagnostics_mutex_);
		diagnostics_.last_failure = failure;
		diagnostics_.native_error = native_error;
		if(diagnostics_.errors != UINT32_MAX) ++diagnostics_.errors;
	}

	void TcpHttpBackend::setState(TcpRuntimeState state)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(diagnostics_mutex_);
		diagnostics_.state = state;
	}

} // namespace NaWServer

namespace NaWServer::tcp::detail
{

	void retryTcpSseClose(TcpHttpBackend &backend) noexcept
	{
#if NAWSERVER_TCP_SSE
		if(backend.runtime_)
			backend.runtime_->registry.retrySseClose();

#else
		(void)backend;
#endif
	}

	std::size_t tcpSseCloseErrors(const TcpHttpBackend &backend) noexcept
	{
		return backend.sse_close_errors_
#if NAWSERVER_TCP_SSE
		       + (backend.runtime_ ? backend.runtime_->registry.sseCloseErrors() : 0)
#endif
		;
	}

} // namespace NaWServer::tcp::detail
