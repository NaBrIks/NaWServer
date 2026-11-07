// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/http_backend.h>
#include <array>
#include <string_view>

namespace NaWServer::http::wire
{

	// Incremental synchronous output. Metadata and body are borrowed and immutable
	// until complete()/reset(); a current span is valid only until advance/reset.
	// This class performs no I/O and owns only its small status/length prefix.
	class ResponseWriter
	{
		enum class Phase
		{
			Complete, Prefix, ContentType, TypeLineEnd, HeaderName,
			HeaderSeparator, HeaderValue, HeaderLineEnd, HeadersEnd, Body
		};

		std::array<char, 128> prefix_{};
		std::size_t prefix_size_ = 0;
		const char *content_type_ = nullptr;
		const Header *headers_ = nullptr;
		std::size_t header_count_ = 0;
		std::size_t header_index_ = 0;
		ByteView body_;
		ByteView span_;
		std::size_t offset_ = 0;
		Phase phase_ = Phase::Complete;

		bool appendPrefix(std::string_view bytes);
		bool appendNumber(std::size_t number);
		void nextSpan();
		void selectHeader();
		SendResult prepareImpl(HttpMethod method, int status, const char *type,
		    const Header *headers, std::size_t count, ByteView body, std::size_t length, bool keep_alive);

	public:
		ResponseWriter() = default;
		ResponseWriter(const ResponseWriter &) = delete;
		ResponseWriter &operator=(const ResponseWriter &) = delete;
		ResponseWriter(ResponseWriter &&) = delete;
		ResponseWriter &operator=(ResponseWriter &&) = delete;

		// Validate the entire response before exposing its first byte. Connection:
		// close from typed Response options is accepted once and emitted by prefix.
		// Other framing headers and duplicate supplied fields are invalid.
		SendResult prepare(HttpMethod method, int status, const char *content_type,
		    const Header *headers, std::size_t count, ByteView body, bool keep_alive = false);
		// Framing only, for a body supplied incrementally by the owner.
		SendResult prepareHead(HttpMethod method, int status, const char *type,
		    const Header *headers, std::size_t count, std::size_t length, bool keep_alive = false);
		ByteView current() const;
		bool advance(std::size_t bytes);
		bool complete() const;
		bool bodySpan() const
		{
			return phase_ == Phase::Body;
		}

		// Abandon borrowed data only after the owner ends a failed transport step.
		void reset();
	};

} // namespace NaWServer::http::wire
