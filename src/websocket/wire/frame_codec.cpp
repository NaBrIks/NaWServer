// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "frame_codec.h"
#include "detail/utf8.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace NaWServer::websocket::wire
{

	bool validCloseCode(std::uint16_t code)
	{
		// RFC 6455 plus registered 1012-1014; 1004/1005/1006/1015 are not wire codes.
		return code == 1000 || code == 1001 || code == 1002 || code == 1003 ||
		       (code >= 1007 && code <= 1014) || (code >= 3000 && code <= 4999);
	}

	bool makeFrameHeader(std::uint8_t opcode, std::size_t payload_size, FrameHeader &output)
	{
		if(opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)
			return false;

		if((opcode & 8) && payload_size > 125)
			return false;

		if(opcode == 8 && payload_size == 1)
			return false;

		const std::uint64_t payload_length = payload_size;
		if((payload_length >> 63) != 0)
			return false;

		FrameHeader header;
		header.bytes[0] = static_cast<std::uint8_t>(0x80 | opcode);
		if(payload_size <= 125)
		{
			header.bytes[1] = static_cast<std::uint8_t>(payload_size);
			header.size = 2;
		}
		else if(payload_size <= 65535)
		{
			header.bytes[1] = 126;
			header.bytes[2] = static_cast<std::uint8_t>(payload_size >> 8);
			header.bytes[3] = static_cast<std::uint8_t>(payload_size);
			header.size = 4;
		}
		else
		{
			header.bytes[1] = 127;
			for(unsigned index = 0; index < 8; ++index)
				header.bytes[2 + index] = static_cast<std::uint8_t>(payload_length >>
				    (56 - index * 8));

			header.size = 10;
		}

		output = header;
		return true;
	}

	FrameDecoder::FrameDecoder(std::uint8_t *message_storage, std::size_t capacity, std::size_t message_limit)
	  : message_storage_(message_storage), capacity_(capacity), message_limit_(message_limit)
	{
	}

	bool FrameDecoder::validConfiguration() const
	{
		return message_storage_ && message_limit_ > 0 && message_limit_ <= capacity_;
	}

	MutableSpan FrameDecoder::writable()
	{
		if(!validConfiguration() || terminal_ != DecodeStatus::NeedMore)
			return {};

		if(!payload_stage_)
			return {header_.data() + header_used_, header_needed_ - header_used_};

		if(opcode_ & 8)
			return {control_.data() + frame_received_, frame_length_ - frame_received_};

		return {message_storage_ + message_size_ + frame_received_, frame_length_ - frame_received_};
	}

	DecodeResult FrameDecoder::terminalResult() const
	{
		return {terminal_, 0,
		        terminal_ == DecodeStatus::Close ? ByteView(control_.data(), close_payload_size_) : ByteView{},
		        false, terminal_code_};
	}

	DecodeResult FrameDecoder::error(std::uint16_t code, std::size_t consumed)
	{
		terminal_ = DecodeStatus::Error;
		terminal_code_ = code;
		return {terminal_, consumed, {}, false, code};
	}

	DecodeResult FrameDecoder::invalidState()
	{
		terminal_ = DecodeStatus::InvalidState;
		terminal_code_ = 0;
		return terminalResult();
	}

	void FrameDecoder::nextFrame()
	{
		header_used_ = 0;
		header_needed_ = 2;
		length_bytes_ = 0;
		frame_length_ = 0;
		frame_received_ = 0;
		payload_stage_ = false;
	}

	DecodeResult FrameDecoder::headerComplete(std::size_t consumed)
	{
		if(header_used_ == 2)
		{
			opcode_ = header_[0] & 15;
			final_ = (header_[0] & 0x80) != 0;
			if((header_[0] & 0x70) || !(header_[1] & 0x80) ||
			    (opcode_ != 0 && opcode_ != 1 && opcode_ != 2 && opcode_ != 8 && opcode_ != 9 && opcode_ != 10))
			{
				return error(1002, consumed);
			}

			const auto compact_length = header_[1] & 0x7f;
			if((opcode_ & 8) && (!final_ || compact_length > 125))
				return error(1002, consumed);

			length_bytes_ = compact_length == 126 ? 2 : compact_length == 127 ? 8 : 0;
			header_needed_ = 2 + length_bytes_;
			if(length_bytes_)
				return {DecodeStatus::NeedMore, consumed, {}, false, 0};
		}

		if(header_used_ == 2 + length_bytes_)
		{
			std::uint64_t length = header_[1] & 0x7f;
			if(length_bytes_)
			{
				length = 0;
				for(std::size_t index = 0; index < length_bytes_; ++index)
					length = (length << 8) | header_[2 + index];

				if((length_bytes_ == 2 && length < 126) ||
				    (length_bytes_ == 8 && ((header_[2] & 0x80) || length < 65536)))
				{
					return error(1002, consumed);
				}
			}

			if(!(opcode_ & 8))
			{
				if((opcode_ == 0 && !fragmented_) || (opcode_ != 0 && fragmented_))
					return error(1002, consumed);

				if(opcode_ != 0)
				{
					message_size_ = 0;
					binary_ = opcode_ == 2;
				}

				if(length > message_limit_ - message_size_)
					return error(1009, consumed);

				if(!final_)
					fragmented_ = true;
			}

			frame_length_ = static_cast<std::size_t>(length);
			header_needed_ += 4;
			return {DecodeStatus::NeedMore, consumed, {}, false, 0};
		}

		payload_stage_ = true;
		if(frame_length_ == 0)
			return frameComplete(consumed);

		return {DecodeStatus::NeedMore, consumed, {}, false, 0};
	}

	DecodeResult FrameDecoder::frameComplete(std::size_t consumed)
	{
		if(opcode_ & 8)
		{
			const auto size = frame_length_;
			if(opcode_ == 8)
			{
				if(size == 1)
					return error(1002, consumed);

				std::uint16_t code = 1005;
				if(size >= 2)
				{
					code = static_cast<std::uint16_t>((static_cast<std::uint16_t>(control_[0]) << 8) | control_[1]);
					if(!validCloseCode(code))
						return error(1002, consumed);

					if(!NaWServer::detail::validUtf8({reinterpret_cast<const char *>(control_.data() + 2), size - 2}))
						return error(1007, consumed);
				}

				terminal_ = DecodeStatus::Close;
				terminal_code_ = code;
				close_payload_size_ = size;
				return {terminal_, consumed, {control_.data(), size}, false, code};
			}

			const auto status = opcode_ == 9 ? DecodeStatus::Ping : DecodeStatus::Pong;
			nextFrame();
			return {status, consumed, {control_.data(), size}, false, 0};
		}

		message_size_ += frame_length_;
		if(!final_)
		{
			nextFrame();
			return {DecodeStatus::NeedMore, consumed, {}, false, 0};
		}

		if(!binary_ && !NaWServer::detail::validUtf8({reinterpret_cast<const char *>(message_storage_), message_size_}))
			return error(1007, consumed);

		fragmented_ = false;
		const DecodeResult result{DecodeStatus::Message, consumed, {message_storage_, message_size_}, binary_, 0};
		nextFrame();
		return result;
	}

	DecodeResult FrameDecoder::commit(std::size_t received)
	{
		if(terminal_ != DecodeStatus::NeedMore)
			return terminalResult();

		const auto destination = writable();
		if(received == 0 || received > destination.size || !destination.data)
			return invalidState();

		if(!payload_stage_)
		{
			header_used_ += received;
			return header_used_ == header_needed_ ? headerComplete(received) :
			       DecodeResult{DecodeStatus::NeedMore, received, {}, false, 0};
		}

		for(std::size_t index = 0; index < received; ++index)
			destination.data[index] ^= header_[2 + length_bytes_ + ((frame_received_ + index) & 3)];

		frame_received_ += received;
		return frame_received_ == frame_length_ ? frameComplete(received) :
		       DecodeResult{DecodeStatus::NeedMore, received, {}, false, 0};
	}

	DecodeResult FrameDecoder::consume(const std::uint8_t *bytes, std::size_t size)
	{
		if(terminal_ != DecodeStatus::NeedMore)
			return terminalResult();

		if((!bytes && size) || !validConfiguration())
			return invalidState();

		std::size_t consumed = 0;
		while(consumed < size)
		{
			const auto destination = writable();
			if(!destination.data || destination.size == 0)
				return invalidState();

			const auto amount = std::min(size - consumed, destination.size);
			std::memmove(destination.data, bytes + consumed, amount);
			DecodeResult result = commit(amount);
			consumed += amount;
			if(result.status != DecodeStatus::NeedMore)
			{
				result.consumed = consumed;
				return result;
			}
		}
		return {DecodeStatus::NeedMore, consumed, {}, false, 0};
	}

	void FrameDecoder::reset()
	{
		nextFrame();
		message_size_ = 0;
		opcode_ = 0;
		final_ = false;
		fragmented_ = false;
		binary_ = false;
		terminal_ = DecodeStatus::NeedMore;
		terminal_code_ = 0;
		close_payload_size_ = 0;
	}

} // namespace NaWServer::websocket::wire
