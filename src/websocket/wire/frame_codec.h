// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/request_body.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace NaWServer::websocket::wire
{

	struct MutableSpan
	{
		std::uint8_t *data = nullptr;
		std::size_t size = 0;
	};

	enum class DecodeStatus { NeedMore, Message, Ping, Pong, Close, Error, InvalidState };

	struct DecodeResult
	{
		DecodeStatus status = DecodeStatus::NeedMore;
		std::size_t consumed = 0;
		ByteView payload;
		bool binary = false;
		// Error: close to send. Close: received code, or internal-only 1005 when
		// absent (never put 1005 on wire). Ping/Pong/Message/NeedMore: zero.
		std::uint16_t close_code = 0;
	};

	struct FrameHeader
	{
		std::array<std::uint8_t, 10> bytes{};
		std::size_t size = 0;
	};

	// A final, unmasked server frame. Payload remains a separate borrowed span.
	// Only Text/Binary/Close/Ping/Pong opcodes are accepted. Failure is untouched.
	bool makeFrameHeader(std::uint8_t opcode, std::size_t payload_size, FrameHeader &output);
	bool validCloseCode(std::uint16_t code);

	// One receive owner. Data goes directly into borrowed final message storage;
	// header/control scratch is bounded to 14/125 bytes. No allocation or erase.
	// Message/control views end at the next commit/consume/reset or caller write
	// through writable(). Complete an application callback before receiving again.
	class FrameDecoder
	{
	public:
		FrameDecoder(std::uint8_t *message_storage, std::size_t capacity, std::size_t message_limit);

		FrameDecoder(const FrameDecoder &) = delete;
		FrameDecoder &operator=(const FrameDecoder &) = delete;

		// recv directly into this stage-limited span, then commit exactly its
		// positive returned size. Zero recv is transport EOF, not commit(0).
		MutableSpan writable();
		DecodeResult commit(std::size_t received);
		// For a prefetched HTTP tail. Consumes at most one observable event; the
		// caller retains any bytes after result.consumed for the next call.
		DecodeResult consume(const std::uint8_t *bytes, std::size_t size);
		void reset();

	private:
		std::uint8_t *message_storage_;
		std::size_t capacity_;
		std::size_t message_limit_;
		std::array<std::uint8_t, 14> header_{};
		std::array<std::uint8_t, 125> control_{};
		std::size_t header_used_ = 0;
		std::size_t header_needed_ = 2;
		std::size_t length_bytes_ = 0;
		std::size_t frame_length_ = 0;
		std::size_t frame_received_ = 0;
		std::size_t message_size_ = 0;
		std::uint8_t opcode_ = 0;
		bool final_ = false;
		bool fragmented_ = false;
		bool binary_ = false;
		bool payload_stage_ = false;
		DecodeStatus terminal_ = DecodeStatus::NeedMore;
		std::uint16_t terminal_code_ = 0;
		std::size_t close_payload_size_ = 0;

		bool validConfiguration() const;
		DecodeResult error(std::uint16_t code, std::size_t consumed);
		DecodeResult invalidState();
		DecodeResult terminalResult() const;
		DecodeResult headerComplete(std::size_t consumed);
		DecodeResult frameComplete(std::size_t consumed);
		void nextFrame();
	};

} // namespace NaWServer::websocket::wire
