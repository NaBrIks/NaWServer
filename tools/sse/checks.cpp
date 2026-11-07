// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "checks.hpp"
#include <NaWServer/detail/sse/session.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace NaWServer::sse
{
	namespace
	{
		std::size_t count;
#define CHECK(expression)                                                                                              \
		do                                                                                                                 \
		{                                                                                                                  \
			++count;                                                                                                       \
			if(!(expression))                                                                                              \
			{                                                                                                              \
				std::printf("SSE_FAIL line=%d expr=%s\n", __LINE__, #expression);                                          \
				std::abort();                                                                                              \
			}                                                                                                              \
		} while(false)
		std::string text(const Frame &f)
		{
			return {f.bytes.data(), f.size};
		}

		Frame make_frame(std::string_view value)
		{
			Frame f;
			CHECK(encode({value, {}, {}, {}}, f) == EncodeResult::Ok);
			return f;
		}

		// Receiver oracle: parse field lines independently; preserves NUL by length.
		std::string received(const Frame &f)
		{
			auto wire = text(f);
			std::string result;
			for(std::size_t start = 0; start < wire.size();)
			{
				auto end = wire.find('\n', start);
				CHECK(end != std::string::npos);
				auto line = wire.substr(start, end - start);
				auto colon = line.find(':');
				if(line.substr(0, colon) == "data")
				{
					auto value = colon == std::string::npos ? std::string{} : line.substr(colon + 1);
					if(!value.empty() && value.front() == ' ')
						value.erase(0, 1);

					result += value;
					result += '\n';
				}

				start = end + 1;
			}
			CHECK(!result.empty());
			result.pop_back();
			return result;
		}

		void drain_header(Sessions &s, Token t, std::uint64_t now = 0)
		{
			auto w = s.next(t.slot, now);
			CHECK(w.kind == StepKind::Write);
			CHECK(std::string(w.bytes.data(), w.size).find("200 OK") != std::string::npos);
			CHECK(s.written(w, static_cast<int>(w.size), now));
		}

		void complete(Sessions &s, Token t, std::uint64_t now = 0)
		{
			auto c = s.next(t.slot, now);
			CHECK(c.kind == StepKind::Close);
			CHECK(s.completed(c));
			CHECK(!s.completed(c));
		}

		void encoder_checks()
		{
			Frame f = make_frame("");
			CHECK(text(f) == "data: \n\n");
			CHECK(received(f).empty());
			f = make_frame("one\r\ntwo\rthree\n");
			CHECK(text(f) == "data: one\ndata: two\ndata: three\ndata: \n\n");
			CHECK(received(f) == "one\ntwo\nthree\n");
			f = make_frame("  text\n:injected\nevent: fake");
			CHECK(received(f) == "  text\n:injected\nevent: fake");
			CHECK(encode({"x", "status", "boot:42", 0}, f) == EncodeResult::Ok);
			CHECK(text(f) == "event: status\nid: boot:42\nretry: 0\ndata: x\n\n");
			CHECK(encode({"", "", "", 4294967295u}, f) == EncodeResult::Ok);
			CHECK(text(f) == "event: \nid: \nretry: 4294967295\ndata: \n\n");
			CHECK(encode({"x", {}, {}, {}}, f) == EncodeResult::Ok);
			CHECK(text(f).find("id:") == std::string::npos);
			for(auto bad : {std::string("x\ny"), std::string("x\ry"), std::string("x\0y", 3)})
			{
				auto before = f;
				CHECK(encode({"x", bad, {}, {}}, f) == EncodeResult::InvalidField);
				CHECK(encode({"x", {}, bad, {}}, f) == EncodeResult::InvalidField);
				CHECK(f.bytes == before.bytes && f.size == before.size);
			}
			for(auto bad :
			    {std::string("\xc0\x80", 2),
			     std::string("\xed\xa0\x80", 3),
			     std::string("\xf4\x90\x80\x80", 4),
			     std::string("\xe2\x82", 2),
			     std::string("\x80", 1),
			     std::string("\xff", 1),
			     std::string("\xe0\x80\x80", 3)})
			{
				CHECK(!valid_utf8(bad));
				CHECK(encode({bad, {}, {}, {}}, f) == EncodeResult::InvalidUtf8);
			}
			f = make_frame(std::string("a\0b", 3));
			CHECK(received(f) == std::string("a\0b", 3));
			f = make_frame(u8"Русский 中文 日本語 🙂 👩‍🔬");
			CHECK(received(f) == u8"Русский 中文 日本語 🙂 👩‍🔬");
			CHECK(comment("idle\r\nnext\n", f) == EncodeResult::Ok);
			CHECK(text(f) == ": idle\n: next\n: \n\n");
			CHECK(comment("", f) == EncodeResult::Ok);
			CHECK(text(f) == ": \n\n");
			f = make_frame(std::string(frame_capacity - 8, 'a'));
			CHECK(f.size == frame_capacity);
			auto before = f;
			CHECK(encode({std::string(frame_capacity - 7, 'a'), {}, {}, {}}, f) == EncodeResult::TooLarge);
			CHECK(encode({std::string(200, '\n'), {}, {}, {}}, f) == EncodeResult::TooLarge);
			CHECK(comment(std::string(frame_capacity, 'a'), f) == EncodeResult::TooLarge);
			CHECK(f.bytes == before.bytes && f.size == before.size);
			// Deterministic combinations: empty segments, CR, LF, CRLF, BOM, NUL, Unicode.
			const std::vector<std::string> atoms =
			{"", "x", " ", "\n", "\r", "\r\n", u8"🙂", u8"Ж", std::string("\0", 1), "\xef\xbb\xbf"};
			for(const auto &a : atoms)
				for(const auto &b : atoms)
					for(const auto &c : atoms)
					{
						const auto original = a + b + c;
						std::string expected;
						for(std::size_t i = 0; i < original.size(); ++i)
						{
							if(original[i] == '\r')
							{
								expected += '\n';
								if(i + 1 < original.size() && original[i + 1] == '\n')
									++i;
							}
							else
								expected += original[i];
						}
						CHECK(received(make_frame(original)) == expected);
					}
		}

		void lifecycle_checks()
		{
			const auto f = make_frame("payload");
			{
				Sessions s;
				auto header = default_header();
				auto t = s.reserve(77, header, 0);
				CHECK(t.slot == 0);
				header.bytes.fill('x');
				for(unsigned i = 0; i < 100; ++i)
					CHECK(s.next(t.slot, i).kind == StepKind::None);
				CHECK(s.publish(t, f) == Publish::Closed);
				CHECK(s.activate(t, 100));
				CHECK(!s.activate(t, 100));
				CHECK(!s.rollback(t));
				drain_header(s, t, 100);
				s.close(t);
				complete(s, t, 101);
			}
			{
				Sessions s;
				auto t = s.reserve(77, default_header(), 0);
				s.shutdown();
				CHECK(s.next(t.slot, 999).kind == StepKind::None); // original callback still settling
				CHECK(s.rollback(t));
				CHECK(!s.rollback(t));
				CHECK(s.active() == 0);
			}
			{
				Sessions s;
				auto t = s.reserve(77, default_header(), 0);
				s.shutdown();
				CHECK(!s.activate(t, 10)); // externally claimed during concurrent stop: close, never send
				complete(s, t, 10);
				CHECK(s.active() == 0);
			}
			{
				Sessions s;
				auto t = s.reserve(77, default_header(), 0);
				CHECK(s.rollback(t));
				auto fresh = s.reserve(77, default_header(), 0);
				CHECK(!(t == fresh));
				CHECK(!s.activate(t, 0));
				CHECK(s.rollback(fresh));
				CHECK(s.active() == 0);
			}
			{
				Sessions s;
				std::array<Token, 4> t;
				for(std::size_t i = 0; i < t.size(); ++i)
				{
					t[i] = s.open(static_cast<int>(i + 10), 0);
					CHECK(t[i].slot == i);
				}
				CHECK(s.open(99, 0).slot == client_capacity);
				CHECK(s.open(10, 0).slot == client_capacity);
				CHECK(s.publish(t[0], f) == Publish::Queued);
				drain_header(s, t[0]);
				auto w = s.next(0, 1);
				CHECK(w.kind == StepKind::Write);
				CHECK(s.next(0, 1).kind == StepKind::None);
				CHECK(s.written(w, 1, 1));
				CHECK(!s.written(w, 1, 1));
				auto rest = s.next(0, 2);
				CHECK(rest.size == f.size - 1);
				CHECK(s.written(rest, static_cast<int>(rest.size), 2));
				CHECK(s.queued_bytes() == 0);
				CHECK(s.sent_frames() == 1);
				s.shutdown();
				CHECK(s.publish(t[1], f) == Publish::Closed);
				for(auto token : t)
					complete(s, token, 3);
				CHECK(s.active() == 0);
				CHECK(s.completions() == 4);
				CHECK(s.open(10, 4).slot == client_capacity);
			}
			{
				Sessions s;
				auto old = s.open(42, 0);
				auto in = s.next(old.slot, 0);
				s.close(old);
				CHECK(s.next(old.slot, 0).kind == StepKind::None); // no complete during active write
				CHECK(s.written(in, 0, 1));
				complete(s, old, 1);
				auto fresh = s.open(42, 2);
				CHECK(fresh.slot == old.slot && fresh.generation != old.generation);
				CHECK(!s.written(in, 1, 2));
				CHECK(s.publish(old, f) == Publish::Closed);
				CHECK(s.publish(fresh, f) == Publish::Queued);
				s.shutdown();
				complete(s, fresh, 2);
				CHECK(s.queued_bytes() == 0);
			}
			{
				Limits l;
				l.total_bytes = f.size;
				Sessions s(l);
				auto a = s.open(1, 0), b = s.open(2, 0);
				CHECK(s.publish(a, f) == Publish::Queued);
				CHECK(s.publish(b, f) == Publish::GlobalFull);
				CHECK(s.reason(b) == CloseReason::None && s.reason(a) == CloseReason::None);
				s.shutdown();
				complete(s, a);
				complete(s, b);
			}
			{
				Limits l;
				l.pending = 1;
				Sessions s(l);
				auto t = s.open(1, 0);
				CHECK(s.publish(t, f) == Publish::Queued);
				CHECK(s.publish(t, f) == Publish::IndividualOverflow);
				CHECK(s.reason(t) == CloseReason::Overflow);
				complete(s, t);
				CHECK(s.queued_bytes() == 0);
			}
			{
				Limits l;
				l.bytes_per_client = f.size - 1;
				Sessions s(l);
				auto t = s.open(1, 0);
				CHECK(s.publish(t, f) == Publish::IndividualOverflow);
				complete(s, t);
			}
			{
				Limits l;
				l.heartbeat_ms = 10;
				l.stall_ms = 5;
				l.transfer_ms = 20;
				Sessions s(l);
				auto t = s.open(1, 0);
				drain_header(s, t);
				CHECK(s.next(t.slot, 9).kind == StepKind::None);
				auto heartbeat = s.next(t.slot, 1000000);
				CHECK(heartbeat.kind == StepKind::Write);
				CHECK(std::string(heartbeat.bytes.data(), heartbeat.size) == ": heartbeat\n\n");
				CHECK(s.publish(t, f) == Publish::Queued); // arrival during heartbeat cannot be consumed as heartbeat
				CHECK(s.written(heartbeat, static_cast<int>(heartbeat.size), 1000000));
				CHECK(s.queued_bytes() == f.size);
				auto event = s.next(t.slot, 1000001);
				CHECK(s.written(event, static_cast<int>(event.size), 1000001));
				CHECK(s.sent_frames() == 1 && s.reason(t) == CloseReason::None);
				CHECK(s.publish(t, f) == Publish::Queued);
				auto blocked = s.next(t.slot, 1000002);
				CHECK(s.written(blocked, 0, 1000002));
				auto end = s.next(t.slot, 1000007);
				CHECK(end.kind == StepKind::Close && s.reason(t) == CloseReason::Stall);
				CHECK(s.completed(end));
			}
			{
				Limits l;
				l.stall_ms = 10;
				l.transfer_ms = 20;
				Sessions s(l);
				auto t = s.open(1, 0);
				drain_header(s, t);
				CHECK(s.publish(t, make_frame(std::string(100, 'x'))) == Publish::Queued);
				for(std::uint64_t time = 1; time <= 20; time += 3)
				{
					auto w = s.next(t.slot, time);
					CHECK(w.kind == StepKind::Write);
					CHECK(s.written(w, 1, time));
				}
				auto c = s.next(t.slot, 21);
				CHECK(c.kind == StepKind::Close);
				CHECK(s.reason(t) == CloseReason::Deadline);
				CHECK(s.completed(c));
			}
			{
				Sessions s;
				auto t = s.open(1, 0);
				auto w = s.next(t.slot, 0);
				CHECK(s.written(w, -1, 1));
				CHECK(s.reason(t) == CloseReason::Disconnected);
				complete(s, t, 1);
			}
			{
				Sessions s;
				auto t = s.open(1, 0);
				drain_header(s, t);
				auto large = make_frame(std::string(frame_capacity - 8, 'x'));
				CHECK(s.publish(t, large) == Publish::Queued);
				std::string wire;
				for(std::uint64_t i = 0; i < 30 && s.queued_bytes(); ++i)
				{
					auto step = s.next(t.slot, i);
					CHECK(step.kind == StepKind::Write && step.size <= step_capacity);
					const auto n = std::min<std::size_t>(step.size, 37);
					auto fake = step;
					++fake.serial;
					CHECK(!s.written(fake, 1, i));
					wire.append(step.bytes.data(), n);
					CHECK(s.written(step, static_cast<int>(n), i));
				}
				CHECK(wire == text(large));
				CHECK(s.sent_frames() == 1 && s.queued_bytes() == 0);
				s.close(t);
				complete(s, t);
			}
			{
				Sessions s;
				auto t = s.open(1, 0);
				auto step = s.next(t.slot, 0);
				CHECK(s.written(step, static_cast<int>(step.size + 1), 1));
				CHECK(s.reason(t) == CloseReason::Disconnected);
				complete(s, t, 1);
			}
			{
				Limits l;
				l.clients = 2;
				Sessions s(l);
				CHECK(s.open(1, 0).slot == 0);
				CHECK(s.open(2, 0).slot == 1);
				CHECK(s.open(3, 0).slot == client_capacity);
				l.clients = 5;
				Sessions invalid(l);
				CHECK(!invalid.valid_limits());
				CHECK(invalid.open(1, 0).slot == client_capacity);
			}
			// Repeated reconnect/close with active jobs must restore byte budget each time.
			Sessions s;
			for(int i = 0; i < 2000; ++i)
			{
				auto t = s.open(7, static_cast<std::uint64_t>(i));
				CHECK(t.slot == 0);
				CHECK(s.publish(t, f) == Publish::Queued);
				auto w = s.next(0, static_cast<std::uint64_t>(i));
				s.close(t);
				CHECK(s.written(w, static_cast<int>(w.size), static_cast<std::uint64_t>(i)));
				complete(s, t);
				CHECK(s.queued_bytes() == 0 && s.active() == 0);
			}
			CHECK(s.completions() == 2000);
		}

	} // namespace
	std::size_t checks()
	{
		count = 0;
		encoder_checks();
		lifecycle_checks();
		return count;
	}

} // namespace NaWServer::sse
