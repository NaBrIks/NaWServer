// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/detail/sse/response_bridge.hpp>
#include <NaWServer/handler_middleware.h>
#include <NaWServer/request.h>
#include <cstdio>
#include <cstdlib>
#include <string>

// The transport is scripted; request, response and both pipeline stages are real.
using namespace NaWServer;
using namespace NaWServer::sse;
namespace
{
	unsigned checked = 0;
	struct Exchange final : HttpExchange
	{
		HttpRequestHead head() const override { return {}; }
		HeaderReadResult readHeader(const char *, char *, std::size_t) const override { return {}; }
		BodyReadResult readBody(uint8_t *, std::size_t) override { return {BodyReadStatus::End, 0}; }
		ResponseTransport responseTransport() override { return {}; }
	};
#define CHECK(e)                                                                                                       \
		do                                                                                                                 \
		{                                                                                                                  \
			++checked;                                                                                                     \
			if(!(e))                                                                                                       \
			{                                                                                                              \
				std::printf("SSE_BRIDGE_FAIL line=%d expr=%s\n", __LINE__, #e);                                            \
				std::abort();                                                                                              \
			}                                                                                                              \
		} while(false)
	struct Wire
	{
		unsigned calls = 0;
		static SendResult send(void *context, int, const char *, const Header *, std::size_t, ByteView)
		{
			++static_cast<Wire *>(context)->calls;
			return SendResult::Ok;
		}

		ResponseTransport transport()
		{
			return {this, send};
		}

	};
	struct Backend
	{
		struct Reservation
		{
			Backend *owner = nullptr;
			Token value;
			LastEventId last_id;
			~Reservation()
			{
				if(owner)
					owner->rollback(*this);
			}

			Token token() const
			{
				return value;
			}

			AbortHandle abort_handle() const
			{
				return owner->invalid_control ? AbortHandle{} : AbortHandle{owner, Backend::abort};
			}

		};
		Sessions sessions;
		Response *observe = nullptr;
		Admit rejection = Admit::Owned;
		bool invalid_control = false, stop_during_commit = false;
		unsigned begun = 0, rolled_back = 0, aborts = 0, activated = 0;
		Token last;
		static void abort(void *ptr)
		{
			auto &self = *static_cast<Backend *>(ptr);
			++self.aborts;
			self.sessions.close(self.last);
		}

		Admit reserve(Request &, const Frame &preface, Reservation &r)
		{
			CHECK(!observe || observe->state() == ResponseState::Preparing);
			if(rejection != Admit::Owned)
				return rejection;

			r.value = sessions.reserve(9, preface, 0);
			CHECK(r.value.slot != client_capacity);
			r.owner = this;
			last = r.value;
			++begun;
			for(unsigned i = 0; i < 100; ++i)
				CHECK(sessions.next(last.slot, i).kind == StepKind::None);
			return Admit::Owned;
		}

		bool activate(Reservation &r)
		{
			CHECK(observe && observe->state() == ResponseState::External);
			if(stop_during_commit)
				sessions.shutdown();

			++activated;
			auto ready = sessions.activate(r.value, 100);
			r.owner = nullptr;
			return ready;
		}

		void rollback(Reservation &r)
		{
			CHECK(sessions.rollback(r.value));
			++rolled_back;
			r.owner = nullptr;
		}

	};
	void header_checks()
	{
		Frame frame;
		CHECK(encode_headers(nullptr, 0, frame) == SendResult::Ok);
		CHECK(frame.size == default_header().size);
		// Header order is allowed to differ from native-only fixture.
		std::string wire(frame.bytes.data(), frame.size);
		CHECK(wire.find("Cache-Control: no-cache\r\n") != wire.npos);
		CHECK(wire.find("Content-Type: text/event-stream\r\n") != wire.npos);
		auto before = frame;
		for(const char *key :
		    {"Connection",
		     "Content-Length",
		     "Transfer-Encoding",
		     "Content-Type",
		     "Content-Encoding",
		     "Trailer",
		     "bad key",
		     "cOnNeCtIoN",
		     "cOnTeNt-EnCoDiNg"})
		{
			Header bad[]{{key, "value"}};
			CHECK(encode_headers(bad, 1, frame) == SendResult::InvalidArgument);
			CHECK(frame.bytes == before.bytes && frame.size == before.size);
		}
		Header injection[]{{"X-Test", "x\r\nInjected: 1"}};
		CHECK(encode_headers(injection, 1, frame) == SendResult::InvalidArgument);
		Header duplicate[]{{"X-Key", "1"}, {"x-key", "2"}};
		CHECK(encode_headers(duplicate, 2, frame) == SendResult::InvalidArgument);
		CHECK(encode_headers(nullptr, 1, frame) == SendResult::InvalidArgument);
		CHECK(encode_headers(nullptr, Response::max_headers + 1, frame) == SendResult::HeaderLimit);
		std::string huge(769, 'a');
		Header too_long[]{{"X-Huge", huge.c_str()}};
		CHECK(encode_headers(too_long, 1, frame) == SendResult::BufferTooSmall);
		std::string exact(frame_capacity - default_header().size - 5, 'a');
		Header fits[]{{"X", exact.c_str()}}; // X + colon/space + CRLF = 5 bytes
		CHECK(encode_headers(fits, 1, frame) == SendResult::Ok && frame.size == frame_capacity);
		exact += 'a';
		fits[0].value = exact.c_str();
		CHECK(encode_headers(fits, 1, frame) == SendResult::BufferTooSmall);
		frame = before;
		Header aliased_value[]{{"X", frame.bytes.data()}};
		CHECK(encode_headers(aliased_value, 1, frame) == SendResult::InvalidArgument);
		CHECK(frame.bytes == before.bytes && frame.size == before.size);
		Header aliased_name[]{{frame.bytes.data(), "value"}};
		CHECK(encode_headers(aliased_name, 1, frame) == SendResult::InvalidArgument);
		CHECK(frame.bytes == before.bytes && frame.size == before.size);
	}

	void bridge_checks()
	{
		Exchange exchange;
		Request request(exchange);
		{
			Wire wire;
			constexpr Header defaults[] = {{"X-Test", "default"}, {"Cache-Control", "private"}};
			Response response(
				wire.transport(),
				DefaultHeaders(defaults),
				{ContentType::Json, CachePolicy::NoStore, HeaderFlags::NoSniff});
			CHECK(response.header("x-test", "owned override") == SendResult::Ok);
			Backend backend;
			backend.observe = &response;
			auto opened = open_stream(request, response, backend);
			CHECK(opened.active && opened.result == SendResult::Ok && response.state() == ResponseState::External);
			CHECK(wire.calls == 0 && backend.rolled_back == 0 && backend.activated == 1);
			auto step = backend.sessions.next(opened.token.slot, 101);
			CHECK(step.kind == StepKind::Write);
			std::string header(step.bytes.data(), step.size);
			CHECK(header.find("x-test: owned override\r\n") != header.npos);
			CHECK(header.find("Cache-Control: no-store\r\n") != header.npos);
			CHECK(header.find("X-Content-Type-Options: nosniff\r\n") != header.npos);
			CHECK(header.find("application/json") == header.npos); // SSE fixes MIME, not JSON framing
			CHECK(response.text(200, "duplicate") == SendResult::AlreadySent);
			CHECK(response.state() == ResponseState::External && backend.aborts == 1 && wire.calls == 0);
			CHECK(backend.sessions.written(step, 0, 101));
			auto end = backend.sessions.next(opened.token.slot, 102);
			CHECK(end.kind == StepKind::Close);
			CHECK(backend.sessions.completed(end));
		}
		for(auto reason : {Admit::Full, Admit::NoMemory, Admit::InvalidLastId, Admit::InvalidRequest, Admit::Stopping})
		{
			Wire wire;
			Response response(wire.transport());
			Backend backend;
			backend.observe = &response;
			backend.rejection = reason;
			auto opened = open_stream(request, response, backend);
			CHECK(!opened.active && opened.admission == reason && backend.begun == 0);
			CHECK(response.state() == ResponseState::Preparing && wire.calls == 0);
			CHECK(response.text(503, "rejected") == SendResult::Ok && wire.calls == 1);
		}
		{
			Wire wire;
			Response response(wire.transport());
			Backend backend;
			backend.observe = &response;
			backend.invalid_control = true;
			auto opened = open_stream(request, response, backend);
			CHECK(!opened.active && backend.begun == 1 && backend.rolled_back == 1 && backend.activated == 0);
			CHECK(backend.sessions.active() == 0 && wire.calls == 0);
		}
		{
			Wire wire;
			Response response(wire.transport());
			Backend backend;
			backend.observe = &response;
			backend.stop_during_commit = true;
			auto opened = open_stream(request, response, backend);
			CHECK(!opened.active && response.state() == ResponseState::External && backend.rolled_back == 0);
			CHECK(backend.aborts == 1 && wire.calls == 0);
			auto end = backend.sessions.next(opened.token.slot, 101);
			CHECK(end.kind == StepKind::Close);
			CHECK(backend.sessions.completed(end));
		}
		{
			Wire wire;
			Response response(wire.transport());
			Backend backend;
			backend.observe = &response;
			CHECK(response.header("Content-Encoding", "gzip") == SendResult::Ok);
			CHECK(!open_stream(request, response, backend).active && backend.begun == 0 && wire.calls == 0);
		}
		{
			Wire wire;
			Response response(wire.transport());
			Backend backend;
			backend.observe = &response;
			CHECK(response.empty() == SendResult::Ok);
			CHECK(open_stream(request, response, backend).result == SendResult::AlreadySent);
			CHECK(backend.begun == 0 && wire.calls == 1);
		}
	}

	struct Provider
	{
		bool accepted = false;
		bool authenticate(HeaderRequest &, Principal &principal)
		{
			if(accepted)
				principal = {true, 1, 1};

			return accepted;
		}

		const char *challenge() const
		{
			return "Fixture";
		}

	};
	void pipeline_checks()
	{
		Exchange exchange;
		Request request(exchange);
		Provider provider;
		AuthMiddleware auth(provider);
		AccessMiddleware access;
		auto chain = makeHeaderChain(auth, access);
		auto policy = AccessPolicy::require(1);
		for(unsigned shape = 0; shape < 3; ++shape)
		{
			Wire wire;
			Response response(wire.transport());
			Backend backend;
			backend.observe = &response;
			Context context{request, response, policy};
			HeaderContext header_context{request, response, policy};
			auto pair = [&](Request &r, Response &out)
				{
					return open_stream(r, out, backend).result;
				};
			auto only_response = [&](Response &out)
				{
					return pair(request, out);
				}; // borrow only during this call
			auto contextual = [&](Context &ctx)
				{
					return responseResult(pair(ctx.request, ctx.response));
				};
			auto endpoint = shape == 0 ? EndpointRef(pair)
			                : shape == 1 ? EndpointRef(only_response)
			                             : EndpointRef(contextual);
			auto invoke_endpoint = [&](HeaderContext &) { return endpoint(context); };
			provider.accepted = false;
			CHECK(chain.run(header_context, HeaderEndpointRef(invoke_endpoint)) == Result::Unauthorized);
			CHECK(backend.begun == 0 && wire.calls == 0 && response.state() == ResponseState::Preparing);
			provider.accepted = true;
			CHECK(chain.run(header_context, HeaderEndpointRef(invoke_endpoint)) == Result::Complete);
			CHECK(response.state() == ResponseState::External && wire.calls == 0 && backend.begun == 1);
			response.fail(); // Route applies this on a post-next Error; actual Route checked on C3.
			CHECK(backend.aborts == 1 && response.state() == ResponseState::External);
			auto close = backend.sessions.next(backend.last.slot, 100);
			CHECK(close.kind == StepKind::Close);
			CHECK(backend.sessions.completed(close));
		}
	}

} // namespace
int main()
{
	header_checks();
	bridge_checks();
	pipeline_checks();
	std::printf("SSE_BRIDGE_CHECKS passed=%u\n", checked);
}
