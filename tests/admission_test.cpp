// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/admission.h>
#include <NaWServer/request_resource.h>
#include <cstdio>
#include <cstdlib>

namespace
{

	void check(bool condition, int line)
	{
		if(!condition)
		{
			std::printf("ADMISSION_FAIL line=%d\n", line);
			std::abort();
		}
	}

#define CHECK(value) check(static_cast<bool>(value), __LINE__)
	using namespace NaWServer;
	constexpr auto http = AdmissionResource::Http;

	void reservedAndSticky()
	{
		AdmissionOptions options;
		options.reserved_key = "primary";
		options.shared_clients = 2;
		options.idle_timeout_ms = 100;
		Admission<2> admission(options);
		CHECK(admission.valid());
		const auto first = admission.acquire("guest-1", http, 0);
		const auto second = admission.acquire("guest-2", http, 0);
		CHECK(first.ticket && second.ticket);
		CHECK(admission.acquire("extra", http, 0).decision == AdmissionDecision::Capacity);
		const auto primary = admission.acquire("primary", http, 0);
		CHECK(primary.ticket && primary.ticket.client == 0);
		for(unsigned index = 0; index < 10000; ++index)
			CHECK(admission.acquire("extra", http, index).decision == AdmissionDecision::Capacity);
		CHECK(admission.release(first.ticket, 10000));
		CHECK(!admission.release(first.ticket, 10000));
		CHECK(admission.acquire("extra", http, 10099).decision == AdmissionDecision::Capacity);
		const auto replacement = admission.acquire("extra", http, 10100);
		CHECK(replacement.ticket && replacement.ticket.client == first.ticket.client);
		CHECK(!admission.release(first.ticket, 10101));
		CHECK(admission.acquire("extra", http, 10101).decision == AdmissionDecision::Capacity);
		CHECK(admission.release(replacement.ticket, 10102));
		CHECK(admission.release(primary.ticket, 10102));
		CHECK(admission.release(second.ticket, 10102));
	}

	void rateAndIndependentResources()
	{
		AdmissionOptions options;
		options.reserved_key = "primary";
		options.shared.http = 2;
		options.shared.requests_per_window = 3;
		Admission<1> admission(options);
		AdmissionRef reference(admission);
		const auto first = reference.acquire("guest", http, 0);
		const auto second = reference.acquire("guest", http, 0);
		CHECK(first.ticket && second.ticket);
		CHECK(reference.acquire("guest", http, 0).decision == AdmissionDecision::Capacity);
		CHECK(reference.release(first.ticket, 1));
		CHECK(!reference.release(first.ticket, 1));
		const auto stream = reference.acquire("guest", AdmissionResource::WebSocket, 2);
		CHECK(stream.ticket);
		const auto rate = reference.acquire("guest", AdmissionResource::EventStream, 3);
		CHECK(rate.decision == AdmissionDecision::Rate && rate.retry_after_ms == 997);
		const auto primary = reference.acquire("primary", http, 3);
		CHECK(primary.ticket);
		CHECK(reference.release(second.ticket, 4));
		const auto next = reference.acquire("guest", http, 1000);
		CHECK(next.ticket);
		CHECK(reference.release(stream.ticket, 1001));
		CHECK(reference.release(next.ticket, 1001));
		CHECK(reference.release(primary.ticket, 1001));
		CHECK(!reference.release({}, 1002));
	}

	void invalidConfiguration()
	{
		AdmissionOptions options;
		options.shared_clients = 2;
		CHECK(!Admission<1>(options).valid());
		options.shared_clients = 1;
		options.shared.http = NAWSERVER_ADMISSION_MAX_ACTIVE;
		CHECK(!Admission<1>(options).valid());
		options.shared.http = 1;
		Admission<1> admission(options);
		char key[NAWSERVER_ADMISSION_KEY_BYTES + 1]{};
		CHECK(admission.acquire({key, sizeof(key)}, http, 0).decision == AdmissionDecision::Invalid);
		CHECK(admission.acquire({}, static_cast<AdmissionResource>(255), 0).decision == AdmissionDecision::Invalid);
	}

	void multipleProtectedKeysAndUpgrade()
	{
		const AdmissionOptions::ReservedProfile profiles[]{
			{"automation-a", {1, 0, 0}}, {"automation-b", {1, 0, 0}},
			{"automation-c", {1, 0, 0}}, {"automation-d", {1, 0, 0}}
		};
		AdmissionOptions options;
		options.reserved_profiles = profiles;
		options.reserved_profile_count = 4;
		options.shared_clients = 2;
		Admission<2> policy(options);
		AdmissionRef ref(policy);
		CHECK(policy.valid());
		const auto guest_a = ref.acquire("page-a", http, 0);
		const auto guest_b = ref.acquire("page-b", http, 0);
		CHECK(guest_a.ticket && guest_b.ticket);
		CHECK(ref.acquire("page-c", http, 0).decision == AdmissionDecision::Capacity);
		std::array<AdmissionTicket, 4> reserved;
		for(std::size_t index = 0; index < 4; ++index)
		{
			reserved[index] = ref.acquire(profiles[index].key, http, 0).ticket;
			CHECK(reserved[index] && ref.reserved(reserved[index]));
			CHECK(ref.acquire(profiles[index].key, http, 0).decision == AdmissionDecision::Capacity);
		}
		CHECK(!ref.reserved(guest_a.ticket));
		CHECK(ref.stats().active_reserved == 4 && ref.stats().active[0] == 6);
		CHECK(!ref.transfer(reserved[0], AdmissionResource::WebSocket, 1));
		CHECK(reserved[0].resource == AdmissionResource::Http);
		auto stream = guest_a.ticket;
		CHECK(ref.transfer(stream, AdmissionResource::WebSocket, 1));
		CHECK(ref.stats().active[0] == 5 && ref.stats().active[1] == 1);
		CHECK(!ref.release(guest_a.ticket, 2));
		auto next_http = ref.acquire("page-a", http, 2).ticket;
		CHECK(next_http);
		CHECK(!ref.transfer(next_http, AdmissionResource::WebSocket, 2));
		CHECK(ref.release(stream, 3));
		CHECK(ref.transfer(next_http, AdmissionResource::WebSocket, 3));
		CHECK(ref.release(next_http, 4));
		CHECK(ref.release(guest_b.ticket, 4));
		for(auto ticket : reserved) CHECK(ref.release(ticket, 4));
		CHECK(ref.stats().active[0] == 0 && ref.stats().active[1] == 0);
		CHECK(ref.stats().active_reserved == 0);
		CHECK(ref.stats().accepted == 7);
	}

	void invalidProtectedProfiles()
	{
		AdmissionOptions::ReservedProfile profiles[]{ {"same", {}}, {"same", {}} };
		AdmissionOptions options;
		options.reserved_profiles = profiles;
		options.reserved_profile_count = 2;
		CHECK(!Admission<1>(options).valid());
		profiles[1].key = "other";
		CHECK(Admission<1>(options).valid());
		options.reserved_key = "ambiguous";
		CHECK(!Admission<1>(options).valid());
		options.reserved_key = {};
		options.reserved_profiles = nullptr;
		CHECK(!Admission<1>(options).valid());
		options.reserved_profile_count = NAWSERVER_ADMISSION_MAX_RESERVED_KEYS + 1;
		CHECK(!Admission<1>(options).valid());
	}

	void leaseOwnership()
	{
		unsigned released = 0;
		auto release = [](void *context, const RequestResourceLease::Token &token)
		{
			CHECK(token[0] == 42);
			++*static_cast<unsigned *>(context);
		};
		{
			RequestResourceLease original(&released, {42, 0, 0, 0}, release, nullptr, true);
			RequestResourceLease moved(std::move(original));
			CHECK(!original && moved && moved.reserved());
			CHECK(!moved.reclassify(RequestResource::WebSocket));
			CHECK(moved.resource() == RequestResource::Http);
			RequestResourceLease final;
			final = std::move(moved);
			CHECK(!moved && final);
			final.reset();
			final.reset();
		}
		CHECK(released == 1);
	}

}

int main()
{
	reservedAndSticky();
	rateAndIndependentResources();
	invalidConfiguration();
	multipleProtectedKeysAndUpgrade();
	invalidProtectedProfiles();
	leaseOwnership();
	std::puts("ADMISSION_PASS");
}
