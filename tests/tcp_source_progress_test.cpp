// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/backends/tcp.h>
#include <NaWServer/http_server.h>
#include <NaWServer/route.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	using namespace NaWServer;
	using namespace NaWServer::tcp;
	using Clock = std::chrono::steady_clock;
	void check(bool value, int line)
	{
		if(!value) { std::printf("SOURCE_PROGRESS_FAIL %d\n", line); std::fflush(stdout); std::_Exit(1); }
	}
#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	class SlowNative final : public Transport
	{
		std::mutex mutex_;
		std::string output_;
		std::atomic<bool> accepted_{false};
		std::size_t read_ = 0;
		Clock::time_point next_{};
		bool body_ = false;
	public:
		std::atomic<bool> closed{false};
		ListenerResult openListener(const char *, std::uint16_t, int) override
		{ return {Status::Ok, 0, {1, true}, 12345, {}}; }
		AcceptResult accept(Handle) override
		{
			if(accepted_.exchange(true)) return {Status::WouldBlock, 0, {}, {}};
			return {Status::Ok, 0, {2, true}, {}};
		}
		IoResult readSome(Handle, std::uint8_t *bytes, std::size_t capacity) override
		{
			static constexpr char input[] = "GET /file HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
			if(read_ == sizeof(input) - 1) return {IoStatus::WouldBlock, 0, 0};
			const auto size = std::min(capacity, sizeof(input) - 1 - read_);
			std::memcpy(bytes, input + read_, size); read_ += size;
			return {IoStatus::Progress, size, 0};
		}
		IoResult writeSome(Handle, const std::uint8_t *bytes, std::size_t) override
		{
			if(body_ && Clock::now() < next_) return {IoStatus::WouldBlock, 0, 0};
			std::lock_guard<std::mutex> lock(mutex_); output_ += static_cast<char>(*bytes);
			body_ = body_ || output_.find("\r\n\r\n") != output_.npos;
			if(body_) next_ = Clock::now() + std::chrono::milliseconds(3);
			return {IoStatus::Progress, 1, 0};
		}
		WaitResult wait(const Interest *, Ready *ready, std::size_t count, std::uint32_t timeout) override
		{
			for(std::size_t index = 0; index < count; ++index) ready[index] = {};
			if(!accepted_.load() && count) { ready[0].readable = true; return {WaitStatus::Ready, 1, 0}; }
			if(timeout) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return {WaitStatus::Timeout, 0, 0};
		}
		OperationResult shutdownWrite(Handle) override { return {}; }
		CloseResult close(Handle &handle) override
		{
			if(handle.value == 2) closed = true;
			handle = {}; return {Status::Ok, 0, true};
		}
		std::string output() { std::lock_guard<std::mutex> lock(mutex_); return output_; }
	};
}
int main()
{
	SlowNative native;
	TcpOptions options; options.port = 0; options.http_memory.enabled = true;
	options.no_progress_timeout_ms = 30; options.wait_ceiling_ms = 1; options.close_timeout_ms = 1;
	options.transmission.enabled = true; options.transmission.block_bytes = 32;
	options.transmission.maximum_blocks = 2; options.transmission.maximum_blocks_per_connection = 1;
	options.transmission.maximum_bytes = 64; options.transmission.reserved_important_blocks = 0;
	options.transmission.step_bytes = 1; options.transmission.stall_timeout_ms = 500;
	TcpHttpBackend backend(native, options);
	std::atomic<unsigned> released{0};
	auto endpoint = [&](Response &response)
	{
		return response.sendOwnedSource(200, "application/octet-stream", {{&released, 256,
			[](void *, std::size_t offset, std::uint8_t *bytes, std::size_t capacity)
			{
				for(std::size_t index = 0; index < capacity; ++index)
					bytes[index] = static_cast<std::uint8_t>((offset + index) % 251);
				return BodyReadResult{BodyReadStatus::Data, capacity};
			}}, [](void *context) { ++*static_cast<std::atomic<unsigned> *>(context); }});
	};
	Route route("/file", HttpMethod::Get, EndpointRef(endpoint)); HandlerBase *handlers[]{&route};
	HttpLimits limits; limits.exchange_timeout_ms = 10000;
	HttpServer server(backend, handlers, limits); CHECK(server.begin() == HttpError::Ok);
	const auto deadline = Clock::now() + std::chrono::seconds(8);
	while(!native.closed.load())
	{
		CHECK(Clock::now() < deadline); std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(server.end() == HttpError::Ok && released == 1);
	const auto wire = native.output(); const auto split = wire.find("\r\n\r\n");
	CHECK(split != wire.npos && wire.find("Content-Length: 256") != wire.npos);
	CHECK(wire.size() - split - 4 == 256);
	for(std::size_t index = 0; index < 256; ++index)
		CHECK(static_cast<unsigned char>(wire[split + 4 + index]) == index % 251);
	std::puts("SOURCE_PROGRESS_PASS slow-native-progress prevents false producer timeout");
}
