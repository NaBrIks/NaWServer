// SPDX-License-Identifier: MIT
#include "demo.hpp"
#ifdef _WIN32
#include <NaWServer/backends/windows_tcp.h>
#else
#include <NaWServer/backends/linux_tcp.h>
#endif
#include <cstdio>
#include <cstdlib>

namespace
{

	using namespace NaWServer;
#ifdef _WIN32
	using NativeTransport = tcp::WindowsTransport;
#else
	using NativeTransport = tcp::LinuxTransport;
#endif

	void check(bool value, int line)
	{
		if(!value)
		{
			std::printf("COMPACT_START_FAILURE_FAIL line=%d\n", line);
			std::fflush(stdout);
			std::_Exit(1);
		}
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)

	// This lifecycle test never provisions keys or touches persistent storage.
	class KeyStorage final : public keys::Storage
	{
	public:
		keys::ReadStatus read(unsigned, void *, std::size_t, std::size_t &size) override
		{
			size = 0;
			return keys::ReadStatus::Missing;
		}

		bool write(unsigned, const void *, std::size_t) override { return false; }
	};

	class KeyRandom final : public keys::Random
	{
	public:
		bool fill(std::uint8_t *, std::size_t) override { return false; }
	};

	class TestPlatform final : public compact::Platform
	{
		KeyStorage storage_;
		KeyRandom random_;
		keys::Store keys_{storage_, random_};
	public:
		keys::Store &keys() override { return keys_; }
		compact::Settings settings() override { return {}; }
		bool configure(const compact::Settings &) override { return false; }
		compact::Status status() override { return {}; }
		std::size_t maximumFirmwareBytes() override { return 0; }
		const char *firmwareFormat() override { return "none"; }
		BodyReceiver beginFirmware(std::size_t, std::string_view) override { return {}; }
		void scheduleRestart() override {}
		void poll() override {}
	};

}

int main()
{
	NativeTransport transport;
	CHECK(transport.start().status == tcp::Status::Ok);
	auto occupied = transport.openListener("127.0.0.1", 0, 1);
	CHECK(occupied.status == tcp::Status::Ok && occupied.handle.valid && occupied.port != 443);
	auto options = compact::Demo::options();
	options.bind_address = "127.0.0.1";
	options.port = occupied.port;
	TcpHttpBackend backend(transport, options);
	TestPlatform platform;
	compact::Demo demo(backend, platform);
	CHECK(!demo.start());
	const auto failed = backend.diagnostics();
	CHECK(failed.state == TcpRuntimeState::Failed && failed.last_failure == TcpFailure::Listen);
	CHECK(transport.ownedHandles() == 1 && backend.bufferMemoryStats().used_bytes == 0);
	CHECK(transport.close(occupied.handle).released);
	// No intervening stop: start must have rolled back the SSE worker itself.
	// Static metadata from the failed HTTP runtime must also be reusable.
	CHECK(demo.start());
	CHECK(backend.diagnostics().state == TcpRuntimeState::Listening && backend.port() == options.port);
	CHECK(backend.transmissionStats().metadata_bytes > 0 && transport.ownedHandles() == 1);
	CHECK(demo.stop() && transport.ownedHandles() == 0);
	CHECK(backend.bufferMemoryStats().used_bytes == 0 && transport.stop().status == tcp::Status::Ok);
	std::puts("COMPACT_START_FAILURE_PASS rollback_sse=1 static_metadata_reused=1 same_object_retry=1");
}
