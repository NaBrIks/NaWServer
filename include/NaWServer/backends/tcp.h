// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/backends/tcp_transport.h>
#include <NaWServer/http_backend.h>
#include <NaWServer/buffer_memory.h>
#include <NaWServer/backends/tcp_managed.h>
#include <NaWServer/memory_manager.h>
#include <NaWServer/detail/synchronization.h>

namespace NaWServer
{

	class TcpHttpBackend;
	struct TcpHttpMemoryOptions
	{
		bool enabled = false;
		std::uint16_t initial_requests = 2, maximum_requests = 9, initial_websocket_clients = 0;
		// Physical HTTP reserve is opt-in, separate from identity/rate quotas.
		std::uint16_t reserved_requests = 0;
	};
	struct TcpKeepAliveOptions
	{
		bool enabled = false;
		std::uint16_t maximum_idle = 4, maximum_requests = 100;
		std::uint32_t idle_timeout_ms = 5000;
	};
	struct TcpMemoryStats
	{
		MemoryPoolStats headers, bodies, responses, streams;
		MemoryPoolStats reserved_headers, reserved_bodies, reserved_responses;
	};

	enum class TcpRuntimeState { Stopped, Starting, Listening, Recovering, Stopping, CleanupRequired, Failed };
	enum class TcpFailure { None, Storage, Listen, Wait, Listener, Accept, ConnectionCleanup, ListenerCleanup,
		                    WorkerStart };

	struct TcpDiagnostics
	{
		TcpRuntimeState state = TcpRuntimeState::Stopped;
		// Last failure survives recovery/stop; zero means no native code was available.
		TcpFailure last_failure = TcpFailure::None;
		int native_error = 0;
		std::uint16_t port = 0;
		// Saturating counters, reset by the next start attempt (not by recovery).
		std::uint32_t errors = 0, retries = 0, recoveries = 0;
	};

	struct TcpRecoveryOptions
	{
		bool enabled = true;
		// Positive delay between listener attempts, interruptible by stop().
		std::uint32_t retry_delay_ms = 250;
	};

	namespace tcp::detail
	{
		struct HttpRuntime;
		void retryTcpSseClose(TcpHttpBackend &backend) noexcept;
		std::size_t tcpSseCloseErrors(const TcpHttpBackend &backend) noexcept;
	}

	struct TcpOptions
	{
		// Copied into bounded backend storage during construction, not retained.
		const char *bind_address = "127.0.0.1";
		std::uint16_t port = 8080;
		int backlog = 7;
		std::uint32_t no_progress_timeout_ms = 1000;
		std::uint32_t wait_ceiling_ms = 20;
		std::uint32_t stop_timeout_ms = 5000;
		std::uint16_t max_websocket_clients = 4;
		std::uint32_t websocket_send_timeout_ms = 1000;
		// Unmanaged mode shares this pool across WS endpoints; managed mode uses
		// bounded per-stream arenas. Blocks live until completion/abort.
		// No per-send allocation or heap fallback.
		std::size_t websocket_tx_block_size = 4096;
		std::uint16_t websocket_tx_blocks = 4;
		BufferMemoryOptions buffer_memory{{}, defaultBufferLimit(BufferGroup::Tcp)};
		// Bounded graceful close for HTTP/WS. Allows peer FIN after a complete
		// reply; max discarded input is independent of receive-buffer allocation.
		std::uint32_t close_timeout_ms = 250;
		std::size_t close_drain_bytes = 8192;
		TcpManagedOptions managed;
		TcpHttpMemoryOptions http_memory;
		TcpKeepAliveOptions keep_alive;
		TcpRecoveryOptions recovery;
		tcp::TransmissionOptions transmission;
	};

	// One serialized callback/HTTP worker and, when WS routes exist, one WS I/O
	// worker. Incoming WS callbacks wait for the HTTP worker; outgoing WS/control
	// traffic does not. Optional bounded HTTP/1.1 keep-alive. Managed
	// workspaces and admission are independently selected. Transport
	// and handlers outlive successful stop; platform startup is application work.
	// Constructors start nothing. Drain the explicit SSE adapter before stop.
	class TcpHttpBackend final : public HttpBackend
	{
		friend struct tcp::detail::HttpRuntime;
		friend void tcp::detail::retryTcpSseClose(TcpHttpBackend &) noexcept;
		friend std::size_t tcp::detail::tcpSseCloseErrors(const TcpHttpBackend &) noexcept;
		tcp::Transport &transport_;
		TcpOptions options_;
		char address_[16]{};
		bool address_valid_ = false;
		mutable NaWServer::detail::Mutex diagnostics_mutex_;
		TcpDiagnostics diagnostics_;
		tcp::detail::HttpRuntime *runtime_ = nullptr;
		std::size_t sse_close_errors_ = 0;
		detail::BufferPool buffers_;
		void recordFailure(TcpFailure failure, int native_error);
		void setState(TcpRuntimeState state);

	public:
		explicit TcpHttpBackend(tcp::Transport &transport, TcpOptions options = {});
		~TcpHttpBackend() override;

		TcpHttpBackend(const TcpHttpBackend &) = delete;
		TcpHttpBackend &operator=(const TcpHttpBackend &) = delete;

		HttpError start(HttpRequestHandler &handler, const HttpLimits &limits) override;
		HttpError stop() override;
		std::uint16_t port() const;
		// Safe to sample while the worker runs, including during stop/recovery.
		// Other lifecycle calls remain owner-serialized. No runtime pointer escapes.
		TcpDiagnostics diagnostics() const;
		BufferMemoryStats bufferMemoryStats() const
		{
			return buffers_.stats();
		}

		TcpMemoryStats managedMemoryStats() const;
		tcp::TransmissionStats transmissionStats() const;
		bool usesSharedTransmission() const { return options_.transmission.enabled; }
	};

} // namespace NaWServer
