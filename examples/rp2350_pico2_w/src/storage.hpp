#pragma once
#include <cstddef>
#include <cstdint>
#include <NaWServer/detail/synchronization.h>
#include "lfs.h"
#include <NaWServer/api_keys.h>

namespace rp_demo
{
	inline constexpr std::uint32_t flash_bytes = 4 * 1024 * 1024;
	inline constexpr std::uint32_t filesystem_offset = 0x200000;
	inline constexpr std::uint32_t filesystem_bytes = 0x1c0000;
	inline constexpr std::uint32_t settings_offset = 0x3c0000;
	inline constexpr std::uint32_t settings_bytes = 0x40000;
	static_assert(filesystem_offset + filesystem_bytes == settings_offset);
	static_assert(settings_offset + settings_bytes == flash_bytes);

	struct DeviceSettings
	{
		char name[48]{};
		char wifi_ssid[33]{};
		char wifi_password[65]{};
		std::uint32_t boot_count = 0;
		std::uint32_t save_count = 0;
	};

	class Storage
	{
	public:
		bool initialize();
		bool filesystemMounted() const;
		bool filesystemNeedsInitialization() const;
		bool initializeFilesystemExplicitly();
		bool readFile(const char *path, void *bytes, std::size_t capacity, std::size_t &size);
		// An open reader pins a file identity across bounded reads. All filesystem
		// operations remain serialized, but the lock is released during network I/O.
		bool openRead(const char *path, lfs_file_t &file, std::size_t &size);
		bool readPart(lfs_file_t &file, std::size_t offset, void *bytes, std::size_t capacity, std::size_t &size);
		void closeRead(lfs_file_t &file);
		bool writeFile(const char *path, const void *bytes, std::size_t size);
#if NAWSERVER_API_KEYS
        NaWServer::keys::ReadStatus readKeyRecord(const char *path, void *bytes, std::size_t capacity, std::size_t &size);
#endif
		bool seedWebAssets();
		std::uint32_t filesystemUsedBytes();
		DeviceSettings settings();
		bool saveSettings(const DeviceSettings &settings);
		std::uint64_t settingsGeneration() const;

	private:
		mutable NaWServer::detail::Mutex mutex_;
		lfs_t filesystem_{};
		lfs_config configuration_{};
		alignas(4) std::uint8_t read_cache_[256]{};
		alignas(4) std::uint8_t write_cache_[256]{};
		alignas(4) std::uint8_t lookahead_[64]{};
		bool mounted_ = false;
		bool needs_initialization_ = false;
		DeviceSettings settings_{};
		std::uint64_t generation_ = 0;
		std::uint32_t current_record_ = 0;
		void loadSettings();
	};
	Storage &storage();
}
