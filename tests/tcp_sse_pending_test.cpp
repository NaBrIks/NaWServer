// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "backends/tcp/sse_stream.h"
#include "backends/tcp/connection_registry.h"
#include "support/tcp_scripted_transport.h"
#include <NaWServer/detail/sse/queued_adapter.hpp>
#include <NaWServer/request.h>
#include <NaWServer/sse.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace NaWServer;
using namespace NaWServer::tcp::detail;

namespace
{

	unsigned checks = 0;

	void check(bool value, int line)
	{
		++checks;
		if(!value)
		{
			std::printf("TCP_SSE_PENDING_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::abort();
		}
	}

#define CHECK(value) check((value), __LINE__)

	class Lease final : public TcpSseLease
	{
	public:
		enum class Close { Released, WouldBlock, Fatal, ReleasedWithError };
		bool committed = false, callback_done = false, rolled_back = false, released = false;
		bool close_attempted = false, finish_requested = false, release_during_callback = false;
		Close close = Close::Released;
		Close last_close = Close::Released;
		std::uint64_t retry_epoch = 0, attempted_epoch = 0;
		unsigned writes = 0, finishes = 0, closes = 0, close_errors = 0, callbacks = 0, rollbacks = 0;
		std::array<char, 8192> capture{};
		std::size_t captured = 0;

		void activate() override
		{
			committed = true;
		}

		void rollback() override
		{
			if(committed || rolled_back) return;

			rolled_back = true;
			++rollbacks;
		}

		int write(const char *bytes, std::size_t size) override
		{
			if(!committed || released || rolled_back || finish_requested) return -1;

			if(!callback_done) return 0;

			const auto written = std::min(size, std::size_t{3});
			CHECK(written <= capture.size() - captured);
			std::memcpy(capture.data() + captured, bytes, written);
			captured += written;
			++writes;
			return static_cast<int>(written);
		}

		void finish() override
		{
			++finishes;
			finish_requested = true;
			if(!committed || !callback_done || released || rolled_back) return;

			if(close_attempted && (last_close != Close::WouldBlock || attempted_epoch == retry_epoch)) return;

			close_attempted = true;
			attempted_epoch = retry_epoch;
			last_close = close;
			++closes;
			if(close != Close::Released) ++close_errors;

			if(close == Close::Released || close == Close::ReleasedWithError) released = true;
		}

		bool isOpen() const override
		{
			return committed && !rolled_back && !released;
		}

		void callbackComplete() override
		{
			++callbacks;
			callback_done = true;
			if(release_during_callback && committed) released = true;
		}

	};

	class Provider final : public TcpSseLeaseProvider
	{
	public:
		std::shared_ptr<Lease> lease = std::make_shared<Lease>();
		bool refuse = false;
		unsigned retains = 0;
		Token expected{2, 7};

		const void *identity() const override
		{
			return this;
		}

		std::shared_ptr<TcpSseLease> retainSse(Token token) override
		{
			++retains;
			if(refuse || token.slot != expected.slot || token.generation != expected.generation) return {};

			return lease;
		}

	};

	class Exchange final : public HttpExchange
	{
		TcpSseExchange &sse_;
		static SendResult send(void *context, int, const char *, const Header *, std::size_t, ByteView)
		{
			++static_cast<Exchange *>(context)->ordinary_sends;
			return SendResult::Ok;
		}

	public:
		unsigned ordinary_sends = 0;

		explicit Exchange(TcpSseExchange &sse)
		  : sse_(sse)
		{
		}

		HttpRequestHead head() const override
		{
			return {HttpMethod::Get, "/events", 0};
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
			return {this, send};
		}

		void *extension(const void *key) const override
		{
			return key == &sse::exchange_key ? &sse_ : nullptr;
		}

	};

	class ManualAdapter final : public sse::QueuedAdapter
	{
		std::uint64_t time_ = 0;
		bool allow_restart_;
	public:
		explicit ManualAdapter(bool allow_restart = false)
		  : allow_restart_(allow_restart)
		{
		}

		bool start() override
		{
			return beginRuntime(allow_restart_);
		}

		bool stop(std::uint32_t) override
		{
			requestStop();
			poll(time_);
			return finishRuntime();
		}

		void step(std::uint64_t time)
		{
			time_ = time;
			poll(time_);
		}

	};

	struct Fixture
	{
		Provider provider;
		TcpSseExchange binding{provider, provider.expected};
		Exchange exchange{binding};
		Request request{exchange};
		Response response{exchange.responseTransport()};
		ManualAdapter adapter;
		sse::EventSource source{adapter};

		Fixture()
		{
			CHECK(source.start());
		}

	};

	void reservationRollback()
	{
		Fixture fixture;
		{
			sse::Backend::Reservation reservation;
			CHECK(fixture.adapter.reserve(fixture.request, sse::default_header(), reservation) == sse::Admit::Owned);
			fixture.adapter.step(0);
			CHECK(fixture.source.active() == 1 && fixture.provider.retains == 1);
			CHECK(fixture.provider.lease->writes == 0 && fixture.provider.lease->closes == 0);
		}
		CHECK(fixture.source.active() == 0 && fixture.source.stats().rolled_back == 1);
		CHECK(fixture.source.stats().begun == 1 && fixture.source.completions() == 1);
		CHECK(fixture.provider.lease->rollbacks == 1 && !fixture.binding.callbackComplete());
		CHECK(fixture.response.text(409, "ordinary after rollback") == SendResult::Ok);
		CHECK(fixture.exchange.ordinary_sends == 1);
		CHECK(fixture.source.stop(0));
	}

	void closeBeforeCallbackGate()
	{
		Fixture fixture;
		const auto opened = fixture.source.open(fixture.request, fixture.response);
		CHECK(opened.active && opened.result == SendResult::Ok);
		for(unsigned index = 0; index < 4; ++index)
			fixture.adapter.step(index);

		CHECK(fixture.provider.lease->writes == 0 && fixture.provider.lease->closes == 0);
		CHECK(fixture.source.close(opened.token));
		fixture.adapter.step(4);
		fixture.adapter.step(5);
		CHECK(fixture.provider.lease->finishes == 2 && fixture.provider.lease->closes == 0);
		CHECK(fixture.source.active() == 1 && fixture.source.completions() == 0);
		CHECK(fixture.binding.callbackComplete());
		fixture.adapter.step(6);
		CHECK(fixture.provider.lease->closes == 1 && fixture.source.active() == 0);
		CHECK(fixture.source.completions() == 1 && fixture.source.stats().queued_bytes == 0);
		fixture.adapter.step(7);
		CHECK(fixture.provider.lease->closes == 1 && fixture.provider.lease->callbacks == 1);
		CHECK(fixture.binding.callbackComplete() && fixture.provider.lease->callbacks == 1);
		CHECK(fixture.source.stop(0));
	}

	void retainedWouldBlock()
	{
		Fixture fixture;
		const auto opened = fixture.source.open(fixture.request, fixture.response);
		CHECK(opened.active && fixture.binding.callbackComplete());
		sse::Frame frame;
		CHECK(sse::encode({"owned queued event", std::nullopt, std::nullopt, std::nullopt}, frame) ==
		    sse::EncodeResult::Ok);
		CHECK(fixture.source.publish(opened.token, frame) == sse::Publish::Queued);
		fixture.provider.lease->close = Lease::Close::WouldBlock;
		CHECK(fixture.source.close(opened.token));
		fixture.adapter.step(0);
		CHECK(fixture.provider.lease->closes == 1 && fixture.provider.lease->close_errors == 1);
		CHECK(fixture.source.active() == 1 && fixture.source.completions() == 0);
		CHECK(fixture.source.stats().queued_bytes == frame.size);
		CHECK(!fixture.source.stop(0));
		fixture.provider.lease->close = Lease::Close::Released;
		fixture.adapter.step(1);
		CHECK(fixture.provider.lease->closes == 1 && fixture.source.active() == 1);
		++fixture.provider.lease->retry_epoch; // Models the explicit owner stop hook.
		CHECK(fixture.source.stop(0));
		CHECK(fixture.provider.lease->closes == 2 && fixture.source.completions() == 1);
		CHECK(fixture.source.stats().queued_bytes == 0 && fixture.source.active() == 0);
	}

	void fatalRetainedAndReleasedWithError()
	{
		Fixture fixture;
		const auto opened = fixture.source.open(fixture.request, fixture.response);
		CHECK(opened.active && fixture.binding.callbackComplete());
		fixture.provider.lease->close = Lease::Close::Fatal;
		CHECK(fixture.source.close(opened.token));
		fixture.adapter.step(0);
		CHECK(!fixture.source.stop(0));
		++fixture.provider.lease->retry_epoch;
		CHECK(!fixture.source.stop(0));
		CHECK(fixture.provider.lease->closes == 1 && fixture.source.completions() == 0);
		// Test-only acknowledgment of external release permits fixture cleanup. This
		// does not claim the production backend can recover an ambiguous native close.
		fixture.provider.lease->released = true;
		CHECK(fixture.source.stop(0) && fixture.source.completions() == 1);

		Fixture released;
		const auto second = released.source.open(released.request, released.response);
		CHECK(second.active && released.binding.callbackComplete());
		released.provider.lease->close = Lease::Close::ReleasedWithError;
		CHECK(released.source.close(second.token));
		released.adapter.step(0);
		CHECK(released.provider.lease->closes == 1 && released.provider.lease->close_errors == 1);
		CHECK(released.source.active() == 0 && released.source.completions() == 1);
		CHECK(released.source.stop(0) && released.provider.lease->closes == 1);
	}

	void partialPrefaceAndOwnedEvent()
	{
		Fixture fixture;
		const auto opened = fixture.source.open(fixture.request, fixture.response);
		CHECK(opened.active && fixture.binding.callbackComplete());
		sse::Frame frame;
		CHECK(sse::encode({"hello\nworld", "update", "id7", 100}, frame) == sse::EncodeResult::Ok);
		CHECK(fixture.source.publish(opened.token, frame) == sse::Publish::Queued);
		frame.bytes.fill('X');
		for(unsigned index = 0; index < 150; ++index)
			fixture.adapter.step(index);

		const std::string_view received(fixture.provider.lease->capture.data(), fixture.provider.lease->captured);
		CHECK(received.find("HTTP/1.1 200 OK\r\n") == 0);
		CHECK(received.find("Content-Type: text/event-stream\r\n") != received.npos);
		CHECK(received.find("data: hello\ndata: world\n") != received.npos);
		CHECK(received.find("event: update\n") != received.npos && received.find("id: id7\n") != received.npos);
		CHECK(fixture.source.stats().sent_frames == 1);
		CHECK(fixture.source.stop(0) && fixture.provider.lease->closes == 1);
	}

	void abortAndExchangeLifetime()
	{
		Fixture fixture;
		const auto opened = fixture.source.open(fixture.request, fixture.response);
		CHECK(opened.active);
		CHECK(fixture.response.text(409, "must not escape") == SendResult::AlreadySent);
		fixture.adapter.step(0);
		CHECK(fixture.provider.lease->writes == 0 && fixture.provider.lease->closes == 0);
		CHECK(fixture.source.active() == 1 && fixture.exchange.ordinary_sends == 0);
		CHECK(fixture.binding.callbackComplete());
		CHECK(fixture.source.stop(0) && fixture.provider.lease->closes == 1);

		Provider provider;
		provider.lease->release_during_callback = true;
		{
			TcpSseExchange exchange(provider, provider.expected);
			auto lease = exchange.retain();
			CHECK(lease && !exchange.retain() && provider.retains == 1);
			lease->activate();
			CHECK(exchange.callbackComplete());
			CHECK(!lease->isOpen() && exchange.callbackComplete());
			CHECK(provider.lease->callbacks == 1);
		}
		CHECK(provider.lease->rollbacks == 0);
	}

	void refusedRetainAndStaleToken()
	{
		Fixture fixture;
		fixture.provider.refuse = true;
		const auto opened = fixture.source.open(fixture.request, fixture.response);
		CHECK(!opened.active && opened.admission == sse::Admit::NoMemory);
		CHECK(fixture.source.active() == 0 && fixture.source.stats().begun == 0);
		CHECK(!fixture.binding.retain() && fixture.provider.retains == 1);
		CHECK(!fixture.binding.callbackComplete());
		CHECK(fixture.provider.lease->writes == 0 && fixture.provider.lease->closes == 0);
		CHECK(fixture.source.stop(0));
		Provider provider;
		TcpSseExchange stale(provider, {provider.expected.slot, provider.expected.generation - 1});
		CHECK(!stale.retain() && !stale.callbackComplete());
		CHECK(provider.retains == 1 && provider.lease->writes == 0 && provider.lease->closes == 0);
	}

	void restartPreservesGenerations()
	{
		Provider provider;
		ManualAdapter adapter(true);
		sse::EventSource source(adapter);
		sse::Token old_token;
		sse::Frame frame;
		CHECK(sse::encode({"after restart", std::nullopt, std::nullopt, std::nullopt}, frame) == sse::EncodeResult::Ok);
		for(unsigned cycle = 0; cycle < 10; ++cycle)
		{
			provider.lease = std::make_shared<Lease>();
			TcpSseExchange binding(provider, provider.expected);
			Exchange exchange(binding);
			Request request(exchange);
			Response response(exchange.responseTransport());
			CHECK(source.start() && !source.start());
			const auto opened = source.open(request, response);
			CHECK(opened.active && binding.callbackComplete());
			CHECK(opened.token.generation == cycle + 1);
			if(cycle)
			{
				CHECK(source.publish(old_token, frame) == sse::Publish::Closed);
				CHECK(opened.token.generation != old_token.generation);
			}

			CHECK(source.publish(opened.token, frame) == sse::Publish::Queued);
			CHECK(source.stop(0));
			CHECK(source.active() == 0 && source.stats().begun == cycle + 1 && source.completions() == cycle + 1);
			old_token = opened.token;
		}
	}

	struct RegistryFixture
	{
		tcp::test::ScriptedTransport transport;
		int identity = 0;
		ConnectionSlot slots[1];
		CloseState close_states[1];
		ConnectionRegistry registry{transport, &identity, slots, close_states, 1};
		tcp::Handle listener;
		Token token;

		RegistryFixture()
		{
			tcp::test::Step step;
			step.operation = tcp::test::Operation::OpenListener;
			step.handle = {41, true};
			step.port = 12345;
			CHECK(transport.append(step));
			listener = transport.openListener("127.0.0.1", 0, 3).handle;
			accept();
		}

		void accept()
		{
			tcp::test::Step step;
			step.operation = tcp::test::Operation::Accept;
			step.handle = {0, true};
			CHECK(transport.append(step));
			auto connection = transport.accept(listener).handle;
			token = registry.claim(connection, Owner::Http);
			CHECK(token.valid() && !connection.valid);
		}

		void appendClose(tcp::Status status, bool released, int native_error = 0)
		{
			tcp::test::Step step;
			step.operation = tcp::test::Operation::Close;
			step.status = status;
			step.released = released;
			step.native_error = native_error;
			step.expected_handle = {0, true};
			CHECK(transport.append(step));
		}

		void finishListener()
		{
			tcp::test::Step step;
			step.operation = tcp::test::Operation::Close;
			step.expected_handle = listener;
			CHECK(transport.append(step));
			CHECK(transport.close(listener).released);
			CHECK(transport.owned() == 0 && transport.failures() == 0 && transport.remaining() == 0);
		}

	};

	void concreteLeaseGateAndRegistration()
	{
		RegistryFixture fixture;
		{
			auto lease = fixture.registry.retainSse(fixture.token);
			CHECK(lease && !lease->isOpen());
			CHECK(!fixture.registry.retainSse(fixture.token));
			lease->activate();
			CHECK(lease->isOpen());
			CHECK(lease->write("hello", 5) == 0);
			lease->finish();
			CHECK(fixture.transport.calls(tcp::test::Operation::Write) == 0);
			CHECK(fixture.transport.calls(tcp::test::Operation::Close) == 0);
			CHECK(fixture.registry.get(fixture.token, Owner::Http).valid && !fixture.registry.beginStop());
			lease->callbackComplete();
			CHECK(!fixture.registry.get(fixture.token, Owner::Http).valid);
			CHECK(fixture.registry.get(fixture.token, Owner::Sse).valid);
			tcp::test::Step writing;
			writing.operation = tcp::test::Operation::Write;
			writing.expected_handle = {0, true};
			writing.bytes = reinterpret_cast<const std::uint8_t *>("hello");
			writing.size = 5;
			writing.maximum_chunk = 2;
			CHECK(fixture.transport.append(writing));
			CHECK(lease->write("hello", 5) == 2);
			CHECK(lease->write("llo", 3) == 2);
			CHECK(lease->write("o", 1) == 1);
			CHECK(fixture.transport.capturedSize() == 5 &&
			    std::memcmp(fixture.transport.capturedBytes(), "hello", 5) == 0);
			fixture.appendClose(tcp::Status::Ok, true);
			lease->finish();
			CHECK(!lease->isOpen() && fixture.registry.active() == 0);
			CHECK(!fixture.registry.beginStop()); // The released lease still borrows Runtime.
			lease->finish();
			CHECK(fixture.transport.calls(tcp::test::Operation::Close) == 1);
		}
		CHECK(fixture.registry.beginStop() && fixture.registry.cleanupAfterJoin());
		CHECK(fixture.registry.sseCloseErrors() == 0);
		fixture.finishListener();
	}

	void concretePendingWouldBlock()
	{
		RegistryFixture fixture;
		{
			TcpSseExchange binding(fixture.registry, fixture.token);
			Exchange exchange(binding);
			Request request(exchange);
			Response response(exchange.responseTransport());
			ManualAdapter adapter;
			sse::EventSource source(adapter);
			CHECK(source.start());
			const auto opened = source.open(request, response);
			CHECK(opened.active && binding.callbackComplete());
			CHECK(source.close(opened.token));
			fixture.appendClose(tcp::Status::WouldBlock, false, 123);
			adapter.step(0);
			CHECK(source.active() == 1 && source.completions() == 0 && fixture.registry.sseCloseErrors() == 1);
			CHECK(!source.stop(0) && !fixture.registry.beginStop());
			CHECK(fixture.transport.calls(tcp::test::Operation::Close) == 1);
			fixture.registry.retrySseClose();
			fixture.appendClose(tcp::Status::Fatal, true, 456);
			CHECK(source.stop(0));
			CHECK(source.active() == 0 && source.completions() == 1 && fixture.registry.sseCloseErrors() == 2);
			CHECK(fixture.transport.calls(tcp::test::Operation::ShutdownWrite) == 0);
			CHECK(fixture.transport.calls(tcp::test::Operation::Close) == 2);
			CHECK(!fixture.registry.beginStop());
		}
		CHECK(fixture.registry.beginStop() && fixture.registry.cleanupAfterJoin());
		fixture.finishListener();
	}

	void concreteStaleRollbackCannotClearNewRegistration()
	{
		RegistryFixture fixture;
		auto stale = fixture.registry.retainSse(fixture.token);
		CHECK(stale != nullptr);
		const auto old_token = fixture.token;
		// Defensive malformed-owner model: an uncommitted lease outlives an HTTP
		// close/reuse. It must never clear the new generation's admission flag.
		fixture.appendClose(tcp::Status::Ok, true);
		CHECK(fixture.registry.close(old_token, Owner::Http).released);
		fixture.accept();
		CHECK(fixture.token.generation != old_token.generation);
		auto current = fixture.registry.retainSse(fixture.token);
		CHECK(current != nullptr);
		stale->rollback();
		CHECK(!fixture.registry.retainSse(fixture.token));
		CHECK(!fixture.registry.get(old_token, Owner::Http).valid);
		current->rollback();
		fixture.appendClose(tcp::Status::Ok, true);
		CHECK(fixture.registry.close(fixture.token, Owner::Http).released);
		stale.reset();
		current.reset();
		CHECK(fixture.registry.beginStop() && fixture.registry.cleanupAfterJoin());
		fixture.finishListener();
	}

	void concreteStaleCallbackAndFinishCannotTouchNewSocket()
	{
		for(const bool callback_already_complete : {false, true})
		{
			RegistryFixture fixture;
			auto stale = fixture.registry.retainSse(fixture.token);
			CHECK(stale != nullptr);
			stale->activate();
			if(callback_already_complete) stale->callbackComplete();

			const auto old_token = fixture.token;
			fixture.appendClose(tcp::Status::Ok, true);
			CHECK(fixture.registry.close(old_token, callback_already_complete ? Owner::Sse : Owner::Http).released);
			fixture.accept();
			auto current = fixture.registry.retainSse(fixture.token);
			CHECK(current != nullptr && fixture.token.generation != old_token.generation);
			if(callback_already_complete)
				stale->finish();
			else
				stale->callbackComplete();

			CHECK(!stale->isOpen() && stale->write("stale", 5) == -1);
			stale->finish();
			CHECK(fixture.transport.calls(tcp::test::Operation::Close) == 1);
			CHECK(fixture.transport.calls(tcp::test::Operation::Write) == 0);
			CHECK(fixture.transport.calls(tcp::test::Operation::ShutdownWrite) == 0);
			stale.reset();
			CHECK(!fixture.registry.retainSse(fixture.token));
			CHECK(fixture.registry.get(fixture.token, Owner::Http).valid);
			current->rollback();
			current.reset();
			fixture.appendClose(tcp::Status::Ok, true);
			CHECK(fixture.registry.close(fixture.token, Owner::Http).released);
			CHECK(fixture.registry.beginStop() && fixture.registry.cleanupAfterJoin());
			fixture.finishListener();
		}
	}

	void fourIndependentPendingCloses()
	{
		tcp::test::ScriptedTransport transport;
		ConnectionSlot slots[4];
		CloseState close_states[4];
		int identity = 0;
		ConnectionRegistry registry(transport, &identity, slots, close_states, 4);
		tcp::test::Step listening;
		listening.operation = tcp::test::Operation::OpenListener;
		listening.handle = {41, true};
		listening.port = 12345;
		CHECK(transport.append(listening));
		auto listener = transport.openListener("127.0.0.1", 0, 3).handle;
		ManualAdapter adapter;
		sse::EventSource source(adapter);
		CHECK(source.start());
		sse::Token tokens[4];
		for(std::size_t index = 0; index < 4; ++index)
		{
			tcp::test::Step accepting;
			accepting.operation = tcp::test::Operation::Accept;
			accepting.handle = {index, true};
			CHECK(transport.append(accepting));
			auto handle = transport.accept(listener).handle;
			const auto token = registry.claim(handle, Owner::Http);
			CHECK(token.valid());
			TcpSseExchange binding(registry, token);
			Exchange exchange(binding);
			Request request(exchange);
			Response response(exchange.responseTransport());
			const auto opened = source.open(request, response);
			CHECK(opened.active && binding.callbackComplete());
			tokens[index] = opened.token;
		}
		// Every callback object has ended; only independent owned leases remain.
		for(std::size_t index = 0; index < 4; ++index)
		{
			CHECK(source.close(tokens[index]));
			tcp::test::Step close;
			close.operation = tcp::test::Operation::Close;
			close.expected_handle = {index, true};
			close.status = index == 0 || index == 2 ? tcp::Status::WouldBlock :
			    (index == 3 ? tcp::Status::Fatal : tcp::Status::Ok);
			close.released = index == 1 || index == 3;
			CHECK(transport.append(close));
		}
		adapter.step(0);
		CHECK(source.active() == 2 && source.completions() == 2 && registry.sseCloseErrors() == 3);
		CHECK(transport.calls(tcp::test::Operation::Close) == 4 && !registry.beginStop());
		adapter.step(1);
		CHECK(transport.calls(tcp::test::Operation::Close) == 4 && source.active() == 2);
		for(const auto native : {0u, 2u})
		{
			tcp::test::Step close;
			close.operation = tcp::test::Operation::Close;
			close.expected_handle = {native, true};
			CHECK(transport.append(close));
		}
		registry.retrySseClose();
		CHECK(source.stop(0));
		CHECK(source.active() == 0 && source.completions() == 4 && source.stats().begun == 4);
		CHECK(transport.calls(tcp::test::Operation::Close) == 6);
		CHECK(transport.calls(tcp::test::Operation::ShutdownWrite) == 0);
		CHECK(registry.beginStop() && registry.cleanupAfterJoin());
		tcp::test::Step close;
		close.operation = tcp::test::Operation::Close;
		close.expected_handle = listener;
		CHECK(transport.append(close) && transport.close(listener).released);
		CHECK(transport.owned() == 0 && transport.failures() == 0 && transport.remaining() == 0);
	}

	[[noreturn]] void concreteFatalRetainedProcessCase()
	{
		RegistryFixture fixture;
		TcpSseExchange binding(fixture.registry, fixture.token);
		Exchange exchange(binding);
		Request request(exchange);
		Response response(exchange.responseTransport());
		ManualAdapter adapter;
		sse::EventSource source(adapter);
		CHECK(source.start());
		const auto opened = source.open(request, response);
		CHECK(opened.active && binding.callbackComplete() && source.close(opened.token));
		fixture.appendClose(tcp::Status::Fatal, false, 789);
		adapter.step(0);
		CHECK(!source.stop(0) && !fixture.registry.beginStop());
		fixture.registry.retrySseClose();
		CHECK(!source.stop(0) && source.active() == 1 && source.completions() == 0);
		CHECK(fixture.transport.calls(tcp::test::Operation::Close) == 1);
		CHECK(fixture.registry.sseCloseErrors() == 1 && fixture.transport.failures() == 0);
		std::printf("TCP_SSE_FATAL_RETAINED checks=%u close_attempts=1 stop=false active=1 completed=0 PASS\n", checks);
		std::fflush(stdout);
		// This separate negative-test process deliberately ends with pinned owners.
		// No real sockets exist in ScriptedTransport. Destruction would violate the
		// documented failed-stop lifetime contract, so no recovery is fabricated.
		std::_Exit(0);
	}

} // namespace

int main(int argc, char **argv)
{
	if(argc == 2 && std::strcmp(argv[1], "--fatal-retained") == 0) concreteFatalRetainedProcessCase();

	reservationRollback();
	closeBeforeCallbackGate();
	retainedWouldBlock();
	fatalRetainedAndReleasedWithError();
	partialPrefaceAndOwnedEvent();
	abortAndExchangeLifetime();
	refusedRetainAndStaleToken();
	restartPreservesGenerations();
	concreteLeaseGateAndRegistration();
	concretePendingWouldBlock();
	concreteStaleRollbackCannotClearNewRegistration();
	concreteStaleCallbackAndFinishCannotTouchNewSocket();
	fourIndependentPendingCloses();
	std::printf("TCP_SSE_PENDING checks=%u queued_adapter_bytes=%zu exchange_bytes=%zu PASS\n",
	    checks, sizeof(sse::QueuedAdapter), sizeof(TcpSseExchange));
}
