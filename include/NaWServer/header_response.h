// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <NaWServer/response.h>
#ifndef NAWSERVER_HEADER_REPLY_MAX_BYTES
#define NAWSERVER_HEADER_REPLY_MAX_BYTES 512
#endif
namespace NaWServer
{
	// Early middleware can issue a bounded refusal, but cannot start a file,
	// external stream or arbitrary producer before admission has completed.
	class HeaderResponse
	{
		Response &response_;
	public:
		HeaderResponse(Response &response)
		  : response_(response)
		{
		}

		SendResult header(std::string_view name, std::string_view value)
		{
			return response_.header(name, value);
		}

		SendResult send(int status, const char *type, ByteView body)
		{
			if(body.size() > NAWSERVER_HEADER_REPLY_MAX_BYTES) { response_.drop(); return SendResult::BufferTooSmall; }

			return response_.send(status, type, body);
		}

		SendResult text(int status, std::string_view body)
		{
			return send(status, "text/plain; charset=utf-8",
			           {reinterpret_cast<const std::uint8_t *>(body.data()), body.size()});
		}

		SendResult empty(int status = 204)
		{
			return response_.empty(status);
		}

		bool started() const
		{
			return response_.started();
		}

		bool failed() const
		{
			return response_.failed();
		}

		bool dropped() const
		{
			return response_.dropped();
		}

		void drop()
		{
			response_.drop();
		}

	};
}
