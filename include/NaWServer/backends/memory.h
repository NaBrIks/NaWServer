// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/http_backend.h>
#include <NaWServer/websocket_backend.h>
#include <limits>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace NaWServer
{
	class MemoryWebSocketExchange;
	class MemorySseStream;
	struct MemoryRequest
	{
		HttpMethod method = HttpMethod::Get;
		std::string target = "/";
		std::vector<std::pair<std::string, std::string> > headers;
		std::vector<uint8_t> body;
		// Independent advertised length permits truncated/oversized request tests.
		std::size_t content_length = 0;
		std::size_t read_chunk_size = std::numeric_limits<std::size_t>::max();
		std::size_t fail_read_after = std::numeric_limits<std::size_t>::max();
		BodyReadStatus read_failure = BodyReadStatus::Timeout;
		bool fail_send = false;
	};
	struct MemoryResponse
	{
		int status = 0;
		std::string content_type;
		std::vector<std::pair<std::string, std::string> > headers;
		std::vector<uint8_t> body;
		std::size_t send_calls = 0;
		std::size_t read_calls = 0;
		ExchangeDisposition disposition = ExchangeDisposition::CloseConnection;
		HttpError result = HttpError::InvalidState;
		std::shared_ptr<MemorySseStream> sse_stream;
	};
	struct MemoryWebSocketHandle
	{
		std::size_t slot = std::numeric_limits<std::size_t>::max();
		std::uint64_t generation = 0;
		explicit operator bool() const {
			return generation != 0;
		}
	};
	struct MemoryWebSocketMessage
	{
		std::vector<uint8_t> bytes;
		bool binary = false;
	};
	// Synchronous, single-owner backend. Constructors allocate/start nothing.
	class MemoryHttpBackend final : public HttpBackend
	{
		HttpRequestHandler *handler_ = nullptr;
		HttpLimits limits_;
		bool handling_ = false;
		std::size_t sse_capture_capacity_ = 16 * 1024;
		struct WebSocketSlot;
		class MemorySession;
		friend class MemorySession;
		friend class MemoryWebSocketExchange;
		std::vector<std::unique_ptr<WebSocketSlot> > websocket_slots_;
		std::uint64_t next_generation_ = 1;
		bool stopWebSocket(MemoryWebSocketHandle handle);
		WebSocketSendResult queueWebSocket(std::size_t slot, std::uint64_t generation,
		    const uint8_t *data, std::size_t size, bool binary);
	public:
		explicit MemoryHttpBackend(std::size_t sse_capture_capacity = 16 * 1024);
		~MemoryHttpBackend() override;
		HttpError start(HttpRequestHandler &handler, const HttpLimits &limits) override;
		HttpError stop() override;
		MemoryResponse perform(const MemoryRequest &request);
		MemoryWebSocketHandle connectWebSocket(const std::string &path);
		WebSocketReadStatus receive(MemoryWebSocketHandle handle, const uint8_t *data,
		    std::size_t size, bool binary);
		WebSocketReadStatus receive(MemoryWebSocketHandle handle, const std::vector<uint8_t> &data, bool binary)
		{
			return receive(handle, data.data(), data.size(), binary);
		}

		std::vector<MemoryWebSocketMessage> takeSent(MemoryWebSocketHandle handle);
		bool close(MemoryWebSocketHandle handle);
		std::size_t websocketCount() const;
		std::size_t pendingMessageCount(MemoryWebSocketHandle handle) const;
	};
}
