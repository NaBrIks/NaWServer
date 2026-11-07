// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <cstddef>
#include <NaWServer/handler.h>

namespace NaWServer
{

	/// @brief For static text
	class HandlerStaticUriText : public HandlerStaticUrl
	{
	private:
		const char *_text;
		const std::ptrdiff_t _text_len;
		const char *content_type_;

	protected:
		void handleRequest(Request *request, Response &response) override;

	public:
		HandlerStaticUriText(std::string url);
		HandlerStaticUriText(std::string url, const char *text);
		// Body and MIME are borrowed and must outlive the handler.
		HandlerStaticUriText(
			std::string url,
			const char *text,
			std::ptrdiff_t text_len,
			const char *content_type = "text/html");
		HandlerStaticUriText(IHandlerContainer &handlerContainer, std::string url);
		HandlerStaticUriText(IHandlerContainer &handlerContainer, std::string url, const char *text);
		HandlerStaticUriText(
			IHandlerContainer &handlerContainer,
			std::string url,
			const char *text,
			std::ptrdiff_t text_len,
			const char *content_type = "text/html");
	};

	/// @brief For binary and may be gziped
	class HandlerStaticUriBin : public HandlerStaticUrl
	{
	private:
		const uint8_t *_buf;
		const std::ptrdiff_t _buf_len;
		const bool _is_gzip{false};
		const char *content_type_;

	protected:
		void handleRequest(Request *request, Response &response) override;

	public:
		// MIME describes the original resource, independently of gzip encoding.
		// Body and MIME are borrowed and must outlive the handler.
		HandlerStaticUriBin(
			std::string url,
			const uint8_t *buf,
			std::ptrdiff_t buf_len,
			const char *content_type = "application/octet-stream",
			bool is_gzip = false);
		HandlerStaticUriBin(
			IHandlerContainer &handlerContainer,
			std::string url,
			const uint8_t *buf,
			std::ptrdiff_t buf_len,
			const char *content_type = "application/octet-stream",
			bool is_gzip = false);
	};

} // namespace NaWServer
