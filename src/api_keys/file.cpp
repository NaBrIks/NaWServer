// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/api_keys_file.h>
#if NAWSERVER_API_KEYS
#include <cstdio>
#include <filesystem>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>
#endif
namespace NaWServer::keys
{
	ReadStatus FileStorage::read(unsigned record, void *bytes, std::size_t capacity, std::size_t &size)
	{
		size = 0;
		if(record > 1 || !bytes) return ReadStatus::Error;
		const auto path = std::filesystem::u8path(base_path_ + (record ? ".1" : ".0"));
#ifdef _WIN32
		const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if(file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? ReadStatus::Missing : ReadStatus::Error;
		BY_HANDLE_FILE_INFORMATION info{}; DWORD received = 0;
		const bool ok = capacity <= std::numeric_limits<DWORD>::max() && GetFileInformationByHandle(file, &info) &&
		    !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !info.nFileSizeHigh && info.nFileSizeLow <= capacity &&
		    ReadFile(file, bytes, info.nFileSizeLow, &received, nullptr) && received == info.nFileSizeLow;
		CloseHandle(file); if(!ok) return ReadStatus::Error; size = received;
#else
		const auto file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if(file < 0) return errno == ENOENT ? ReadStatus::Missing : ReadStatus::Error;
		struct stat info{}; bool ok = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 && static_cast<std::uint64_t>(info.st_size) <= capacity;
		while(ok && size < static_cast<std::size_t>(info.st_size)) { const auto count = ::read(file, static_cast<char *>(bytes) + size, static_cast<std::size_t>(info.st_size) - size); if(count < 0 && errno == EINTR) continue; if(count <= 0) ok = false; else size += static_cast<std::size_t>(count); }
		::close(file); if(!ok) { size = 0; return ReadStatus::Error; }
#endif
		return ReadStatus::Ok;
	}
	bool FileStorage::write(unsigned record, const void *bytes, std::size_t size)
	{
		if(record > 1 || !bytes || !size) return false;
		const auto path = std::filesystem::u8path(base_path_ + (record ? ".1" : ".0")), temp = std::filesystem::u8path(base_path_ + ".tmp");
#ifdef _WIN32
		HANDLE token = nullptr; DWORD needed = 0;
		if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
		GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
		std::string user(needed, '\0');
		const bool got_user = GetTokenInformation(token, TokenUser, user.data(), needed, &needed) != FALSE; CloseHandle(token);
		LPWSTR sid = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
		if(!got_user || !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid)) return false;
		const std::wstring permissions = std::wstring(L"D:P(A;;FA;;;SY)(A;;FA;;;") + sid + L")"; LocalFree(sid);
		if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(permissions.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
		SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
		const auto file = CreateFileW(temp.c_str(), GENERIC_WRITE | WRITE_DAC, 0, &attributes, CREATE_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		bool ok = false; DWORD written = 0; BY_HANDLE_FILE_INFORMATION info{};
		if(file != INVALID_HANDLE_VALUE)
		{
			ok = SetKernelObjectSecurity(file, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) &&
			    GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
			    size <= std::numeric_limits<DWORD>::max() && WriteFile(file, bytes, static_cast<DWORD>(size), &written, nullptr) && written == size && FlushFileBuffers(file);
			CloseHandle(file);
		}
		LocalFree(descriptor);
		if(!ok) { DeleteFileW(temp.c_str()); return false; }
		return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
		const auto file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
		if(file < 0) return false;
		bool ok = ::fchmod(file, S_IRUSR | S_IWUSR) == 0; std::size_t used = 0;
		while(ok && used < size) { const auto count = ::write(file, static_cast<const char *>(bytes) + used, size - used); if(count < 0 && errno == EINTR) continue; if(count <= 0) ok = false; else used += static_cast<std::size_t>(count); }
		if(ok) ok = ::fsync(file) == 0;
		if(::close(file) != 0) ok = false;
		if(!ok || ::rename(temp.c_str(), path.c_str()) != 0) { ::unlink(temp.c_str()); return false; }
		auto parent = path.parent_path(); if(parent.empty()) parent = ".";
		const auto directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if(directory < 0) return false;
		ok = ::fsync(directory) == 0; ::close(directory); return ok;
#endif
	}
	bool SystemRandom::fill(std::uint8_t *bytes, std::size_t size)
	{
		if(!bytes && size) return false;
#ifdef _WIN32
		return size <= std::numeric_limits<ULONG>::max() && BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
		std::size_t used = 0;
		while(used < size) { const auto count = ::getrandom(bytes + used, size - used, 0); if(count < 0 && errno == EINTR) continue; if(count <= 0) return false; used += static_cast<std::size_t>(count); }
		return true;
#endif
	}
}
#endif
