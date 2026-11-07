// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/http_backend.h>
#include <NaWServer/header_request.h>
#include <NaWServer/pipeline.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <NaWServer/request_body.h>
#include <NaWServer/form_parameters.h>
namespace NaWServer
{
	class HandlerBase;
	enum class ParameterSource
	{
		None,
		Query,
		Form
	};
	// Read side of one HTTP exchange. Borrowed views never escape the callback.
	class Request : public HeaderRequest
	{
		friend ExchangeDisposition dispatchHttp(HttpExchange &, HandlerBase *const *, std::size_t,
		    DefaultHeaders, ResponseOptions, const HttpLimits &,
		    const std::function<void(Request &, Response &)> &, HeaderPipeline);
		RequestBody body_;
		ParameterMap query_params_, form_params_;
		bool parameters_prepared_{};
		ParameterParseResult parameter_result_{};
		ParameterSource parameter_source_{ParameterSource::None};
		void setBody(std::vector<uint8_t> body)
		{
			body_.assign(std::move(body));
		}

		void borrowBody(ByteView body)
		{
			body_.borrow(body);
		}

		bool prepareParameters();
		static std::optional<std::string_view> find(const ParameterMap &, const std::string &);

	public:
		explicit Request(HttpExchange &exchange)
		  : HeaderRequest(exchange)
		{
		}

		Request(const Request &) = delete;
		Request &operator=(const Request &) = delete;
		HttpMethod method() const
		{
			return exchange_.head().method;
		}

		std::string_view path() const;
		std::string_view rawQuery() const;
		std::size_t contentLength() const
		{
			return exchange_.head().content_length;
		}

		void *transportExtension(const void *key) const
		{
			return exchange_.extension(key);
		}

		[[nodiscard]] HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const;
		std::optional<std::string> header(const char *name) const;
		bool hasHeader(const char *name) const
		{
			return readHeader(name, nullptr, 0).status == HeaderReadStatus::BufferTooSmall;
		}

		std::string host() const
		{
			return header("Host").value_or("");
		}

		ByteView body() const
		{
			return body_.view();
		}

		BodyMode bodyMode() const
		{
			const auto *state = requestState();
			return state ? state->body.mode : BodyMode::Buffered;
		}

		[[nodiscard]] BodyCopyResult copyBodyTo(uint8_t *destination, std::size_t capacity) const
		{
			return body_.copyTo(destination, capacity);
		}

		template <std::size_t N> [[nodiscard]] BodyCopyResult copyBodyTo(uint8_t (&destination)[N]) const
		{
			return body_.copyTo(destination);
		}

		template <std::size_t N> [[nodiscard]] BodyCopyResult copyBodyTo(std::array<uint8_t, N> &destination) const
		{
			return body_.copyTo(destination);
		}

		[[nodiscard]] BodyCopyResult copyBodyTo(std::vector<uint8_t> &destination) const
		{
			return body_.copyTo(destination);
		}

		std::optional<std::string_view> query(const std::string &name) const
		{
			return find(query_params_, name);
		}

		std::optional<std::string_view> form(const std::string &name) const
		{
			return find(form_params_, name);
		}

		// Explicit combined lookup preserves form-over-query behavior of the sample.
		std::optional<std::string_view> parameter(const std::string &name) const
		{
			auto value = form(name);
			return value ? value : query(name);
		}

		ParameterParseResult parameterParseResult() const
		{
			return parameter_result_;
		}

		ParameterSource parameterErrorSource() const
		{
			return parameter_source_;
		}

	};
} // namespace NaWServer
