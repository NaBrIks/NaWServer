// SPDX-License-Identifier: MIT
#include "demo.hpp"
#include <NaWServer/api_keys_file.h>
#ifdef _WIN32
#include <NaWServer/backends/windows_tcp.h>
#else
#include <NaWServer/backends/linux_tcp.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

class HostPlatform final : public compact::Platform
{
	NaWServer::keys::FileStorage storage_;
	NaWServer::keys::SystemRandom random_;
	NaWServer::keys::Store keys_{storage_, random_};
	compact::Settings settings_;
	std::mutex mutex_;
	std::mutex keys_mutex_;
	std::chrono::steady_clock::time_point began_ = std::chrono::steady_clock::now();
public:
	explicit HostPlatform(const std::string &base) : storage_(base + "/keys") {}
	bool initialize()
	{
		using namespace NaWServer::keys;
		if(keys_.load() != Status::Ok) return false;
		if(!keys_.metadata(0).active)
		{
			Secret secret;
			if(keys_.create("API", secret) != Status::Ok) return false;
			std::printf("NEW_KEY=%s\n", secret.value.data()); secret.clear();
		}
		return true;
	}
	NaWServer::keys::Store &keys() override { return keys_; }
	bool authorized(std::string_view value) override
	{
		std::lock_guard<std::mutex> lock(keys_mutex_);
		return keys_.authenticate(value).has_value();
	}
	std::uint32_t keyRevision() override
	{
		std::lock_guard<std::mutex> lock(keys_mutex_);
		return keys_.metadata(0).revision;
	}
	bool issueKey(NaWServer::keys::Secret &secret) override
	{
		std::lock_guard<std::mutex> lock(keys_mutex_);
		return compact::Platform::issueKey(secret);
	}
	compact::Settings settings() override { std::lock_guard<std::mutex> lock(mutex_); return settings_; }
	bool configure(const compact::Settings &value) override { std::lock_guard<std::mutex> lock(mutex_); settings_ = value; return true; }
	compact::Status status() override
	{
		compact::Status value; value.platform = "host"; value.firmware = "compact-host";
		value.uptime_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began_).count());
		return value;
	}
	std::size_t maximumFirmwareBytes() override { return 0; }
	const char *firmwareFormat() override { return "none"; }
	NaWServer::BodyReceiver beginFirmware(std::size_t, std::string_view) override { return {}; }
	void scheduleRestart() override {}
	void poll() override {}
};

int main(int argc, char **argv)
{
	const auto port = argc > 1 ? std::atoi(argv[1]) : 8187;
	const auto seconds = argc > 2 ? std::atoi(argv[2]) : 60;
	if(port <= 0 || port > 65535 || port == 443 || seconds <= 0) return 2;
	const std::string state = argc > 3 ? argv[3] : "local/compact-host";
	std::filesystem::create_directories(state);
	HostPlatform platform(state); if(!platform.initialize()) return 3;
#ifdef _WIN32
	NaWServer::tcp::WindowsTransport transport;
#else
	NaWServer::tcp::LinuxTransport transport;
#endif
	if(transport.start().status != NaWServer::tcp::Status::Ok) return 4;
	auto options = compact::Demo::options(); options.bind_address = "127.0.0.1"; options.port = static_cast<std::uint16_t>(port);
	NaWServer::TcpHttpBackend backend(transport, options);
	compact::Demo demo(backend, platform);
	if(!demo.start()) return 5;
	std::printf("READY http://127.0.0.1:%d/\n", port); std::fflush(stdout);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while(std::chrono::steady_clock::now() < deadline) { demo.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
	if(!demo.stop()) return 6;
	return transport.stop().status == NaWServer::tcp::Status::Ok ? 0 : 7;
}
