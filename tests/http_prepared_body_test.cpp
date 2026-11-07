// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
	bool counting = false;
	unsigned allocations = 0;
	unsigned checks = 0;
	void check(bool condition, int line)
	{
		++checks;
		if(!condition)
		{
			std::fprintf(stderr, "HTTP_PREPARED_BODY_FAIL line=%d\n", line);
			std::abort();
		}
	}

}

void *operator new(std::size_t size)
{
	if(counting) ++allocations;

	if(void *storage = std::malloc(size ? size : 1)) return storage;

	std::abort();
}

void *operator new[](std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void *storage) noexcept
{
	std::free(storage);
}

void operator delete[](void *storage) noexcept
{
	std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept
{
	std::free(storage);
}

void operator delete[](void *storage, std::size_t) noexcept
{
	std::free(storage);
}

namespace
{
	using namespace NaWServer;
#define CHECK(value) check((value), __LINE__)

	class Exchange final : public HttpExchange
	{
		static SendResult send(void *context, int status, const char *, const Header *, std::size_t, ByteView body)
		{
			auto &exchange = *static_cast<Exchange *>(context);
			++exchange.responses;
			exchange.status = status;
			CHECK(body.size() <= exchange.output.size());
			if(body.size()) std::memcpy(exchange.output.data(), body.data(), body.size());

			exchange.output_size = body.size();
			return SendResult::Ok;
		}

	public:
		HttpRequestHead request_head{HttpMethod::Post, "/", 0};
		std::optional<ByteView> prepared;
		ByteView fallback;
		std::size_t offset = 0;
		unsigned reads = 0;
		mutable unsigned capability_calls = 0;
		unsigned responses = 0;
		int status = 0;
		std::array<std::uint8_t, 4096> output{};
		std::size_t output_size = 0;
		HttpRequestHead head() const override
		{
			return request_head;
		}

		HeaderReadResult readHeader(const char *, char *, std::size_t) const override
		{
			return {HeaderReadStatus::NotFound, 0};
		}

		BodyReadResult readBody(std::uint8_t *destination, std::size_t capacity) override
		{
			++reads;
			const auto count = std::min(capacity, fallback.size() - offset);
			if(!count) return {BodyReadStatus::End, 0};

			std::memcpy(destination, fallback.data() + offset, count);
			offset += count;
			return {BodyReadStatus::Data, count};
		}

		std::optional<ByteView> preparedBody() const override
		{
			++capability_calls;
			return prepared;
		}

		ResponseTransport responseTransport() override
		{
			return {this, send};
		}

	};

	class Endpoint final : public HandlerBase
	{
	public:
		ByteView expected;
		bool borrowed = true;
		unsigned calls = 0;
		RouteDescriptors routes() const override
		{
			static const RouteDescriptor route{"/", HttpMethod::Any};
			return {&route, 1};
		}

		void handleRequest(Request *request, Response &response) override
		{
			++calls;
			const auto body = request->body();
			CHECK(body.size() == expected.size());
			if(borrowed) CHECK(body.data() == expected.data());
			else CHECK(body.empty() || body.data() != expected.data());

			if(body.size()) CHECK(std::memcmp(body.data(), expected.data(), body.size()) == 0);

			CHECK(response.bytes(200, body) == SendResult::Ok);
		}

	};

	ExchangeDisposition dispatch(Exchange &exchange, Endpoint &endpoint)
	{
		HandlerBase *const handlers[]{&endpoint};
		const std::function<void(Request &, Response &)> missing;
		counting = true;
		allocations = 0;
		const auto result = dispatchHttp(exchange, handlers, 1, {}, {}, {}, missing);
		counting = false;
		return result;
	}

}

int main()
{
	std::array<std::uint8_t, 4096> bytes{};
	for(std::size_t index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::uint8_t>(index);

	Endpoint endpoint;
	for(const auto size : {std::size_t{0}, std::size_t{1}, bytes.size()})
	{
		Exchange exchange;
		exchange.request_head.content_length = size;
		exchange.prepared = ByteView{bytes.data(), size};
		endpoint.expected = *exchange.prepared;
		CHECK(dispatch(exchange, endpoint) == ExchangeDisposition::Complete);
		CHECK(allocations == 0 && exchange.reads == 0 && exchange.capability_calls == 1);
		CHECK(exchange.status == 200 && exchange.responses == 1 && exchange.output_size == size);
	}
	{
		Exchange exchange;
		exchange.prepared = ByteView{};
		endpoint.expected = {};
		CHECK(dispatch(exchange, endpoint) == ExchangeDisposition::Complete);
		CHECK(allocations == 0 && exchange.reads == 0 && exchange.status == 200);
	}
	for(const auto invalid : {ByteView{}, ByteView{nullptr, 4}, ByteView{bytes.data(), 3}, ByteView{bytes.data(), 5}})
	{
		Exchange exchange;
		exchange.request_head.content_length = 4;
		exchange.prepared = invalid;
		const auto calls = endpoint.calls;
		CHECK(dispatch(exchange, endpoint) == ExchangeDisposition::CloseConnection);
		CHECK(endpoint.calls == calls && exchange.status == 400 && exchange.responses == 1 && exchange.reads == 0);
		CHECK(allocations == 0);
	}
	{
		Exchange exchange;
		exchange.request_head.content_length = 4097;
		exchange.prepared = ByteView{nullptr, 4097};
		CHECK(dispatch(exchange, endpoint) == ExchangeDisposition::CloseConnection);
		CHECK(exchange.status == 413 && exchange.reads == 0 && exchange.capability_calls == 0 && allocations == 0);
	}
	{
		Exchange exchange;
		exchange.request_head.content_length = 4;
		exchange.fallback = {bytes.data(), 4};
		endpoint.expected = exchange.fallback;
		endpoint.borrowed = false;
		CHECK(dispatch(exchange, endpoint) == ExchangeDisposition::Complete);
		CHECK(exchange.reads == 1 && exchange.status == 200 && allocations == 1);
	}
	std::printf("HTTP_PREPARED_BODY_PASS checks=%u RequestBody=%zu Request=%zu\n",
	    checks, sizeof(RequestBody), sizeof(Request));
}
