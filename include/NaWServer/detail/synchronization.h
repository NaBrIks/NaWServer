// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <cstddef>

#if !defined(NAWSERVER_FREERTOS_MUTEX) || !NAWSERVER_FREERTOS_MUTEX
#include <mutex>
#endif

namespace NaWServer::detail
{

#if defined(NAWSERVER_FREERTOS_MUTEX) && NAWSERVER_FREERTOS_MUTEX
#ifndef NAWSERVER_MUTEX_STORAGE_BYTES
#define NAWSERVER_MUTEX_STORAGE_BYTES 128
#endif

	// This private representation deliberately contains no RTOS headers. The
	// selected implementation checks its size/alignment against the real SDK.
	class Mutex
	{
		alignas(std::max_align_t) unsigned char storage_[NAWSERVER_MUTEX_STORAGE_BYTES]{};
		void *handle_ = nullptr;
	public:
		Mutex() noexcept;
		~Mutex();
		Mutex(const Mutex &) = delete;
		Mutex &operator=(const Mutex &) = delete;
		void lock();
		bool try_lock();
		void unlock();
	};
#else
	using Mutex = std::mutex;
#endif

	struct TryToLock {};
	inline constexpr TryToLock tryToLock{};

	template <class MutexType> class LockGuard
	{
		MutexType &mutex_;
	public:
		explicit LockGuard(MutexType &mutex) : mutex_(mutex) { mutex_.lock(); }
		~LockGuard() { mutex_.unlock(); }
		LockGuard(const LockGuard &) = delete;
		LockGuard &operator=(const LockGuard &) = delete;
	};

	template <class MutexType> class UniqueLock
	{
		MutexType &mutex_;
		bool owns_ = false;
	public:
		explicit UniqueLock(MutexType &mutex) : mutex_(mutex), owns_(true) { mutex_.lock(); }
		UniqueLock(MutexType &mutex, TryToLock) : mutex_(mutex), owns_(mutex_.try_lock()) {}
		~UniqueLock() { if(owns_) mutex_.unlock(); }
		UniqueLock(const UniqueLock &) = delete;
		UniqueLock &operator=(const UniqueLock &) = delete;
		bool owns_lock() const { return owns_; }
		void unlock() { mutex_.unlock(); owns_ = false; }
		void lock() { mutex_.lock(); owns_ = true; }
	};

} // namespace NaWServer::detail
