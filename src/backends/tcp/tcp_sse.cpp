// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/backends/tcp.h>
#include <NaWServer/backends/tcp_sse.h>
#include <NaWServer/request.h>
#include "sse_stream.h"
#include "platform/worker.h"
#include <cstdlib>

namespace NaWServer::sse
{

	TcpAdapter::TcpAdapter(TcpHttpBackend &backend, Limits limits)
	  : QueuedAdapter(limits), backend_(backend)
	{
	}

	TcpAdapter::~TcpAdapter()
	{
		if(worker_ && !stop(0)) std::abort();
	}

	bool TcpAdapter::start()
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(lifecycle_mutex_);
		if(worker_ || !signal_.initialize() || !beginRuntime(true, backend_.usesSharedTransmission())) return false;

		stop_requested_.store(false, std::memory_order_release);
		worker_ = tcp::detail::startTcpWorker(entry, this, tcp::detail::WorkerRole::EventStream);
		if(!worker_)
		{
			requestStop();
			poll(tcp::detail::tcpMilliseconds());
			(void)finishRuntime();
			return false;
		}

		return true;
	}

	bool TcpAdapter::stop(std::uint32_t timeout_ms)
	{
		if(tcp::detail::isTcpWorkerContext(this)) return false;

		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(lifecycle_mutex_);
		if(!worker_) return true;

		requestStop();
		tcp::detail::retryTcpSseClose(backend_);
		stop_requested_.store(true, std::memory_order_release);
		signal_.notify();
		if(!tcp::detail::joinTcpWorker(worker_, timeout_ms)) return false;

		worker_ = nullptr;
		return true;
	}

	Admit TcpAdapter::reserve(Request &request, const Frame &preface, Reservation &reservation)
	{
		auto *exchange = static_cast<Exchange *>(request.transportExtension(&exchange_key));
		if(!exchange || exchange->owner() != &backend_) return Admit::InvalidRequest;

		return QueuedAdapter::reserve(request, preface, reservation);
	}

	std::size_t TcpAdapter::close_errors() const
	{
		return tcp::detail::tcpSseCloseErrors(backend_);
	}

	void TcpAdapter::entry(void *context)
	{
		static_cast<TcpAdapter *>(context)->run();
	}

	void TcpAdapter::wakeRuntime()
	{
		signal_.notify();
	}

	std::uint64_t TcpAdapter::runtimeMilliseconds() const
	{
		return tcp::detail::tcpMilliseconds();
	}

	void TcpAdapter::run()
	{
		for(;;)
		{
			poll(tcp::detail::tcpMilliseconds());
			if(stop_requested_.load(std::memory_order_acquire) && finishRuntime()) return;

			(void)signal_.wait(pollDelay(tcp::detail::tcpMilliseconds()));
		}
	}

} // namespace NaWServer::sse
