// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/http_backend.h>
#include <optional>

namespace NaWServer::http::wire
{

	struct HeaderEntry
	{
		std::size_t name_offset = 0;
		std::size_t name_size = 0;
		std::size_t value_offset = 0;
		std::size_t value_size = 0;
	};

	enum class ParseStatus { NeedMore, Complete, Reject, InvalidState };

	struct ParseResult
	{
		ParseStatus state = ParseStatus::NeedMore;
		int status = 0;
		std::size_t head_bytes = 0;
	};

	// Storage and index are borrowed and remain stationary through callback/send.
	// The caller receives directly into the storage tail and supplies its total
	// used length. advance is monotonic until reset; bytes beyond the first head
	// delimiter are untouched body/frame prefix, not additional header bytes.
	// Configuration must fit the supplied storage/index; invalid API use is an
	// InvalidState result, separate from malformed wire input (Reject/status).
	class RequestParser
	{
	public:
		RequestParser(char *storage, std::size_t storage_capacity,
		    HeaderEntry *entries, std::size_t entry_capacity, const HttpLimits &limits);

		RequestParser(const RequestParser &) = delete;
		RequestParser &operator=(const RequestParser &) = delete;

		ParseResult advance(std::size_t used);
		// End all borrowed views before resetting/reusing the caller's workspace.
		void reset();
		HttpRequestHead head() const;
		std::string_view rawHead() const;
		bool http11() const;
		std::optional<std::string_view> headerValue(std::string_view name) const;
		std::size_t headerCount(std::string_view name) const;
		std::size_t headerCount() const;
		HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const;

	private:
		char *storage_;
		std::size_t storage_capacity_;
		HeaderEntry *entries_;
		std::size_t entry_capacity_;
		HttpLimits limits_;
		std::size_t used_ = 0;
		std::size_t scanned_ = 0;
		std::size_t header_count_ = 0;
		HttpRequestHead head_{HttpMethod::Unknown, {}, 0};
		bool http11_ = false;
		ParseResult result_;

		bool validConfiguration() const;
		ParseResult reject(int status);
		ParseResult parseHead(std::size_t head_bytes);
		std::string_view name(const HeaderEntry &entry) const;
		std::string_view value(const HeaderEntry &entry) const;
	};

} // namespace NaWServer::http::wire
