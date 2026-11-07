// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/sse.h>
#include <NaWServer/request.h>

namespace NaWServer::sse
{
	OpenResult EventSource::open(Request &request, Response &response)
	{
		return open_stream(request, response, backend_);
	}

} // namespace NaWServer::sse
