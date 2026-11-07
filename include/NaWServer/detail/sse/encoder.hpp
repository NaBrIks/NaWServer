// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace NaWServer::sse
{
	constexpr std::size_t frame_capacity = 768;
	struct Frame
	{
		std::array<char, frame_capacity> bytes{};
		std::size_t size = 0;
	};
	struct Event
	{
		std::string_view data;
		std::optional<std::string_view> name;
		std::optional<std::string_view> id;
		std::optional<std::uint32_t> retry_ms;
	};
	enum class EncodeResult
	{
		Ok,
		InvalidUtf8,
		InvalidField,
		TooLarge
	};
	// No allocation. Failure leaves out untouched. Data/comment allow U+0000;
	// event/id reject CR, LF and NUL. CR/CRLF in data normalize to LF on receipt.
	EncodeResult encode(const Event &event, Frame &out);
	EncodeResult comment(std::string_view text, Frame &out);
	bool valid_field(std::string_view value);
	bool valid_utf8(std::string_view value);
} // namespace NaWServer::sse
