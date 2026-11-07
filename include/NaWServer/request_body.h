// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

namespace NaWServer
{
	enum class BodyReadStatus { Data, End, Timeout, Disconnected, TransportError };
	struct BodyReadResult
	{
		BodyReadStatus status = BodyReadStatus::End;
		std::size_t size = 0;
	};

	// Borrowed bytes, never a NUL-terminated string. The owner must outlive the view.
	class ByteView
	{
		const uint8_t *data_{};
		std::size_t size_{};

	public:
		constexpr ByteView() = default;
		constexpr ByteView(const uint8_t *data, std::size_t size)
		  : data_(data), size_(size)
		{
		}

		constexpr const uint8_t *data() const
		{
			return data_;
		}

		constexpr std::size_t size() const
		{
			return size_;
		}

		constexpr bool empty() const
		{
			return size_ == 0;
		}

	};

	enum class BodyCopyResult
	{
		Ok,
		BufferTooSmall,
		InvalidDestination
	};

	// Internal request storage, independent of HTTPD and any payload codec.
	// Owned moves transfer storage; borrowed views remain valid only as long as
	// their external owner. Callers copy explicitly into their chosen storage.
	class RequestBody
	{
		std::vector<uint8_t> bytes_;
		std::optional<ByteView> borrowed_;

	public:
		RequestBody() = default;
		explicit RequestBody(std::vector<uint8_t> bytes)
		  : bytes_(std::move(bytes))
		{
		}

		RequestBody(const RequestBody &) = delete;
		RequestBody &operator=(const RequestBody &) = delete;
		RequestBody(RequestBody &&other) noexcept
		  : bytes_(std::move(other.bytes_)), borrowed_(std::exchange(other.borrowed_, std::nullopt))
		{
		}

		RequestBody &operator=(RequestBody &&other) noexcept
		{
			if(this != &other)
			{
				bytes_ = std::move(other.bytes_);
				borrowed_ = std::exchange(other.borrowed_, std::nullopt);
			}

			return *this;
		}

		void assign(std::vector<uint8_t> bytes)
		{
			bytes_ = std::move(bytes);
			borrowed_.reset();
		}

		// Call only with a validated complete body whose owner outlives this view.
		void borrow(ByteView bytes)
		{
			bytes_.clear();
			borrowed_ = bytes;
		}

		ByteView view() const
		{
			return borrowed_.value_or(ByteView{bytes_.data(), bytes_.size()});
		}

		// All-or-nothing. nullptr is valid only for an empty body. Tail bytes are untouched.
		[[nodiscard]] BodyCopyResult copyTo(uint8_t *destination, std::size_t capacity) const
		{
			const auto bytes = view();
			if(capacity < bytes.size())
				return BodyCopyResult::BufferTooSmall;

			if(bytes.empty())
				return BodyCopyResult::Ok;

			if(!destination)
				return BodyCopyResult::InvalidDestination;

			std::memmove(destination, bytes.data(), bytes.size());
			return BodyCopyResult::Ok;
		}

		template <std::size_t N> [[nodiscard]] BodyCopyResult copyTo(uint8_t (&destination)[N]) const
		{
			return copyTo(destination, N);
		}

		template <std::size_t N> [[nodiscard]] BodyCopyResult copyTo(std::array<uint8_t, N> &destination) const
		{
			return copyTo(destination.data(), destination.size());
		}

		// assign reuses capacity where possible. Allocation failure follows std::vector's
		// policy/toolchain settings; Ok is not a recoverable out-of-memory guarantee.
		[[nodiscard]] BodyCopyResult copyTo(std::vector<uint8_t> &destination) const
		{
			const auto bytes = view();
			if(bytes.empty()) destination.clear();
			else destination.assign(bytes.data(), bytes.data() + bytes.size());

			return BodyCopyResult::Ok;
		}

	};
} // namespace NaWServer
