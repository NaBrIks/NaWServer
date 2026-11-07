// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/websocket.h>
#include <NaWServer/response.h>
#include "backends/tcp/session_io.h"
#include "support/tcp_scripted_transport.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace
{

	bool count_allocations = false;
	bool fail_nothrow_once = false;
	std::size_t allocation_count = 0;
	std::size_t deallocation_count = 0;
	unsigned checks = 0;

	void releaseStorage(void *storage)
	{
		if(count_allocations && storage)
			++deallocation_count;

		std::free(storage);
	}

	void check(bool condition, int line)
	{
		++checks;
		if(!condition)
		{
			std::printf("WEBSOCKET_ADMISSION_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

} // namespace

void *operator new(std::size_t size)
{
	if(count_allocations)
		++allocation_count;

	if(void *storage = std::malloc(size ? size : 1))
		return storage;

	std::abort();
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
	if(fail_nothrow_once)
	{
		fail_nothrow_once = false;
		return nullptr;
	}

	return ::operator new(size);
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *storage) noexcept
{
	releaseStorage(storage);
}

void operator delete[](void *storage) noexcept
{
	releaseStorage(storage);
}

void operator delete(void *storage, std::size_t) noexcept
{
	releaseStorage(storage);
}

void operator delete[](void *storage, std::size_t) noexcept
{
	releaseStorage(storage);
}

void operator delete(void *storage, const std::nothrow_t &) noexcept
{
	releaseStorage(storage);
}

namespace
{

	using namespace NaWServer;
	using namespace NaWServer::tcp;
	using namespace NaWServer::tcp::detail;
	using namespace NaWServer::tcp::test;

#define CHECK(value) check((value), __LINE__)

	static_assert(!std::is_copy_constructible_v<WebSocketReservation>);
	static_assert(!std::is_copy_assignable_v<WebSocketReservation>);
	static_assert(std::is_nothrow_move_constructible_v<WebSocketReservation>);
	static_assert(std::is_nothrow_move_assignable_v<WebSocketReservation>);

	class Session final : public WebSocketSession
	{
	public:
		bool alive = true;
		unsigned invalidations = 0;
		unsigned sends = 0;

		WebSocketSendResult sendCopy(const std::uint8_t *, std::size_t, bool) override
		{
			if(!alive)
				return WebSocketSendResult::Closed;

			++sends;
			return WebSocketSendResult::Queued;
		}

		void invalidate() override
		{
			++invalidations;
			alive = false;
		}

	};

	class Exchange final : public HttpExchange, public WebSocketExchange
	{
	public:
		bool handshake_complete = false;
		WebSocketReservation *prepared = nullptr;
		std::shared_ptr<Session> session;
		WebSocketClient *attached = nullptr;
		unsigned creates = 0;
		unsigned attachments = 0;
		unsigned close_requests = 0;
		WebSocketReadResult message{WebSocketReadStatus::Ignored, {}, false};

		explicit Exchange(std::shared_ptr<Session> session)
		  : session(std::move(session))
		{
		}

		~Exchange() override
		{
			disconnect();
		}

		HttpRequestHead head() const override
		{
			return {HttpMethod::Get, "/ws", 0};
		}

		HeaderReadResult readHeader(const char *, char *, std::size_t) const override
		{
			return {HeaderReadStatus::NotFound, 0};
		}

		BodyReadResult readBody(std::uint8_t *, std::size_t) override
		{
			return {BodyReadStatus::End, 0};
		}

		ResponseTransport responseTransport() override
		{
			return {};
		}

		void *extension(const void *key) const override
		{
			return key == &websocket_exchange_key ?
			       static_cast<WebSocketExchange *>(const_cast<Exchange *>(this)) : nullptr;
		}

		bool opening() const override
		{
			return handshake_complete;
		}

		WebSocketReservation *preparedAdmission() override
		{
			return prepared;
		}

		std::shared_ptr<WebSocketSession> createSession(const WebSocketLimits &) override
		{
			++creates;
			return session;
		}

		WebSocketClient *client() const override
		{
			return attached;
		}

		void attach(WebSocketClient *client) override
		{
			CHECK(handshake_complete && !attached);
			attached = client;
			++attachments;
		}

		WebSocketReadResult readMessage(const WebSocketLimits &) override
		{
			return message;
		}

		void close() override
		{
			++close_requests;
		}

		void disconnect()
		{
			if(attached)
			{
				auto *client = std::exchange(attached, nullptr);
				WebSocketEndpoint::disconnect(client);
			}
		}

	};

	class WrappedEndpoint final : public WebSocketEndpoint
	{
	public:
		unsigned before = 0;
		unsigned after = 0;

		explicit WrappedEndpoint(WebSocketLimits limits = {})
		  : WebSocketEndpoint("/ws", limits)
		{
		}

		void handleRequest(Request *request, Response &response) override
		{
			++before;
			WebSocketEndpoint::handleRequest(request, response);
			++after;
		}

	};

	void dispatch(HandlerBase &handler, Exchange &exchange)
	{
		Request request(exchange);
		Response response(exchange.responseTransport());
		handler.handleRequest(&request, response);
	}

	struct Connection
	{
		ScriptedTransport transport;
		Handle listener;
		Handle connection;

		Connection()
		{
			Step step;
			step.operation = Operation::OpenListener;
			step.handle = {42, true};
			step.port = 31234;
			CHECK(transport.append(step));
			listener = transport.openListener("127.0.0.1", 0, 7).handle;
			step = {};
			step.operation = Operation::Accept;
			step.handle = {0, true};
			CHECK(transport.append(step));
			connection = transport.accept(listener).handle;
			CHECK(listener.valid && connection.valid && connection.value == 0);
		}

		void output(std::size_t size, std::size_t maximum_chunk)
		{
			if(size)
			{
				Step step;
				step.operation = Operation::Write;
				step.expected_handle = connection;
				step.size = size;
				step.maximum_chunk = maximum_chunk;
				CHECK(transport.append(step));
			}
		}

		void fatalWrite()
		{
			Step step;
			step.operation = Operation::Write;
			step.expected_handle = connection;
			step.io_status = IoStatus::Fatal;
			step.native_error = 123;
			CHECK(transport.append(step));
		}

		static std::uint64_t milliseconds(void *context)
		{
			return static_cast<ScriptedTransport *>(context)->milliseconds();
		}

		bool write(std::string_view bytes)
		{
			std::atomic<bool> stopped{false};
			SessionIo io(transport, connection, stopped, {&transport, milliseconds}, 5000, 1000, 20);
			return io.write(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
		}

		void close()
		{
			CHECK(transport.remaining() == 0 && transport.failures() == 0);
			Step step;
			step.operation = Operation::Close;
			step.expected_handle = connection;
			CHECK(transport.append(step));
			CHECK(transport.close(connection).released);
			step.expected_handle = listener;
			CHECK(transport.append(step));
			CHECK(transport.close(listener).released);
			CHECK(transport.owned() == 0 && transport.released() == 2);
		}

	};

	constexpr std::string_view handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
	    "Connection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";

	void before101AndCommit()
	{
		for(const auto chunk : {1u, 2u, 3u, 7u, 127u})
		{
			Connection connection;
			WrappedEndpoint endpoint;
			unsigned callbacks = 0;
			auto session = std::make_shared<Session>();
			Exchange exchange(session);
			endpoint.onConnect([&](WebSocketClient *client)
			{
				CHECK(exchange.handshake_complete && exchange.attached == client && endpoint.count() == 1);
				CHECK(connection.transport.capturedSize() == handshake.size());
				++callbacks;
			});
			allocation_count = 0;
			count_allocations = true;
			auto reservation = endpoint.prepare(session);
			count_allocations = false;
			CHECK(reservation.valid() && !reservation.committed() && reservation.owner() == &endpoint);
			CHECK(allocation_count == 2);
			CHECK(endpoint.count() == 0 && endpoint.send("before101").queued == 0 && session->sends == 0);
			CHECK(!reservation.commit(exchange) && reservation.valid() && !callbacks);
			exchange.prepared = &reservation;
			connection.output(handshake.size(), chunk);
			exchange.handshake_complete = connection.write(handshake);
			CHECK(exchange.handshake_complete && !callbacks && endpoint.count() == 0);
			allocation_count = 0;
			count_allocations = true;
			dispatch(endpoint, exchange);
			count_allocations = false;
			CHECK(allocation_count == 0 && reservation.committed() && !reservation.valid());
			CHECK(endpoint.before == 1 && endpoint.after == 1 && callbacks == 1);
			CHECK(exchange.creates == 0 && exchange.attachments == 1 && !exchange.close_requests);
			CHECK(!reservation.commit(exchange) && callbacks == 1);
			reservation.rollback();
			CHECK(reservation.committed() && endpoint.count() == 1 && session->invalidations == 0);
			CHECK(endpoint.send("after101").queued == 1 && session->sends == 1);
			exchange.disconnect();
			CHECK(endpoint.count() == 0 && session->invalidations == 1 && !session->alive);
			connection.close();
		}
	}

	void partial101Rollback()
	{
		for(std::size_t cutoff = 0; cutoff < handshake.size(); ++cutoff)
		{
			Connection connection;
			WebSocketEndpoint endpoint("/ws");
			unsigned callbacks = 0;
			endpoint.onConnect([&](WebSocketClient *) {
				++callbacks;
			});
			auto session = std::make_shared<Session>();
			Exchange exchange(session);
			{
				auto reservation = endpoint.prepare(session);
				exchange.prepared = &reservation;
				CHECK(reservation.valid());
				connection.output(cutoff, 3);
				connection.fatalWrite();
				exchange.handshake_complete = connection.write(handshake);
				CHECK(!exchange.handshake_complete && connection.transport.capturedSize() == cutoff);
				CHECK(!reservation.committed() && endpoint.count() == 0 && callbacks == 0);
				CHECK(exchange.creates == 0 && exchange.attachments == 0 && session->invalidations == 0);
				allocation_count = 0;
				deallocation_count = 0;
				count_allocations = true;
			}
			count_allocations = false;
			CHECK(allocation_count == 0 && deallocation_count == 2);
			CHECK(session->invalidations == 1 && !session->alive && callbacks == 0 && endpoint.count() == 0);
			connection.close();
		}
	}

	void limitsMovesAndFailure()
	{
		WebSocketLimits limits;
		limits.max_clients = 1;
		WebSocketEndpoint endpoint("/ws", limits);
		auto first = std::make_shared<Session>();
		auto pending = endpoint.prepare(first);
		CHECK(pending.valid() && endpoint.count() == 0);
		auto second = std::make_shared<Session>();
		CHECK(!endpoint.prepare(second).valid() && second->invalidations == 1);
		auto competing_session = std::make_shared<Session>();
		Exchange competing_open(competing_session);
		competing_open.handshake_complete = true;
		dispatch(endpoint, competing_open);
		CHECK(competing_open.creates == 1 && competing_open.close_requests == 1 && !competing_open.attached);
		CHECK(pending.valid() && competing_session->invalidations == 1 && endpoint.count() == 0);
		WebSocketReservation moved(std::move(pending));
		CHECK(moved.valid() && !pending.valid() && pending.owner() == nullptr);
		moved.rollback();
		moved.rollback();
		CHECK(first->invalidations == 1 && !moved.valid());

		auto third = std::make_shared<Session>();
		fail_nothrow_once = true;
		CHECK(!endpoint.prepare(third).valid() && !fail_nothrow_once && third->invalidations == 1);
		auto fourth = std::make_shared<Session>();
		auto recovered = endpoint.prepare(fourth);
		CHECK(recovered.valid());
		Exchange exchange(fourth);
		exchange.handshake_complete = true;
		exchange.prepared = &recovered;
		dispatch(endpoint, exchange);
		CHECK(recovered.committed() && endpoint.count() == 1);
		auto excess = std::make_shared<Session>();
		CHECK(!endpoint.prepare(excess).valid() && excess->invalidations == 1);
		exchange.disconnect();
		CHECK(fourth->invalidations == 1 && endpoint.count() == 0);
		CHECK(!endpoint.prepare({}).valid());
		std::weak_ptr<Session> lifetime;
		{
			auto solely_owned = std::make_shared<Session>();
			lifetime = solely_owned;
			auto borrowed_owner = endpoint.prepare(std::move(solely_owned));
			CHECK(borrowed_owner.valid() && !solely_owned && !lifetime.expired());
			borrowed_owner.rollback();
			CHECK(lifetime.expired());
		}

		limits.max_clients = 2;
		WebSocketEndpoint two("/two", limits);
		auto old = std::make_shared<Session>();
		auto replacement = std::make_shared<Session>();
		auto left = two.prepare(old);
		auto right = two.prepare(replacement);
		allocation_count = 0;
		count_allocations = true;
		left = std::move(right);
		count_allocations = false;
		CHECK(allocation_count == 0 && left.valid() && !right.valid());
		CHECK(old->invalidations == 1 && replacement->invalidations == 0);
		auto additional = std::make_shared<Session>();
		auto available = two.prepare(additional);
		CHECK(available.valid());
		auto denied = std::make_shared<Session>();
		CHECK(!two.prepare(denied).valid() && denied->invalidations == 1);
		left.rollback();
		available.rollback();
		CHECK(replacement->invalidations == 1 && additional->invalidations == 1 && two.count() == 0);
	}

	void handlersAndLegacyOpening()
	{
		struct Unsupported final : HandlerBase
		{
			const char *websocket_uri() const override
			{
				return "/custom";
			}

		} unsupported;
		CHECK(unsupported.websocketEndpoint() == nullptr);
		WebSocketEndpoint owner("/ws");
		WebSocketEndpoint other("/other");
		auto session = std::make_shared<Session>();
		auto prepared = owner.prepare(session);
		Exchange exchange(session);
		exchange.handshake_complete = true;
		exchange.prepared = &prepared;
		dispatch(other, exchange);
		CHECK(exchange.close_requests == 1 && !exchange.attached && !prepared.committed() && prepared.valid());
		prepared.rollback();
		CHECK(session->invalidations == 1 && owner.count() == 0 && other.count() == 0);

		struct DecliningEndpoint final : WebSocketEndpoint
		{
			unsigned calls = 0;
			DecliningEndpoint()
			  : WebSocketEndpoint("/decline")
			{
			}

			void handleRequest(Request *, Response &) override
			{
				++calls;
			}

		} declining;
		CHECK(declining.websocketEndpoint() == &declining);
		auto declined_session = std::make_shared<Session>();
		auto declined = declining.prepare(declined_session);
		Exchange declined_exchange(declined_session);
		declined_exchange.handshake_complete = true;
		declined_exchange.prepared = &declined;
		dispatch(declining, declined_exchange);
		CHECK(declining.calls == 1 && !declined.committed() && !declined_exchange.attached);
		declined.rollback();
		CHECK(declined_session->invalidations == 1 && declining.count() == 0);

		WrappedEndpoint legacy;
		unsigned connects = 0;
		unsigned messages = 0;
		legacy.onConnect([&](WebSocketClient *) {
			++connects;
		});
		legacy.onMessage([&](WebSocketClient *, const std::uint8_t *bytes, std::size_t size, bool binary)
		{
			CHECK(size == 3 && bytes[0] == 0 && bytes[1] == 255 && bytes[2] == 0 && binary);
			++messages;
		});
		auto legacy_session = std::make_shared<Session>();
		Exchange legacy_exchange(legacy_session);
		legacy_exchange.handshake_complete = true;
		dispatch(legacy, legacy_exchange);
		CHECK(legacy_exchange.creates == 1 && legacy_exchange.attachments == 1 && connects == 1);
		CHECK(legacy.before == 1 && legacy.after == 1 && legacy.count() == 1);
		const std::uint8_t payload[]{0, 255, 0};
		legacy_exchange.handshake_complete = false;
		legacy_exchange.message = {WebSocketReadStatus::Message, {payload, sizeof(payload)}, true};
		dispatch(legacy, legacy_exchange);
		CHECK(messages == 1 && connects == 1 && legacy_exchange.creates == 1);
		CHECK(legacy.before == 2 && legacy.after == 2);
		legacy_exchange.disconnect();
		CHECK(legacy.count() == 0 && legacy_session->invalidations == 1);
	}

} // namespace

int main()
{
	before101AndCommit();
	partial101Rollback();
	limitsMovesAndFailure();
	handlersAndLegacyOpening();
	std::printf("WEBSOCKET_ADMISSION checks=%u reservation_bytes=%zu endpoint_bytes=%zu client_bytes=%zu PASS\n",
	    checks, sizeof(WebSocketReservation), sizeof(WebSocketEndpoint), sizeof(WebSocketClient));
}
