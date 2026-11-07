// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/request_body.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

using namespace NaWServer;
static unsigned checks;
#define CHECK(expression)                                                                                              \
		do                                                                                                                 \
		{                                                                                                                  \
			++checks;                                                                                                      \
			if(!(expression))                                                                                              \
			{                                                                                                              \
				std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression);                                         \
				std::exit(1);                                                                                              \
			}                                                                                                              \
		} while(0)

int main()
{
	static_assert(std::is_same_v<decltype(ByteView{}.data()), const uint8_t *>);
	static_assert(!std::is_copy_constructible_v<RequestBody>);
	static_assert(std::is_nothrow_move_constructible_v<RequestBody>);
	static_assert(std::is_nothrow_move_assignable_v<RequestBody>);
	const std::vector<uint8_t> expected{0, 0xff, '&', '=', '+', 0, 0x80};
	RequestBody source(expected);
	const auto view = source.view();
	CHECK(view.size() == expected.size());
	CHECK(!view.empty());
	CHECK(source.view().data() == view.data());
	CHECK(std::memcmp(view.data(), expected.data(), expected.size()) == 0);

	uint8_t fixed[9];
	std::memset(fixed, 0x5a, sizeof(fixed));
	CHECK(source.copyTo(fixed, 6) == BodyCopyResult::BufferTooSmall);
	CHECK(fixed[0] == 0x5a && fixed[6] == 0x5a);
	CHECK(source.copyTo(nullptr, expected.size()) == BodyCopyResult::InvalidDestination);
	CHECK(source.copyTo(fixed, expected.size()) == BodyCopyResult::Ok);
	CHECK(std::memcmp(fixed, expected.data(), expected.size()) == 0);
	CHECK(fixed[7] == 0x5a && fixed[8] == 0x5a);
	CHECK(source.copyTo(fixed) == BodyCopyResult::Ok);

	std::array<uint8_t, 7> exact{};
	CHECK(source.copyTo(exact) == BodyCopyResult::Ok);
	CHECK(std::memcmp(exact.data(), expected.data(), expected.size()) == 0);
	std::array<uint8_t, 1> small{{0x5a}};
	CHECK(source.copyTo(small) == BodyCopyResult::BufferTooSmall);
	CHECK(small[0] == 0x5a);

	std::vector<uint8_t> reusable;
	reusable.reserve(32);
	const auto *allocation = reusable.data();
	const auto capacity = reusable.capacity();
	CHECK(source.copyTo(reusable) == BodyCopyResult::Ok);
	CHECK(reusable == expected);
	CHECK(reusable.capacity() == capacity && reusable.data() == allocation);
	std::vector<uint8_t> growing;
	CHECK(source.copyTo(growing) == BodyCopyResult::Ok);
	CHECK(growing == expected);

	RequestBody moved(std::move(source));
	CHECK(moved.view().data() == view.data());
	moved.assign({42});
	CHECK(reusable == expected); // Copy remains valid after source replacement.
	CHECK(moved.view().size() == 1 && moved.view().data()[0] == 42);

	RequestBody empty;
	CHECK(empty.view().empty());
	CHECK(empty.copyTo(nullptr, 0) == BodyCopyResult::Ok);
	std::array<uint8_t, 0> zero{};
	CHECK(empty.copyTo(zero) == BodyCopyResult::Ok);
	CHECK(empty.copyTo(reusable) == BodyCopyResult::Ok);
	CHECK(reusable.empty() && reusable.capacity() == capacity);

	std::vector<uint8_t> large(4096);
	for(std::size_t i = 0; i < large.size(); ++i)
		large[i] = static_cast<uint8_t>(i);
	RequestBody maximum(large);
	CHECK(maximum.copyTo(growing) == BodyCopyResult::Ok);
	CHECK(growing == large);

	uint8_t borrowed_bytes[]{0, 0xff, 3, 4};
	RequestBody borrowed;
	borrowed.borrow({borrowed_bytes, sizeof(borrowed_bytes)});
	CHECK(borrowed.view().data() == borrowed_bytes && borrowed.view().size() == sizeof(borrowed_bytes));
	CHECK(borrowed.copyTo(fixed, 3) == BodyCopyResult::BufferTooSmall);
	CHECK(borrowed.copyTo(nullptr, 4) == BodyCopyResult::InvalidDestination);
	CHECK(borrowed.copyTo(fixed) == BodyCopyResult::Ok && std::memcmp(fixed, borrowed_bytes, 4) == 0);
	CHECK(borrowed.copyTo(reusable) == BodyCopyResult::Ok && reusable.size() == 4 && reusable[1] == 0xff);
	RequestBody moved_borrowed(std::move(borrowed));
	CHECK(borrowed.view().empty() && moved_borrowed.view().data() == borrowed_bytes);
	borrowed = std::move(moved_borrowed);
	CHECK(moved_borrowed.view().empty() && borrowed.view().data() == borrowed_bytes);
	borrowed_bytes[2] = 55;
	CHECK(borrowed.view().data()[2] == 55);
	borrowed.assign({8, 9});
	CHECK(borrowed.view().size() == 2 && borrowed.view().data() != borrowed_bytes);
	borrowed.borrow({});
	CHECK(borrowed.view().empty() && borrowed.view().data() == nullptr);
	CHECK(borrowed.copyTo(nullptr, 0) == BodyCopyResult::Ok);
	CHECK(borrowed.copyTo(reusable) == BodyCopyResult::Ok && reusable.empty());
	std::printf("PASS: %u request body checks\n", checks);
}
