// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/request.h>
#include <NaWServer/response.h>
#include <NaWServer/handler_static.h>
#include <cstring>
namespace NaWServer
{
	HandlerStaticUriText::HandlerStaticUriText(std::string url)
	  : HandlerStaticUriText(std::move(url), "oK")
	{
	}

	HandlerStaticUriText::HandlerStaticUriText(std::string url, const char *text)
	  : HandlerStaticUriText(std::move(url), text, text ? std::strlen(text) : 0)
	{
	}

	HandlerStaticUriText::HandlerStaticUriText(
		std::string url,
		const char *text,
		std::ptrdiff_t size,
		const char *content_type)
	  : HandlerStaticUrl(std::move(url)), _text(text), _text_len(size), content_type_(content_type)
	{
	}

	HandlerStaticUriText::HandlerStaticUriText(IHandlerContainer &container, std::string url)
	  : HandlerStaticUriText(container, std::move(url), "oK")
	{
	}

	HandlerStaticUriText::HandlerStaticUriText(IHandlerContainer &container, std::string url, const char *text)
	  : HandlerStaticUriText(container, std::move(url), text, text ? std::strlen(text) : 0)
	{
	}

	HandlerStaticUriText::HandlerStaticUriText(
		IHandlerContainer &container,
		std::string url,
		const char *text,
		std::ptrdiff_t size,
		const char *content_type)
	  : HandlerStaticUrl(container, std::move(url)), _text(text), _text_len(size), content_type_(content_type)
	{
	}

	void HandlerStaticUriText::handleRequest(Request *, Response &response)
	{
		if(_text_len < 0)
		{
			response.fail();
			return;
		}

		(void)response.send(
			200,
			content_type_,
			{reinterpret_cast<const uint8_t *>(_text), static_cast<std::size_t>(_text_len)});
	}

	HandlerStaticUriBin::HandlerStaticUriBin(
		std::string url,
		const uint8_t *data,
		std::ptrdiff_t size,
		const char *content_type,
		bool gzip)
	  : HandlerStaticUrl(std::move(url)), _buf(data), _buf_len(size), _is_gzip(gzip), content_type_(content_type)
	{
	}

	HandlerStaticUriBin::HandlerStaticUriBin(
		IHandlerContainer &container,
		std::string url,
		const uint8_t *data,
		std::ptrdiff_t size,
		const char *content_type,
		bool gzip)
	  : HandlerStaticUrl(container, std::move(url)),
	    _buf(data),
	    _buf_len(size),
	    _is_gzip(gzip),
	    content_type_(content_type)
	{
	}

	void HandlerStaticUriBin::handleRequest(Request *, Response &response)
	{
		if(_buf_len < 0)
		{
			response.fail();
			return;
		}

		if(_is_gzip && response.header("Content-Encoding", "gzip") != SendResult::Ok)
			return;

		(void)response.send(200, content_type_, {_buf, static_cast<std::size_t>(_buf_len)});
	}

} // namespace NaWServer
