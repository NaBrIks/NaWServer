#include "storage.hpp"
#include "demo_assets.hpp"
#include "wifi_config.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace rp_demo
{
	namespace
	{
		constexpr std::uint32_t record_magic = 0x52504e56;
		constexpr std::uint32_t commit_magic = 0x434f4d54;
		constexpr std::uint32_t bank_bytes = settings_bytes / 2;
		struct Record
		{
			std::uint32_t magic, schema;
			std::uint64_t generation;
			std::uint32_t size, checksum;
			DeviceSettings settings;
		};
		struct Commit
		{
			std::uint32_t magic, checksum;
			std::uint64_t generation;
		};
		static_assert(sizeof(Record) <= FLASH_PAGE_SIZE);

		std::uint32_t checksum(const void *bytes, std::size_t size)
		{
			auto *data = static_cast<const std::uint8_t *>(bytes);
			std::uint32_t value = 0xffffffff;
			for(std::size_t index = 0; index < size; ++index)
			{
				value ^= data[index];
				for(unsigned bit = 0; bit < 8; ++bit)
					value = (value >> 1) ^ (0xedb88320u & (0u - (value & 1u)));
			}
			return ~value;
		}

		struct FlashOperation
		{
			std::uint32_t offset, size;
			const std::uint8_t *bytes;
		};

		void __not_in_flash_func(operateFlash)(void *argument)
		{
			auto &operation = *static_cast<FlashOperation *>(argument);
			if(operation.bytes) flash_range_program(operation.offset, operation.bytes, operation.size);
			else flash_range_erase(operation.offset, operation.size);
		}

		bool erase(std::uint32_t offset, std::uint32_t size)
		{
			if(offset < filesystem_offset || offset + size > flash_bytes ||
			    offset % FLASH_SECTOR_SIZE || size % FLASH_SECTOR_SIZE) return false;
			FlashOperation operation{offset, size, nullptr};
			return flash_safe_execute(operateFlash, &operation, 1000) == PICO_OK;
		}

		bool program(std::uint32_t offset, const void *bytes, std::uint32_t size)
		{
			if(offset < filesystem_offset || offset + size > flash_bytes ||
			    offset % FLASH_PAGE_SIZE || size % FLASH_PAGE_SIZE) return false;
			// The source can be an XIP seed asset. Copy each page to RAM before the
			// flash command disables XIP; no task may observe an in-progress update.
			alignas(4) std::array<std::uint8_t, FLASH_PAGE_SIZE> page;
			for(std::uint32_t index = 0; index < size; index += FLASH_PAGE_SIZE)
			{
				std::memcpy(page.data(), static_cast<const std::uint8_t *>(bytes) + index, page.size());
				FlashOperation operation{offset + index, FLASH_PAGE_SIZE, page.data()};
				if(flash_safe_execute(operateFlash, &operation, 1000) != PICO_OK) return false;
			}
			return true;
		}

		bool blank(std::uint32_t offset, std::uint32_t size)
		{
			const auto *bytes = reinterpret_cast<const std::uint8_t *>(XIP_BASE + offset);
			return std::all_of(bytes, bytes + size, [](std::uint8_t value) { return value == 0xff; });
		}

		int readBlock(const lfs_config *, lfs_block_t block, lfs_off_t offset, void *bytes, lfs_size_t size)
		{
			if(block >= filesystem_bytes / FLASH_SECTOR_SIZE || offset + size > FLASH_SECTOR_SIZE) return LFS_ERR_IO;
			std::memcpy(bytes, reinterpret_cast<const void *>(XIP_BASE + filesystem_offset +
			    block * FLASH_SECTOR_SIZE + offset), size);
			return 0;
		}

		int programBlock(const lfs_config *, lfs_block_t block, lfs_off_t offset, const void *bytes, lfs_size_t size)
		{
			if(block >= filesystem_bytes / FLASH_SECTOR_SIZE || offset + size > FLASH_SECTOR_SIZE) return LFS_ERR_IO;
			return program(filesystem_offset + block * FLASH_SECTOR_SIZE + offset, bytes, size) ? 0 : LFS_ERR_IO;
		}

		int eraseBlock(const lfs_config *, lfs_block_t block)
		{
			if(block >= filesystem_bytes / FLASH_SECTOR_SIZE) return LFS_ERR_IO;
			return erase(filesystem_offset + block * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE) ? 0 : LFS_ERR_IO;
		}

		int syncBlocks(const lfs_config *) { return 0; }
	}

	Storage &storage()
	{
		static Storage instance;
		return instance;
	}

	bool Storage::initialize()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		configuration_.read = readBlock;
		configuration_.prog = programBlock;
		configuration_.erase = eraseBlock;
		configuration_.sync = syncBlocks;
		configuration_.read_size = 256;
		configuration_.prog_size = 256;
		configuration_.block_size = FLASH_SECTOR_SIZE;
		configuration_.block_count = filesystem_bytes / FLASH_SECTOR_SIZE;
		configuration_.block_cycles = 500;
		configuration_.cache_size = 256;
		configuration_.lookahead_size = sizeof(lookahead_);
		configuration_.read_buffer = read_cache_;
		configuration_.prog_buffer = write_cache_;
		configuration_.lookahead_buffer = lookahead_;
		mounted_ = lfs_mount(&filesystem_, &configuration_) == 0;
		needs_initialization_ = !mounted_;
		// Only an entirely erased partition is initialized automatically. A mount
		// failure on nonblank storage is reported without destroying its contents.
		if(!mounted_ && blank(filesystem_offset, filesystem_bytes))
		{
			mounted_ = lfs_format(&filesystem_, &configuration_) == 0 &&
			    lfs_mount(&filesystem_, &configuration_) == 0;
			needs_initialization_ = !mounted_;
		}
		loadSettings();
		return mounted_;
	}

	bool Storage::filesystemMounted() const { return mounted_; }
	bool Storage::filesystemNeedsInitialization() const { return needs_initialization_; }

	bool Storage::initializeFilesystemExplicitly()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(mounted_) return true;
		mounted_ = lfs_format(&filesystem_, &configuration_) == 0 && lfs_mount(&filesystem_, &configuration_) == 0;
		needs_initialization_ = !mounted_;
		return mounted_;
	}

	bool Storage::readFile(const char *path, void *bytes, std::size_t capacity, std::size_t &size)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!mounted_) return false;
		lfs_info information{};
		if(lfs_stat(&filesystem_, path, &information) || information.type != LFS_TYPE_REG || information.size > capacity)
			return false;
		lfs_file_t file{};
		if(lfs_file_open(&filesystem_, &file, path, LFS_O_RDONLY)) return false;
		const auto result = lfs_file_read(&filesystem_, &file, bytes, information.size);
		const auto closed = lfs_file_close(&filesystem_, &file);
		if(result < 0 || static_cast<std::size_t>(result) != information.size || closed) return false;
		size = result;
		return true;
	}

	bool Storage::openRead(const char *path, lfs_file_t &file, std::size_t &size)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!mounted_ || lfs_file_open(&filesystem_, &file, path, LFS_O_RDONLY)) return false;

		const auto length = lfs_file_size(&filesystem_, &file);
		if(length < 0) { (void)lfs_file_close(&filesystem_, &file); return false; }

		size = static_cast<std::size_t>(length);
		return true;
	}

	bool Storage::readPart(lfs_file_t &file, std::size_t offset, void *bytes, std::size_t capacity, std::size_t &size)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!mounted_ || lfs_file_seek(&filesystem_, &file, offset, LFS_SEEK_SET) < 0) return false;

		const auto read = lfs_file_read(&filesystem_, &file, bytes, capacity);
		if(read < 0) return false;

		size = static_cast<std::size_t>(read);
		return true;
	}

	void Storage::closeRead(lfs_file_t &file)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		(void)lfs_file_close(&filesystem_, &file);
	}

	#if NAWSERVER_API_KEYS
 NaWServer::keys::ReadStatus Storage::readKeyRecord(const char *path, void *bytes, std::size_t capacity, std::size_t &size)
 {
  NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
  size=0;
  if(!mounted_)return NaWServer::keys::ReadStatus::Error;
  lfs_info info{};const auto status=lfs_stat(&filesystem_,path,&info);
  if(status==LFS_ERR_NOENT)return NaWServer::keys::ReadStatus::Missing;
  if(status||info.type!=LFS_TYPE_REG||info.size>capacity)return NaWServer::keys::ReadStatus::Error;
  lfs_file_t file{};if(lfs_file_open(&filesystem_,&file,path,LFS_O_RDONLY))return NaWServer::keys::ReadStatus::Error;
  const auto got=lfs_file_read(&filesystem_,&file,bytes,info.size);const auto closed=lfs_file_close(&filesystem_,&file);
  if(got<0||static_cast<std::size_t>(got)!=info.size||closed)return NaWServer::keys::ReadStatus::Error;
  size=info.size;return NaWServer::keys::ReadStatus::Ok;
 }
#endif
	bool Storage::writeFile(const char *path, const void *bytes, std::size_t size)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!mounted_ || !path || std::strlen(path) > 100 || std::strstr(path, "..")) return false;
		char temporary[112];
		std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
		lfs_file_t file{};
		if(lfs_file_open(&filesystem_, &file, temporary, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC)) return false;
		const auto written = lfs_file_write(&filesystem_, &file, bytes, size);
		const auto synced = lfs_file_sync(&filesystem_, &file);
		const auto closed = lfs_file_close(&filesystem_, &file);
		if(written < 0 || static_cast<std::size_t>(written) != size || synced || closed) return false;
		return lfs_rename(&filesystem_, temporary, path) == 0;
	}

	bool Storage::seedWebAssets()
	{
		for(const auto &asset : seed_assets)
		{
			// Versioned seed files are demo inputs; normal firmware updates leave
			// existing FS data intact. Explicit reseeding updates only these names.
			if(!writeFile(asset.storage_path, asset.bytes, asset.size)) return false;
		}
		return true;
	}

	std::uint32_t Storage::filesystemUsedBytes()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		if(!mounted_) return 0;
		const auto blocks = lfs_fs_size(&filesystem_);
		return blocks < 0 ? 0 : static_cast<std::uint32_t>(blocks) * FLASH_SECTOR_SIZE;
	}

	void Storage::loadSettings()
	{
		std::strcpy(settings_.name, "Pico 2 W");
		std::strncpy(settings_.wifi_ssid, WIFI_SSID, sizeof(settings_.wifi_ssid) - 1);
		std::strncpy(settings_.wifi_password, WIFI_PASSWORD, sizeof(settings_.wifi_password) - 1);
		for(std::uint32_t offset = settings_offset; offset < flash_bytes; offset += 512)
		{
			Record record;
			Commit commit;
			std::memcpy(&record, reinterpret_cast<const void *>(XIP_BASE + offset), sizeof(record));
			std::memcpy(&commit, reinterpret_cast<const void *>(XIP_BASE + offset + 256), sizeof(commit));
			if(record.magic != record_magic || record.schema != 1 || record.size != sizeof(DeviceSettings) ||
			    commit.magic != commit_magic || commit.generation != record.generation ||
			    record.checksum != commit.checksum || record.checksum != checksum(&record.settings, sizeof(record.settings)))
				continue;
			if(record.generation > generation_)
			{
				settings_ = record.settings;
				settings_.name[sizeof(settings_.name) - 1] = 0;
				settings_.wifi_ssid[sizeof(settings_.wifi_ssid) - 1] = 0;
				settings_.wifi_password[sizeof(settings_.wifi_password) - 1] = 0;
				generation_ = record.generation;
				current_record_ = offset;
			}
		}
	}

	DeviceSettings Storage::settings()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return settings_;
	}

	std::uint64_t Storage::settingsGeneration() const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		return generation_;
	}

	bool Storage::saveSettings(const DeviceSettings &settings)
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		const auto current_bank = current_record_ >= settings_offset + bank_bytes ? 1u : 0u;
		std::uint32_t offset = generation_ ? current_record_ + 512 : settings_offset;
		const auto bank_end = settings_offset + (current_bank + 1) * bank_bytes;
		while(offset < bank_end && !blank(offset, 512)) offset += 512;
		if(offset >= bank_end)
		{
			offset = settings_offset + (1u - current_bank) * bank_bytes;
			if(!erase(offset, bank_bytes)) return false;
		}
		std::array<std::uint8_t, 256> body, publication;
		body.fill(0xff);
		publication.fill(0xff);
		Record record{record_magic, 1, generation_ + 1, sizeof(DeviceSettings), checksum(&settings, sizeof(settings)), settings};
		Commit commit{commit_magic, record.checksum, record.generation};
		std::memcpy(body.data(), &record, sizeof(record));
		std::memcpy(publication.data(), &commit, sizeof(commit));
		if(!program(offset, body.data(), 256) ||
		    std::memcmp(reinterpret_cast<const void *>(XIP_BASE + offset), body.data(), 256) ||
		    !program(offset + 256, publication.data(), 256) ||
		    std::memcmp(reinterpret_cast<const void *>(XIP_BASE + offset + 256), publication.data(), 256)) return false;
		settings_ = settings;
		generation_ = record.generation;
		current_record_ = offset;
		return true;
	}
}
