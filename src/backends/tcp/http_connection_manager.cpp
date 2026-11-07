// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "http_connection_manager.h"
#include "http/wire/response_writer.h"
#include "platform/worker.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include "detail/ascii.h"

namespace NaWServer::tcp::detail
{
#if NAWSERVER_TCP_MANAGED

	namespace
	{
		class HeaderExchange final : public HttpExchange
		{
			http::wire::RequestParser &parser_;
			HttpRequestState &state_;
			ResponseTransport response_;
			PeerAddress peer_;
		public:
			HeaderExchange(http::wire::RequestParser &parser, HttpRequestState &state,
			    ResponseTransport response, PeerAddress peer)
			  : parser_(parser), state_(state), response_(response), peer_(peer) {}
			HttpRequestHead head() const override { return parser_.head(); }
			HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
			{
				return parser_.readHeader(name, destination, capacity);
			}
			BodyReadResult readBody(std::uint8_t *, std::size_t) override
			{
				return {BodyReadStatus::TransportError, 0};
			}
			ResponseTransport responseTransport() override { return response_; }
			HttpRequestState *requestState() const override { return &state_; }
			std::optional<std::uint32_t> peerAddress() const override
			{
				return peer_.valid ? std::optional<std::uint32_t>(peer_.ipv4) : std::nullopt;
			}
		};
		std::uint64_t after(std::uint64_t now, std::uint32_t delay)
		{
			return now + std::min<std::uint64_t>(delay, std::numeric_limits<std::uint64_t>::max() - now);
		}

		std::string_view rejection(int status)
		{
			switch(status)
			{
			case 429: return
				    "HTTP/1.1 429 Too Many Requests\r\nConnection: close\r\nRetry-After: 1\r\nCache-Control: no-store\r\nContent-Length: 21\r\n\r\nRequest rate exceeded";
			case 400: return
				    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 16\r\n\r\nInvalid request\n";
			case 408: return
				    "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\nContent-Length: 16\r\n\r\nRequest timeout\n";
			case 431: return
				    "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\nContent-Length: 17\r\n\r\nHeaders too large";
			default: return
				    "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nRetry-After: 1\r\nCache-Control: no-store\r\nContent-Length: 18\r\n\r\nNo client capacity";
			}
		}

		bool hasClose(std::string_view value)
		{
			while(!value.empty())
			{
				const auto comma = value.find(',');
				auto token = value.substr(0, comma);
				while(!token.empty() && (token.front() == ' ' || token.front() == '\t'))
					token.remove_prefix(1);
				while(!token.empty() && (token.back() == ' ' || token.back() == '\t'))
					token.remove_suffix(1);
				if(NaWServer::detail::asciiEqualIgnoreCase(token, "close")) return true;

				if(comma == value.npos) break;

				value.remove_prefix(comma + 1);
			}
			return false;
		}

	}

	HttpConnectionManager::HttpConnectionManager(Transport &transport, ConnectionRegistry &registry, Pool &pool, TcpOptions options,
	    HttpLimits limits, HttpRequestHandler &handler, void *context,
	    ConnectionDisposition(*dispatch)(void *, Token, Parser &, ByteView, ResponseTransport, HttpRequestState &))
	  : transport_(transport), registry_(registry), options_(options), limits_(limits), handler_(handler), pool_(pool), context_(context),
	    dispatch_(dispatch)
	{
		const auto &configuration = options_.managed;
		if(!configuration.ingress_slots || configuration.header_bytes < 128 ||
		    configuration.header_bytes > limits.max_header_bytes ||
		    configuration.response_header_bytes < rejection(503).size() ||
		    !configuration.response_body_bytes || !configuration.request_body_block_bytes || !configuration.header_timeout_ms ||
		    !configuration.reject_timeout_ms || !configuration.io_slice_bytes || !dispatch) return;

		const auto client_count = std::max<std::size_t>(1, options.max_websocket_clients);
		const auto job_count = std::min<std::size_t>(options.http_memory.maximum_requests, limits.max_connections);
		const auto connection_count = job_count;
		const std::size_t reserve = options.http_memory.reserved_requests;
		const auto initial = std::min<std::size_t>(options.http_memory.initial_requests, job_count);
		const auto aligned_head = (configuration.header_bytes + alignof(http::wire::HeaderEntry) - 1) /
		    alignof(http::wire::HeaderEntry) * alignof(http::wire::HeaderEntry);
		const auto metadata = aligned_head + limits.max_headers * sizeof(http::wire::HeaderEntry) +
		    configuration.response_header_bytes;
		// A streamed body has a bounded block independent of its total route limit.
		// Header ingress may already contain a body prefix, which also fits here.
		const auto body_capacity = std::max({limits.max_body_size, configuration.header_bytes,
		    configuration.request_body_block_bytes});
		if(!job_count || connection_count > limits.max_connections ||
		    job_count > options.http_memory.maximum_requests || reserve > initial ||
		    !inputs_.allocate(pool, configuration.ingress_slots) || !jobs_.allocate(pool, job_count) ||
		    !clients_.allocate(pool, client_count) || !links_.allocate(pool, limits.max_connections)) return;

		if(options.keep_alive.enabled && (!options.keep_alive.maximum_idle || !options.keep_alive.maximum_requests ||
		    !options.keep_alive.idle_timeout_ms || !idle_.allocate(pool, options.keep_alive.maximum_idle))) return;

		if(job_count > reserve &&
		    (!headers_.initialize(pool_, {initial - reserve, job_count - reserve, metadata}) ||
		    !bodies_.initialize(pool_, {0, job_count - reserve, body_capacity}) ||
		    !responses_.initialize(pool_, {initial - reserve, job_count - reserve, configuration.response_body_bytes})))
			return;

		if(reserve && (!reserved_headers_.initialize(pool_, {reserve, reserve, metadata}) ||
		    !reserved_bodies_.initialize(pool_, {reserve, reserve, body_capacity}) ||
		    !reserved_responses_.initialize(pool_, {reserve, reserve, configuration.response_body_bytes}))) return;

		for(std::size_t index = 0; index < inputs_.size(); ++index)
		{
			auto &input = inputs_[index];
			if(!input.head.allocate(pool, configuration.header_bytes) ||
			    !input.entries.allocate(pool, limits.max_headers) ||
			    !input.reply.allocate(pool, configuration.response_header_bytes)) return;
		}
		for(std::size_t index = 0; index < jobs_.size(); ++index)
		{
			auto &job = jobs_[index];
			job.owner = this;
		}
		const auto stream_count = options.max_websocket_clients;
#if NAWSERVER_TCP_WEBSOCKET
		if(stream_count && (!configuration.stream_bytes_per_client ||
		    !streams_.initialize(pool_, {options.http_memory.initial_websocket_clients, stream_count,
		                                 configuration.stream_bytes_per_client}))) return;
#else
		(void)stream_count;
#endif
		valid_ = true;
	}

	TcpMemoryStats HttpConnectionManager::stats() const
	{
		return {headers_.stats(), bodies_.stats(), responses_.stats(), streams_.stats(),
		        reserved_headers_.stats(), reserved_bodies_.stats(), reserved_responses_.stats()};
	}

	bool HttpConnectionManager::attach(Input &input, Handle handle, std::uint64_t now, std::uint16_t requests)
	{
		input.request = {};
		input.abort = false;
		input.reply_size = 0;
		input.handle = handle;
		input.phase = Phase::Head;
		input.used = input.offset = 0;
		input.status = 0;
		input.requests = requests;
		input.close_attempted = false;
		input.deadline = after(now, options_.managed.header_timeout_ms);
		auto wire_limits = limits_;
		wire_limits.max_header_bytes = input.head.size();
		wire_limits.max_body_size = std::numeric_limits<std::size_t>::max();
		input.parser.emplace(input.head.get(), input.head.size(), input.entries.get(), input.entries.size(),
		    wire_limits);
		return true;
	}

	bool HttpConnectionManager::accept(Handle handle, std::uint64_t now)
	{
		const std::size_t first = 0;
		for(std::size_t index = first; index < inputs_.size(); ++index)
		{
			auto &input = inputs_[index];
			if(input.phase != Phase::Free) continue;

			return attach(input, handle, now);
		}
		// No ingress workspace remains. This is the finite TCP boundary, not an
		// HTTP denial: no parsed request exists and no protected quota is consumed.
		orphan_ = handle;
		const auto closed = transport_.close(orphan_);
		orphan_close_ = closed;
		if(!closed.released) valid_ = false;

		return closed.released;
	}

	void HttpConnectionManager::reject(Input &input, int status, std::uint64_t now)
	{
		input.status = status;
		input.offset = 0;
		input.phase = Phase::Send;
		input.deadline = after(now, options_.managed.reject_timeout_ms);
	}

	SendResult HttpConnectionManager::captureInput(void *context, int status, const char *type,
	    const Header *headers, std::size_t count, ByteView body)
	{
		auto &input = *static_cast<Input *>(context);
		if(input.reply_size) return SendResult::AlreadySent;
		http::wire::ResponseWriter writer;
		const auto prepared = writer.prepare(input.parser->head().method, status, type, headers, count, body);
		if(prepared != SendResult::Ok) return prepared;
		std::size_t used = 0;
		while(!writer.complete())
		{
			const auto part = writer.current();
			if(part.size() > input.reply.size() - used) return SendResult::BufferTooSmall;
			std::memcpy(input.reply.get() + used, part.data(), part.size());
			used += part.size();
			if(!writer.advance(part.size())) return SendResult::InvalidArgument;
		}
		input.reply_size = used;
		return SendResult::Ok;
	}

	bool HttpConnectionManager::closeInput(Input &input)
	{
		if(!input.handle.valid) return true;

		if(input.close_attempted && input.closed.status != Status::WouldBlock) return false;
		if(!input.abort)
		{
			const auto pressure = transport_.writePressure(input.handle);
			if(pressure.jobs && tcpMilliseconds() < input.deadline)
			{ input.phase = Phase::Closing; return false; }
			if(pressure.failed || pressure.jobs) input.abort = true;
		}

		const auto result = input.abort ? transport_.abort(input.handle) : transport_.close(input.handle);
		input.closed = result;
		input.close_attempted = true;
		if(!result.released)
		{
			if(result.status != Status::WouldBlock) valid_ = false;
			input.phase = Phase::Closing;
			return false;
		}

		input.parser.reset();
		input.request = {};
		input.phase = Phase::Free;
		return true;
	}

	void HttpConnectionManager::admit(Input &input, std::size_t head_bytes, std::uint64_t now)
	{
		HeaderExchange exchange(*input.parser, input.request, {&input, captureInput},
		    transport_.peerAddress(input.handle));
		const auto decision = handler_.prepareHead(exchange, input.request);
		if(decision != HeaderDisposition::Continue)
		{
			if(decision == HeaderDisposition::Drop)
			{
				input.abort = true;
				(void)closeInput(input);
			}
			else reject(input, 503, now);
			return;
		}
		const auto length = input.parser->head().content_length;
		const auto &body_options = input.request.body;
		const auto maximum = body_options.max_bytes ? body_options.max_bytes : limits_.max_body_size;
		if(length > maximum || (body_options.mode == BodyMode::None && length) ||
		    (body_options.mode == BodyMode::Stream && (!body_options.begin || !body_options.block_bytes ||
		        body_options.block_bytes > options_.managed.request_body_block_bytes)))
		{
			reject(input, 400, now);
			return;
		}
		bool reserved = false;
#if NAWSERVER_CLIENT_QUOTAS
		reserved = input.request.resource_lease.reserved();
#endif
		const auto protected_count = options_.http_memory.reserved_requests;
		// Physical protected workspaces are selected by an opaque middleware result.
		// The allocator never examines credentials, IPs or client table indexes.
		const auto first = reserved ? std::size_t{0} : std::size_t{protected_count};
		Job *available = nullptr;
		for(std::size_t index = first; index < jobs_.size(); ++index)
			if(jobs_[index].phase == Phase::Free) { available = &jobs_[index]; break; }
		if(!available) { reject(input, 503, now); return; }
		auto &job = *available;
		job.reserved = static_cast<std::size_t>(available - jobs_.get()) < protected_count;
		auto &metadata = job.reserved ? reserved_headers_ : headers_;
		const auto aligned_head = (input.head.size() + alignof(http::wire::HeaderEntry) - 1) /
		    alignof(http::wire::HeaderEntry) * alignof(http::wire::HeaderEntry);
		const auto bytes = aligned_head + limits_.max_headers * sizeof(http::wire::HeaderEntry) +
		    options_.managed.response_header_bytes;
		job.storage = metadata.acquire(bytes);
		const auto prefix = body_options.mode == BodyMode::Stream ?
		    std::min(input.used - head_bytes, length) : input.used - head_bytes;
		const auto needed = body_options.mode == BodyMode::Stream ?
		    std::max(prefix, std::min(length, body_options.block_bytes)) : std::max(length, input.used - head_bytes);
		if(needed) job.body = (job.reserved ? reserved_bodies_ : bodies_).acquire(needed);
		if(!job.storage || (needed && !job.body))
		{
			job.storage.reset(); job.body.reset(); reject(input, 503, now); return;
		}
		if(body_options.mode == BodyMode::Stream)
		{
			job.receiver = body_options.begin(body_options.context, exchange);
			if(!job.receiver)
			{
				job.storage.reset(); job.body.reset(); reject(input, 503, now); return;
			}
		}
		job.head = {reinterpret_cast<char *>(job.storage.data()), input.head.size()};
		job.entries = {reinterpret_cast<http::wire::HeaderEntry *>(job.storage.data() + aligned_head), limits_.max_headers};
		for(std::size_t index = 0; index < job.entries.size(); ++index)
			new(job.entries.get() + index) http::wire::HeaderEntry{};
		job.output_head = {reinterpret_cast<char *>(job.storage.data() + aligned_head +
		               limits_.max_headers * sizeof(http::wire::HeaderEntry)), options_.managed.response_header_bytes};
		auto handle = input.handle;
		const auto token = registry_.claim(handle, Owner::Http);
		if(!token.valid())
		{
			job.receiver.reset(); job.storage.reset(); job.body.reset(); reject(input, 503, now); return;
		}
		std::memcpy(job.head.get(), input.head.get(), head_bytes);
		auto request_limits = limits_;
		request_limits.max_header_bytes = job.head.size();
		request_limits.max_body_size = std::numeric_limits<std::size_t>::max();
		job.parser.emplace(job.head.get(), job.head.size(), job.entries.get(), job.entries.size(), request_limits);
		job.token = token;
		job.request = std::move(input.request);
		input.handle = {};
		if(job.parser->advance(head_bytes).state != http::wire::ParseStatus::Complete)
		{
			(void)closeJob(job, now); input.phase = Phase::Free; return;
		}
		transport_.setWritePolicy(registry_.get(token, Owner::Http),
		    {reserved ? options_.transmission.important_priority : std::uint8_t{0}, 0, false});
		job.stream_client = static_cast<std::size_t>(-1);
		job.body_used = prefix;
		job.body_received = std::min(prefix, length);
		job.body_consumed = 0;
		if(prefix) std::memcpy(job.body.data(), input.head.get() + head_bytes, prefix);
		job.output_head_size = job.offset = job.discarded = 0;
		job.payload = {};
		job.abort = false;
		job.method = job.parser->head().method;
		job.requests = static_cast<std::uint16_t>(input.requests + 1);
		job.persistent = options_.keep_alive.enabled && input.parser->http11() &&
		    job.requests < options_.keep_alive.maximum_requests && input.used - head_bytes <= length;
		if(const auto connection = input.parser->headerValue("Connection"))
			job.persistent = job.persistent && !hasClose(*connection);
		job.deadline = after(now, limits_.exchange_timeout_ms);
		job.last_progress = now;
		job.sent_bytes = transport_.writePressure(registry_.get(token, Owner::Http)).sent_bytes;
		job.phase = Phase::Body;
		input.parser.reset();
		input.phase = Phase::Free;
	}

	bool HttpConnectionManager::readInput(Input &input, std::uint64_t now)
	{
		if(input.phase == Phase::Free) return false;
		if(input.phase == Phase::Closing) return closeInput(input);

		if(input.phase == Phase::Send)
		{
			if(now >= input.deadline) { (void)closeInput(input); return true; }

			const auto bytes = input.reply_size ? std::string_view(input.reply.get(), input.reply_size) : rejection(input.status);
			const auto sent = transport_.writeSome(input.handle,
			        reinterpret_cast<const std::uint8_t *>(bytes.data() + input.offset), bytes.size() - input.offset);
			if(sent.status == IoStatus::Progress && sent.size && sent.size <= bytes.size() - input.offset)
			{
				input.offset += sent.size;
				if(input.offset == bytes.size())
				{
					// Early denial never drains an untrusted body or parks keep-alive.
					(void)closeInput(input);
				}

				return true;
			}

			if(sent.status != IoStatus::WouldBlock && sent.status != IoStatus::Interrupted) (void)closeInput(input);

			return false;
		}

		if(input.phase == Phase::Drain)
		{
			if(now >= input.deadline || input.offset >= options_.close_drain_bytes)
			{
				(void)closeInput(input); return true;
			}

			const auto offered = std::min({std::size_t{256}, input.head.size(),
			                               options_.close_drain_bytes - input.offset});
			const auto received = transport_.readSome(input.handle, reinterpret_cast<std::uint8_t *>(input.head.get()),
			        offered);
			if(received.status == IoStatus::Progress && received.size && received.size <= offered)
			{ input.offset += received.size; return true; }

			if(received.status != IoStatus::WouldBlock && received.status != IoStatus::Interrupted)
			{ (void)closeInput(input); return true; }

			return false;
		}

		if(now >= input.deadline) { reject(input, 408, now); return true; }

		const auto received = transport_.readSome(input.handle,
		        reinterpret_cast<std::uint8_t *>(input.head.get()) + input.used, input.head.size() - input.used);
		if(received.status == IoStatus::Progress && received.size && received.size <= input.head.size() - input.used)
		{
			input.used += received.size;
			const auto parsed = input.parser->advance(input.used);
			if(parsed.state == http::wire::ParseStatus::Complete) admit(input, parsed.head_bytes, now);
			else if(parsed.state == http::wire::ParseStatus::Reject) reject(input, parsed.status, now);
			else if(input.used == input.head.size()) reject(input, 431, now);

			return true;
		}

		if(received.status != IoStatus::WouldBlock && received.status != IoStatus::Interrupted) (void)closeInput(input);

		return false;
	}

	void HttpConnectionManager::completeDirect(void *context)
	{
		auto &job = *static_cast<Job *>(context);
		job.direct_sent = true;
		// Exchange's streaming wire writer explicitly emits Connection: close.
		// Never park this connection in managed keep-alive after that promise.
		job.persistent = false;
	}

	SendResult HttpConnectionManager::capture(void *context, int status, const char *type, const Header *headers,
	    std::size_t count, ByteView body)
	{
		return captureBody(*static_cast<Job *>(context), status, type, headers, count, body, false);
	}

	SendResult HttpConnectionManager::captureImmutable(void *context, int status, const char *type, const Header *headers,
	    std::size_t count, ByteView body)
	{
		return captureBody(*static_cast<Job *>(context), status, type, headers, count, body, true);
	}

	SendResult HttpConnectionManager::captureBody(Job &job, int status, const char *type, const Header *headers,
	    std::size_t count, ByteView body, bool immutable)
	{
		if(job.output_head_size) return SendResult::AlreadySent;

		for(std::size_t index = 0; index < count; ++index)
			if(headers[index].name && headers[index].value &&
			    NaWServer::detail::asciiEqualIgnoreCase(headers[index].name,
			    "Connection") && hasClose(headers[index].value))
				job.persistent = false;

		http::wire::ResponseWriter writer;
		const auto prepared = writer.prepare(job.method, status, type, headers, count, body, job.persistent);
		if(prepared != SendResult::Ok) return prepared;

		std::size_t used = 0;
		while(!writer.complete() && !writer.bodySpan())
		{
			const auto part = writer.current();
			if(part.size() > job.output_head.size() - used) return SendResult::BufferTooSmall;

			std::memcpy(job.output_head.get() + used, part.data(), part.size());
			used += part.size();
			if(!writer.advance(part.size())) return SendResult::InvalidArgument;
		}
		const auto payload = writer.current();
		if(!immutable && !payload.empty())
		{
			job.output_body =
			    (job.reserved ? job.owner->reserved_responses_ : job.owner->responses_).acquire(payload.size());
			if(!job.output_body) return SendResult::BufferTooSmall;

			std::memcpy(job.output_body.data(), payload.data(), payload.size());
		}

		job.payload = immutable ? payload : ByteView{job.output_body.data(), payload.size()};
		job.immutable_payload = immutable;
		job.output_head_size = used;
		return SendResult::Ok;
	}

	SendResult HttpConnectionManager::captureOwned(void *context, int status, const char *type, const Header *headers,
	    std::size_t count, ByteView body, MemoryBlock &block)
	{
		auto &job = *static_cast<Job *>(context);
		const auto result = captureBody(job, status, type, headers, count, body, true);
		if(result == SendResult::Ok) { job.output_body = std::move(block); job.immutable_payload = false; }

		return result;
	}

	SendResult HttpConnectionManager::captureSource(void *context, int status, const char *type,
	    const Header *headers, std::size_t count, OwnedResponseSource &source)
	{
		auto &job = *static_cast<Job *>(context);
		if(job.output_head_size) return SendResult::AlreadySent;
		for(std::size_t index = 0; index < count; ++index)
			if(headers[index].name && headers[index].value &&
			    NaWServer::detail::asciiEqualIgnoreCase(headers[index].name, "Connection") && hasClose(headers[index].value))
				job.persistent = false;
		http::wire::ResponseWriter writer;
		const auto result = writer.prepareHead(job.method, status, type, headers, count, source.view().size, job.persistent);
		if(result != SendResult::Ok) return result;
		std::size_t used = 0;
		while(!writer.complete())
		{
			const auto part = writer.current();
			if(part.size() > job.output_head.size() - used) return SendResult::BufferTooSmall;
			std::memcpy(job.output_head.get() + used, part.data(), part.size());
			used += part.size();
			if(!writer.advance(part.size())) return SendResult::InvalidArgument;
		}
		job.output_head_size = used;
		job.source_offset = job.source_chunk_size = job.source_chunk_offset = 0;
		if(job.method != HttpMethod::Head) job.source = std::move(source);
		return SendResult::Ok;
	}

	HttpConnectionManager::Job *HttpConnectionManager::find(Token token)
	{
		for(std::size_t index = 0; index < jobs_.size(); ++index)
			if(jobs_[index].token.slot == token.slot && jobs_[index].token.generation == token.generation &&
			    token.valid())
				return &jobs_[index];

		return nullptr;
	}

#if NAWSERVER_TCP_WEBSOCKET
	WebSocketTxPool *HttpConnectionManager::websocketPool(Token token)
	{
		auto *job = find(token);
		if(!job) return nullptr;

		std::size_t selected = 0;
		while(selected < clients_.size() && clients_[selected].stream_active) ++selected;
		if(selected == clients_.size() || clients_[selected].stream_active) return nullptr;

		auto &client = clients_[selected];
		if(!client.arena.initialize(streams_, options_.managed.stream_bytes_per_client) ||
		    !client.streams.initialize({client.arena.allocator(), options_.managed.stream_bytes_per_client},
		    BufferGroup::Tcp))
		{
			client.streams.reset();
			client.arena.reset();
			return nullptr;
		}

		client.tx.emplace(options_.transmission.enabled ? 0 : options_.managed.websocket_tx_blocks_per_client,
		    options_.websocket_tx_block_size, &client.streams, options_.transmission.enabled);
		if(!client.tx->valid()) { releaseStream(selected); return nullptr; }

		client.stream_active = true;
		job->stream_client = selected;
		return &*client.tx;
	}

#endif
	void HttpConnectionManager::releaseStream(std::size_t index)
	{
#if NAWSERVER_TCP_WEBSOCKET
		if(index >= clients_.size()) return;

		auto &client = clients_[index];
		client.tx.reset();
		client.streams.reset();
		client.arena.reset();
		client.stream_active = false;
#else
		(void)index;
#endif
	}

	void HttpConnectionManager::releaseWork(Job &job, std::uint64_t now)
	{
		(void)now;
		job.receiver.reset();
		job.source.reset();
		job.request = {};
		releaseStream(job.stream_client);
		job.stream_client = static_cast<std::size_t>(-1);
		job.abort = false;
		job.token = {};
		job.parser.reset();
		job.payload = {};
		job.output_body.reset();
		job.body.reset();
		job.storage.reset();
		job.phase = Phase::Free;
		job.direct_sent = false;
		job.immutable_payload = false;
	}

	bool HttpConnectionManager::closeJob(Job &job, std::uint64_t now)
	{
		if(job.token.valid())
		{
			const bool closing = registry_.get(job.token, Owner::Closing).valid;
			if(closing || registry_.get(job.token, Owner::Http).valid)
			{
				if(!job.abort)
				{
					const auto pressure = transport_.writePressure(registry_.get(job.token, closing ? Owner::Closing : Owner::Http));
					if(pressure.jobs && now < job.deadline)
					{ job.phase = Phase::Closing; return false; }
					if(pressure.failed || pressure.jobs) job.abort = true;
				}
				const auto result = registry_.close(job.token, closing ? Owner::Closing : Owner::Http, job.abort);
				if(!result.released)
				{
					if(result.status != Status::WouldBlock) valid_ = false;
					job.phase = Phase::Closing;
					return false;
				}
			}
		}

		releaseWork(job, now);
		return true;
	}

	bool HttpConnectionManager::work(Job &job, std::uint64_t now)
	{
		if(job.phase == Phase::Free) return false;
		if(job.phase == Phase::Closing) return valid_ && closeJob(job, now);

		const auto handle = registry_.get(job.token, Owner::Http);
		if(!handle.valid) { valid_ = false; return false; }
		if(job.phase == Phase::Send && options_.transmission.enabled)
		{
			const auto pressure = transport_.writePressure(handle);
			if(pressure.sent_bytes != job.sent_bytes)
			{
				job.sent_bytes = pressure.sent_bytes;
				job.last_progress = now;
			}
		}

		if(job.phase != Phase::Drain && (now >= job.deadline ||
		    (now >= job.last_progress && now - job.last_progress >= options_.no_progress_timeout_ms)))
		{
			(void)closeJob(job, now);
			return true;
		}

		if(job.phase == Phase::Body)
		{
			const auto expected = job.parser->head().content_length;
			if(job.request.body.mode == BodyMode::Stream)
			{
				if(job.body_used)
				{
					const auto offered = std::min(job.body_used, job.request.body.block_bytes);
					const auto consumed = job.receiver.consume({job.body.data(), offered});
					if(consumed == BodyConsumeResult::Pause) return false;
					if(consumed == BodyConsumeResult::Error)
					{
						job.abort = true; (void)closeJob(job, now); return true;
					}
					job.body_consumed += offered;
					job.body_used -= offered;
					if(job.body_used) std::memmove(job.body.data(), job.body.data() + offered, job.body_used);
					job.last_progress = now;
					return true;
				}
				if(job.body_received < expected)
				{
					const auto offered = std::min({expected - job.body_received, job.body.size(),
					    options_.managed.io_slice_bytes, job.request.body.block_bytes});
					const auto received = transport_.readSome(handle, job.body.data(), offered);
					if(received.status == IoStatus::Progress && received.size && received.size <= offered)
					{
						job.body_used = received.size; job.body_received += received.size;
						job.last_progress = now; return true;
					}
					if(received.status != IoStatus::WouldBlock && received.status != IoStatus::Interrupted)
					{ job.abort = true; (void)closeJob(job, now); }
					return false;
				}
				if(!job.receiver.finish())
				{
					job.abort = true; (void)closeJob(job, now); return true;
				}
				job.receiver.reset();
				job.request.body_streamed = true;
			}
			else if(job.body_used < expected)
			{
				const auto received = transport_.readSome(handle, job.body.data() + job.body_used,
				        std::min(expected - job.body_used, options_.managed.io_slice_bytes));
				if(received.status == IoStatus::Progress && received.size && received.size <= expected - job.body_used)
				{
					job.body_used += received.size;
					job.last_progress = now;
					return true;
				}

				if(received.status != IoStatus::WouldBlock && received.status != IoStatus::Interrupted)
					(void)closeJob(job, now);

				return false;
			}

			ResponseTransport response{&job, capture, captureImmutable, captureOwned};
			response.direct_started = completeDirect;
			response.send_owned_source = captureSource;
			const auto disposition = dispatch_(context_, job.token, *job.parser, {job.body.data(), job.body_used},
			        response, job.request);
			if(disposition == ConnectionDisposition::WebSocket || disposition == ConnectionDisposition::External)
			{
				links_[job.token.slot] = {job.token, std::move(job.request),
					                      disposition ==
					                      ConnectionDisposition::WebSocket ? Owner::WebSocket : Owner::Sse,
					                      job.stream_client};
				job.stream_client = static_cast<std::size_t>(-1);
				releaseWork(job, now);
				return true;
			}

			// The callback no longer borrows the incoming body. Release it before
			// sending; persistent idle connections never pin request payloads.
			job.body.reset();
			if(disposition == ConnectionDisposition::Drop)
			{ job.abort = true; (void)closeJob(job, now); return true; }
			// A close response still owns captured output. Only Drop cancels it.
			if(disposition == ConnectionDisposition::Close) job.persistent = false;

			if(job.direct_sent)
			{
				// Direct source/stream already completed through the shared writer.
				// Reuse the ordinary completion/keep-alive path with an empty send.
				job.output_head_size = 0;
				job.payload = {};
				job.phase = Phase::Send;
				job.offset = 0;
				job.last_progress = now;
				return true;
			}

			if(!job.output_head_size)
			{
				// Refusal must not request another payload allocation after an OOM
				// or floor rejection. The already owned metadata contains this reply.
				const auto refused = rejection(503);
				if(refused.size() > job.output_head.size()) { (void)closeJob(job, now); return true; }

				std::memcpy(job.output_head.get(), refused.data(), refused.size());
				job.output_head_size = refused.size();
				job.payload = {};
				job.persistent = false;
			}

			job.phase = Phase::Send;
			job.offset = 0;
			job.last_progress = now;
			return true;
		}

		if(job.phase == Phase::Send)
		{
			if(job.offset == job.output_head_size && job.source)
			{
				const auto source = job.source.view();
				if(job.source_offset == source.size)
				{
					job.source.reset(); job.output_body.reset();
				}
				else if(!job.source_chunk_size)
				{
					const auto offered = std::min({source.size - job.source_offset, options_.managed.io_slice_bytes,
					    options_.managed.response_body_bytes});
					if(options_.transmission.enabled)
					{
						// Fill a reserved TX block directly. Its Filling lease prevents the
						// writer/Drop owner from recycling bytes while the source reads.
						auto lease = transport_.acquireWrite(handle, offered);
						if(!lease) return false;
						const auto read = source.read(source.context, job.source_offset, lease.data(), lease.capacity());
						if(read.status != BodyReadStatus::Data || !read.size || read.size > lease.capacity() ||
						    !lease.commit(read.size))
						{
							lease.reset(); job.abort = true; (void)closeJob(job, now); return true;
						}
						job.source_offset += read.size; job.last_progress = now;
						return true;
					}
					job.output_body = (job.reserved ? reserved_responses_ : responses_).acquire(offered);
					if(!job.output_body) { job.abort = true; (void)closeJob(job, now); return true; }
					const auto read = source.read(source.context, job.source_offset, job.output_body.data(), offered);
					if(read.status != BodyReadStatus::Data || !read.size || read.size > offered)
					{ job.abort = true; (void)closeJob(job, now); return true; }
					job.source_chunk_size = read.size; job.source_chunk_offset = 0;
					return true;
				}
				else
				{
					const auto offered = job.source_chunk_size - job.source_chunk_offset;
					const auto sent = transport_.writeSome(handle, job.output_body.data() + job.source_chunk_offset, offered);
					if(sent.status == IoStatus::Progress && sent.size && sent.size <= offered)
					{
						job.source_offset += sent.size; job.source_chunk_offset += sent.size;
						job.last_progress = now;
						if(job.source_chunk_offset == job.source_chunk_size)
						{ job.source_chunk_size = job.source_chunk_offset = 0; job.output_body.reset(); }
						return true;
					}
					if(sent.status != IoStatus::WouldBlock && sent.status != IoStatus::Interrupted)
					{ job.abort = true; (void)closeJob(job, now); }
					return false;
				}
			}
			const bool head = job.offset < job.output_head_size;
			const auto offset = head ? job.offset : job.offset - job.output_head_size;
			const auto size = head ? job.output_head_size : job.payload.size();
			const auto *bytes =
			    head ? reinterpret_cast<const std::uint8_t *>(job.output_head.get()) : job.payload.data();
			if(offset == size)
			{
				const auto pressure = transport_.writePressure(handle);
				if(pressure.failed) { job.abort = true; (void)closeJob(job, now); return true; }
				if(pressure.jobs) return false;
				if(job.persistent)
				{
					const auto protected_count = std::min<std::size_t>(options_.http_memory.reserved_requests, idle_.size());
					const auto first = !job.reserved ? protected_count : std::size_t{0};
					const auto last = idle_.size();
					for(std::size_t index = first; index < last; ++index)
					{
						auto &idle = idle_[index];
						if(idle.handle.valid) continue;

						idle.handle = registry_.releaseHttp(job.token);
						if(!idle.handle.valid) { valid_ = false; return false; }

						idle.requests = job.requests;
						idle.deadline = after(now, options_.keep_alive.idle_timeout_ms);
						idle.attempted = false;
						idle.request = std::move(job.request);
						releaseWork(job, now);
						return true;
					}
				}

				job.phase = Phase::Drain;
				job.close_deadline = after(now, options_.close_timeout_ms);
				return true;
			}

			const auto offered = std::min(size - offset, options_.managed.io_slice_bytes);
			const auto sent = !head && job.immutable_payload ? transport_.writeImmutableSome(handle, bytes + offset, offered) :
			    transport_.writeSome(handle, bytes + offset, offered);
			if(sent.status == IoStatus::Progress && sent.size && sent.size <= offered)
			{
				job.offset += sent.size;
				job.last_progress = now;
				return true;
			}

			if(sent.status != IoStatus::WouldBlock && sent.status != IoStatus::Interrupted) (void)closeJob(job, now);

			return false;
		}

		if(now >= job.close_deadline || job.discarded >= options_.close_drain_bytes)
		{
			(void)closeJob(job, now);
			return true;
		}

		std::uint8_t discard[256];
		const auto offered = std::min(sizeof(discard), options_.close_drain_bytes - job.discarded);
		const auto received = transport_.readSome(handle, discard, offered);
		if(received.status == IoStatus::Progress && received.size && received.size <= offered)
		{ job.discarded += received.size; return true; }

		if(received.status != IoStatus::WouldBlock && received.status != IoStatus::Interrupted)
			(void)closeJob(job, now);

		return false;
	}

	bool HttpConnectionManager::closeIdle(Idle &idle)
	{
		if(!idle.handle.valid) return true;

		if(idle.attempted && idle.closed.status != Status::WouldBlock) return false;

		idle.closed = transport_.close(idle.handle);
		idle.attempted = true;
		if(!idle.closed.released)
		{
			if(idle.closed.status != Status::WouldBlock) valid_ = false;
			return false;
		}
		idle.request = {};

		return true;
	}

	bool HttpConnectionManager::pollIdle(Idle &idle, std::uint64_t now)
	{
		if(!idle.handle.valid) return false;

		if(now >= idle.deadline) { (void)closeIdle(idle); return true; }

		const std::size_t first = 0;
		for(std::size_t index = first; index < inputs_.size(); ++index)
		{
			auto &input = inputs_[index];
			if(input.phase != Phase::Free) continue;

			std::uint8_t first_byte;
			const auto read = transport_.readSome(idle.handle, &first_byte, 1);
			if(read.status == IoStatus::Progress && read.size == 1)
			{
				// The next request gets a fresh identity; idle quota was retained
				// until this point so sleeping sockets could not evade accounting.
				idle.request = {};
				(void)attach(input, idle.handle, now, idle.requests);
				input.head[0] = static_cast<char>(first_byte);
				input.used = 1;
				idle.handle = {};
				return true;
			}

			if(read.status != IoStatus::WouldBlock && read.status != IoStatus::Interrupted) (void)closeIdle(idle);

			break;
		}
		return false;
	}

	bool HttpConnectionManager::pump(std::uint64_t now)
	{
		bool progress = false;
		for(std::size_t index = 0; index < links_.size(); ++index)
		{
			auto &link = links_[index];
			if(link.token.valid() && !registry_.get(link.token, link.owner).valid && !registry_.get(link.token,
			    Owner::Closing).valid)
			{

				releaseStream(link.client);
				link = {};
			}
		}
		// Reserved jobs are at the start. A bounded syscall per job never holds
		// another identity's response or enters its application callback recursively.
		for(std::size_t index = 0; index < jobs_.size(); ++index)
			progress = work(jobs_[index], now) || progress;
		for(std::size_t index = 0; index < inputs_.size(); ++index)
			progress = readInput(inputs_[index], now) || progress;
		for(std::size_t index = 0; index < idle_.size(); ++index)
			progress = pollIdle(idle_[index], now) || progress;
		return progress;
	}

	bool HttpConnectionManager::cleanup()
	{
		bool released = true;
		if(orphan_.valid)
		{
			if(orphan_close_.status == Status::WouldBlock) orphan_close_ = transport_.close(orphan_);

			released = !orphan_.valid;
		}

		for(std::size_t index = 0; index < inputs_.size(); ++index)
			if(inputs_[index].handle.valid) released = closeInput(inputs_[index]) && released;

		for(std::size_t index = 0; index < idle_.size(); ++index)
			if(idle_[index].handle.valid) released = closeIdle(idle_[index]) && released;

		for(std::size_t index = 0; index < jobs_.size(); ++index)
			if(jobs_[index].phase != Phase::Free || jobs_[index].token.valid())
				released = closeJob(jobs_[index], tcpMilliseconds()) && released;

		for(std::size_t index = 0; index < links_.size(); ++index)
		{
			auto &link = links_[index];
			if(link.token.valid())
			{
				if(registry_.get(link.token, link.owner).valid || registry_.get(link.token, Owner::Closing).valid)
					released = false;
				else
				{

					releaseStream(link.client);
					link = {};
				}
			}
		}
		return released;
	}

#endif

} // namespace NaWServer::tcp::detail
