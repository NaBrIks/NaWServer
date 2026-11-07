// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/detail/sse/session.hpp>
#include <cstdio>
#include <cstdlib>

namespace
{

	unsigned checks = 0;
	void check(bool value, int line)
	{
		++checks;
		if(!value) { std::printf("SSE_DIRECT_FAIL line=%d\n", line); std::exit(1); }
	}
#define CHECK(value) check((value), __LINE__)

}

int main()
{
	using namespace NaWServer::sse;
	Limits limits;
	limits.clients = limits.initial_clients = 2;
	limits.heartbeat_ms = 20;
	limits.stall_ms = 10;
	limits.transfer_ms = 30;
	Sessions sessions(limits);
	CHECK(sessions.prepareStorage(true));
	CHECK(sessions.bufferMemoryStats().used_bytes == 0);
	CHECK(sessions.managedMemoryStats().allocated_bytes == 0);
	const auto token = sessions.reserveDirect(7, 10, 0);
	CHECK(sessions.pollDelay(0) == UINT32_MAX);
	CHECK(token.slot < client_capacity && sessions.direct(token));
	CHECK(sessions.next(token.slot, 0).kind == StepKind::None);
	CHECK(sessions.activate(token, 0));
	CHECK(sessions.pollDelay(0) == 1);
	sessions.directProgress(token, 0, false, 1);
	CHECK(sessions.pollDelay(1) == 20 && sessions.pollDelay(20) == 1);
	CHECK(sessions.sent_frames() == 0 && !sessions.directHeartbeat(token, 20));
	CHECK(sessions.directHeartbeat(token, 21));
	sessions.directQueued(token, 8, false, 21);
	sessions.directProgress(token, 0, false, 22);
	CHECK(sessions.sent_frames() == 0 && !sessions.directHeartbeat(token, 22));
	CHECK(sessions.directReadiness(token, 40) == Publish::Queued);
	sessions.directQueued(token, 40, true, 23);
	CHECK(sessions.pollDelay(23) == 1);
	sessions.directQueued(token, 50, true, 23);
	sessions.directProgress(token, 75, false, 24);
	CHECK(sessions.sent_frames() == 0 && sessions.queued_bytes() == 90);
	sessions.directProgress(token, 50, false, 25);
	CHECK(sessions.sent_frames() == 1 && sessions.queued_bytes() == 50);
	sessions.directProgress(token, 0, false, 26);
	CHECK(sessions.sent_frames() == 2 && sessions.queued_bytes() == 0);
	CHECK(!sessions.directHeartbeat(token, 45) && sessions.directHeartbeat(token, 46));
	sessions.close(token);
	auto close = sessions.next(token.slot, 27);
	CHECK(close.kind == StepKind::Close && sessions.completed(close));
	CHECK(sessions.directReadiness(token, 10) == Publish::Closed);
	CHECK(sessions.active() == 0 && sessions.bufferMemoryStats().used_bytes == 0);
	CHECK(sessions.pollDelay(27) == UINT32_MAX);
	const auto stalled = sessions.reserveDirect(7, 10, 30);
	CHECK(stalled.generation != token.generation && sessions.activate(stalled, 30));
	sessions.directProgress(stalled, 10, false, 35);
	close = sessions.next(stalled.slot, 40);
	CHECK(close.kind == StepKind::Close && sessions.reason(stalled) == CloseReason::Stall);
	CHECK(sessions.completed(close));
	const auto failed = sessions.reserveDirect(7, 10, 50);
	CHECK(sessions.activate(failed, 50));
	sessions.directProgress(failed, 0, true, 51);
	CHECK(sessions.sent_frames() == 2 && sessions.reason(failed) == CloseReason::Disconnected);
	close = sessions.next(failed.slot, 51);
	CHECK(sessions.completed(close));
	const auto rollback = sessions.reserveDirect(7, 10, 60);
	CHECK(sessions.rollback(rollback) && sessions.active() == 0);
	const auto heartbeat = sessions.reserveDirect(7, 10, 70);
	CHECK(sessions.activate(heartbeat, 70));
	sessions.directProgress(heartbeat, 0, false, 71);
	CHECK(sessions.directHeartbeat(heartbeat, 91));
	sessions.directHeartbeatBlocked(heartbeat, 91);
	sessions.directHeartbeatBlocked(heartbeat, 101);
	CHECK(sessions.reason(heartbeat) == CloseReason::Stall);
	close = sessions.next(heartbeat.slot, 101);
	CHECK(sessions.completed(close));
	sessions.shutdown();
	CHECK(sessions.restart());
	sessions.releaseStorage();
	CHECK(sessions.bufferMemoryStats().used_bytes == 0);
	Limits bounded;
	bounded.clients = 2; bounded.pending = 1;
	bounded.bytes_per_client = 50; bounded.total_bytes = 60;
	Sessions quotas(bounded);
	CHECK(quotas.prepareStorage(true));
	const auto first = quotas.reserveDirect(1, 10, 0);
	const auto second = quotas.reserveDirect(2, 10, 0);
	CHECK(quotas.activate(first, 0) && quotas.activate(second, 0));
	quotas.directProgress(first, 0, false, 1);
	quotas.directProgress(second, 0, false, 1);
	quotas.directQueued(first, 40, true, 2);
	CHECK(quotas.directReadiness(second, 30) == Publish::GlobalFull);
	CHECK(quotas.reason(second) == CloseReason::None);
	CHECK(quotas.directReadiness(first, 1) == Publish::IndividualOverflow);
	CHECK(quotas.reason(first) == CloseReason::Overflow);
	quotas.shutdown();
	CHECK(quotas.completed(quotas.next(first.slot, 3)));
	CHECK(quotas.completed(quotas.next(second.slot, 3)));
	CHECK(quotas.queued_bytes() == 0 && quotas.active() == 0);
	std::printf("SSE_DIRECT_PASS checks=%u\n", checks);
}
