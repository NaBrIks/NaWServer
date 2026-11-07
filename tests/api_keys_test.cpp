// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/api_keys.h>
#include <NaWServer/api_keys_file.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
using namespace NaWServer::keys;
namespace
{
	void check(bool ok, int line) { if(!ok) { std::printf("API_KEYS_FAIL line=%d\n", line); std::exit(1); } }
#define CHECK(v) check(static_cast<bool>(v), __LINE__)
	class Memory final : public Storage
	{
	public:
		std::array<std::array<std::uint8_t, Store::RecordBytes>, 2> records{};
		std::array<std::size_t, 2> sizes{};
		bool fail_write = false, fail_read = false;
		ReadStatus read(unsigned record, void *bytes, std::size_t capacity, std::size_t &size) override
		{
			size = 0; if(fail_read || record > 1 || capacity < sizes[record]) return ReadStatus::Error;
			if(!sizes[record]) return ReadStatus::Missing;
			size = sizes[record]; std::memcpy(bytes, records[record].data(), size); return ReadStatus::Ok;
		}
		bool write(unsigned record, const void *bytes, std::size_t size) override
		{
			if(fail_write) return false;
			if(record > 1 || size > records[record].size()) return false;
			std::memcpy(records[record].data(), bytes, size); sizes[record] = size; return true;
		}
	};
	class Entropy final : public Random
	{
	public:
		bool fail = false, repeat = false; std::uint8_t sequence = 0;
		bool fill(std::uint8_t *bytes, std::size_t size) override
		{
			if(fail) return false;
			if(!repeat) ++sequence;
			for(std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(sequence + i);
			return true;
		}
	};
	void stateAndFailures()
	{
		Memory memory; Entropy random; Store store(memory, random); Secret a, b;
		CHECK(!store.authenticate(std::string(64, '0'))); CHECK(store.load() == Status::Ok);
		CHECK(store.create("first", a) == Status::Ok && a.slot == 0);
		const auto first = std::string(a.value.data()); CHECK(first.size() == 64 && store.authenticate(first) == 0);
		CHECK(store.metadata(0).active && std::string(store.metadata(0).label.data()) == "first");
		CHECK(!store.authenticate(Store::identity(0))); CHECK(!store.authenticate(first + "0"));
		Store reboot(memory, random); CHECK(reboot.load() == Status::Ok && reboot.authenticate(first) == 0);
		random.repeat = true; CHECK(store.rotate(0, b) == Status::RandomError && b.value[0] == 0); random.repeat = false;
		CHECK(store.rotate(0, b) == Status::Ok && !store.authenticate(first) && store.authenticate(b.value.data()) == 0);
		CHECK(Store::identity(0) == "naw-api-slot-0");
		CHECK(store.revoke(0) == Status::Ok && !store.authenticate(b.value.data()));
		CHECK(store.revoke(0) == Status::NotFound);
		CHECK(reboot.load() == Status::Ok && !reboot.authenticate(b.value.data()));
		CHECK(store.create("bad\nlabel", b) == Status::Invalid);
		CHECK(store.create(std::string(25, 'a'), b) == Status::Invalid);
		random.fail = true; CHECK(store.create("fail", b) == Status::RandomError && b.value[0] == 0); random.fail = false;
		for(std::size_t i = 0; i < NAWSERVER_API_KEYS_MAX; ++i) CHECK(store.create("bounded", b) == Status::Ok);
		CHECK(store.create("full", b) == Status::Full && b.value[0] == 0);
		memory.fail_write = true; CHECK(store.revoke(0) == Status::IoError && !store.ready());
		CHECK(!store.authenticate(first)); memory.fail_write = false; CHECK(store.load() == Status::Ok);
		// The complete write exists on disk, but its readback fails: never serve stale in-memory auth.
		memory.fail_read = true; CHECK(store.rotate(0, b) == Status::IoError && !store.ready() && !b.value[0]);
		memory.fail_read = false; CHECK(store.load() == Status::Ok && store.metadata(0).active);
	}
	void damagedRecords()
	{
		Memory memory; Entropy random; Store store(memory, random); Secret a, b;
		CHECK(store.load() == Status::Ok && store.create("one", a) == Status::Ok);
		CHECK(store.create("two", b) == Status::Ok);
		memory.records[1][21] ^= 0x40; // Newest generation damaged: previous valid snapshot retained.
		CHECK(store.load() == Status::Ok && store.authenticate(a.value.data()) && !store.authenticate(b.value.data()));
		memory.records[0][0] ^= 1;
		CHECK(store.load() == Status::Corrupt && !store.ready() && !store.authenticate(a.value.data()));
		CHECK(store.create("must-not-reset", b) == Status::Invalid);
	}
	void platformPersistence()
	{
		const auto base = std::filesystem::temp_directory_path() / ("naw-keys-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		const auto prefix = base.u8string(); FileStorage files(prefix); SystemRandom random; Secret secret;
		Store store(files, random); CHECK(store.load() == Status::Ok && store.create("host", secret) == Status::Ok);
		FileStorage other_files(prefix); Store reboot(other_files, random); CHECK(reboot.load() == Status::Ok && reboot.authenticate(secret.value.data()) == secret.slot);
		CHECK(reboot.rotate(secret.slot, secret) == Status::Ok);
		CHECK(store.load() == Status::Ok && store.authenticate(secret.value.data()));
#ifndef _WIN32
		const auto permissions = std::filesystem::status(prefix + ".0").permissions();
		CHECK((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
		for(const auto suffix : {".0", ".1", ".tmp"}) std::filesystem::remove(prefix + suffix);
	}
}
int main() { stateAndFailures(); damagedRecords(); platformPersistence(); std::puts("API_KEYS_PASS"); }
