// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace NaWServer
{

	enum class RequestResource : std::uint8_t { Http, WebSocket, EventStream };

	// The middleware owns policy and identity. The I/O owner only retains this
	// permit while resources remain in use, and returns it on completion/abort.
	// Callbacks are serialized with the owning policy, never invoked under a
	// transport writer lock. No borrowed request/header views are retained.
	class RequestResourceLease
	{
	public:
		using Token = std::array<std::uint64_t, 4>;
		using Release = void (*)(void *, const Token &);
		using Reclassify = bool (*)(void *, Token &, RequestResource);

	private:
		void *context_ = nullptr;
		Token token_{};
		Release release_ = nullptr;
		Reclassify reclassify_ = nullptr;
		bool reserved_ = false;
		RequestResource resource_ = RequestResource::Http;

	public:
		RequestResourceLease() = default;
		RequestResourceLease(void *context, Token token, Release release, Reclassify reclassify,
		    bool reserved, RequestResource resource = RequestResource::Http)
		  : context_(context), token_(token), release_(release), reclassify_(reclassify),
		    reserved_(reserved), resource_(resource)
		{
		}

		~RequestResourceLease()
		{
			reset();
		}

		RequestResourceLease(const RequestResourceLease &) = delete;
		RequestResourceLease &operator=(const RequestResourceLease &) = delete;
		RequestResourceLease(RequestResourceLease &&other) noexcept
		{
			*this = std::move(other);
		}

		RequestResourceLease &operator=(RequestResourceLease &&other) noexcept
		{
			if(this == &other) return *this;
			reset();
			context_ = other.context_;
			token_ = other.token_;
			release_ = std::exchange(other.release_, nullptr);
			reclassify_ = std::exchange(other.reclassify_, nullptr);
			reserved_ = std::exchange(other.reserved_, false);
			resource_ = other.resource_;
			return *this;
		}

		explicit operator bool() const
		{
			return release_ != nullptr;
		}

		bool reserved() const
		{
			return release_ && reserved_;
		}

		RequestResource resource() const
		{
			return resource_;
		}

		bool reclassify(RequestResource resource)
		{
			if(!release_ || resource == resource_) return true;
			if(!reclassify_ || !reclassify_(context_, token_, resource)) return false;
			resource_ = resource;
			return true;
		}

		void reset()
		{
			const auto release = std::exchange(release_, nullptr);
			if(release) release(context_, token_);
			context_ = nullptr;
			reclassify_ = nullptr;
			reserved_ = false;
			token_ = {};
		}
	};

} // namespace NaWServer
