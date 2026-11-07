// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/http_backend.h>
#include <optional>
#include <string>
namespace NaWServer
{
	// Header-stage request view deliberately has no body-reading API. The exchange
	// remains owned by the backend; this view may not escape a middleware call.
	class HeaderRequest
	{
	protected:
		HttpExchange &exchange_;
	public:
		explicit HeaderRequest(HttpExchange &exchange)
		  : exchange_(exchange)
		{
		}

		HttpMethod method() const
		{
			return exchange_.head().method;
		}

		std::size_t contentLength() const
		{
			return exchange_.head().content_length;
		}

		std::string_view path() const
		{
			const auto target = exchange_.head().target;
			return target.substr(0, target.find('?'));
		}

		std::string_view rawQuery() const
		{
			const auto target = exchange_.head().target;
			const auto query = target.find('?');
			return query == std::string_view::npos ? std::string_view{} : target.substr(query + 1);
		}

		HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const
		{
			return exchange_.readHeader(name, destination, capacity);
		}

		std::optional<std::string> header(const char *name) const
		{
			const auto size = readHeader(name, nullptr, 0);
			if(size.status != HeaderReadStatus::BufferTooSmall && size.status != HeaderReadStatus::Ok) return {};

			std::string value(size.size + 1, '\0');
			const auto read = readHeader(name, value.data(), value.size());
			if(read.status != HeaderReadStatus::Ok) return {};

			value.resize(read.size);
			return value;
		}

		void *transportExtension(const void *key) const
		{
			return exchange_.extension(key);
		}

		std::optional<std::uint32_t> peerAddress() const
		{
			return exchange_.peerAddress();
		}

		HttpRequestState *requestState() const
		{
			return exchange_.requestState();
		}

	};
}
