// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/detail/sse/handoff_types.hpp>
#include <NaWServer/response.h>

namespace NaWServer::sse
{
	// Build a complete owned HTTP preface. Strings are never retained by pointer.
	// Reject unsupported framing/compression; preserve resolved application headers.
	// Input headers/strings must not overlap out; rejection leaves out unchanged.
	SendResult encode_headers(const Header *headers, std::size_t count, Frame &out);
	struct OpenResult
	{
		SendResult result = SendResult::TransportError;
		Admit admission = Admit::InvalidRequest;
		Token token;
		LastEventId last_id;
		bool active = false;
	};
	// Template injects only the SSE backend, not response semantics. EventSource
	// uses Request + Backend; focused bridge checks can inject a controlled backend.
	// Call only after the shared pipeline's auth/policy checks, from its endpoint.
	template <class RequestType, class Adapter>
	OpenResult open_stream(RequestType &request, Response &response, Adapter &adapter)
	{
		OpenResult out;
		std::array<Header, Response::max_headers> headers{};
		std::size_t count = 0;
		out.result = response.resolveHeaders(headers, count);
		if(out.result != SendResult::Ok)
			return out;

		Frame preface;
		out.result = encode_headers(headers.data(), count, preface);
		if(out.result != SendResult::Ok)
			return out;

		typename Adapter::Reservation reservation;
		out.admission = adapter.reserve(request, preface, reservation);
		out.last_id = reservation.last_id;
		if(out.admission != Admit::Owned)
		{
			out.result = (out.admission == Admit::InvalidLastId || out.admission == Admit::InvalidRequest)
			                 ? SendResult::InvalidArgument
			                 : SendResult::TransportError;
			return out;
		}

		out.token = reservation.token();
		const auto control = reservation.abort_handle();
		out.result = response.claimExternal({control.context, control.abort});
		if(out.result != SendResult::Ok)
			return out; // scoped rollback, no worker bytes

		// No fallible allocation or borrowed callback is introduced after claim.
		out.active = adapter.activate(reservation);
		if(!out.active)
		{
			response.fail();
			out.result = SendResult::TransportError;
		}

		return out;
	}

} // namespace NaWServer::sse
