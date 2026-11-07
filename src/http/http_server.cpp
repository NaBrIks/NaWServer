// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#include <NaWServer/http_server.h>
#include <NaWServer/request.h>
#include <cstring>
namespace NaWServer
{
	namespace
	{
		// Bind persistent request state without retaining middleware stack objects.
		class StateExchange final : public HttpExchange
		{
			HttpExchange &exchange_;
			HttpRequestState &state_;
		public:
			StateExchange(HttpExchange &exchange, HttpRequestState &state)
			  : exchange_(exchange), state_(state)
			{
			}

			HttpRequestHead head() const override
			{
				return exchange_.head();
			}

			HeaderReadResult readHeader(const char *name, char *destination, std::size_t capacity) const override
			{
				return exchange_.readHeader(name, destination, capacity);
			}

			BodyReadResult readBody(uint8_t *destination, std::size_t capacity) override
			{
				return exchange_.readBody(destination, capacity);
			}

			std::optional<ByteView> preparedBody() const override
			{
				return exchange_.preparedBody();
			}

			ResponseTransport responseTransport() override
			{
				return exchange_.responseTransport();
			}

			void *extension(const void *key) const override
			{
				return exchange_.extension(key);
			}

			HttpRequestState *requestState() const override
			{
				return &state_;
			}

			std::optional<std::uint32_t> peerAddress() const override
			{
				return exchange_.peerAddress();
			}

		};
	}
	HttpError HttpServer::begin()
	{
		if(cleanup_required_) return HttpError::CleanupRequired;

		if(running_) return HttpError::Ok;

		if(!limits_.max_body_size || !limits_.max_header_bytes || !limits_.max_headers ||
		    !limits_.max_target_size || !limits_.max_connections || !limits_.exchange_timeout_ms)
			return HttpError::InvalidArgument;

		for(std::size_t index = 0; index < count_; ++index)
		{
			if(!handlers_[index]) return HttpError::InvalidArgument;

			const auto result = handlers_[index]->validate();
			if(result != HttpError::Ok) return result;

#if !NAWSERVER_AUTH
			const auto access = handlers_[index]->accessPolicy();
			if(access.requires_authentication || access.required_permissions) return HttpError::Unsupported;

#endif
			const auto routes = handlers_[index]->routes();
			if(routes.count && !routes.data) return HttpError::InvalidArgument;

			for(std::size_t route = 0; route < routes.count; ++route)
			{
				const auto *path = routes.data[route].path;
				if(!path || path[0] != '/' || std::strchr(path, '?')) return HttpError::InvalidArgument;
			}
			const auto body = handlers_[index]->bodyOptions();
			if(body.mode == BodyMode::Stream && (!body.block_bytes || !body.begin)) return HttpError::InvalidArgument;
		}
		const auto result = backend_.start(*this, limits_);
		if(result == HttpError::Ok) running_ = true;
		else if(result == HttpError::CleanupRequired) cleanup_required_ = true;

		return result;
	}

	HttpError HttpServer::end()
	{
		if(!running_ && !cleanup_required_) return HttpError::Ok;

		const auto result = backend_.stop();
		if(result == HttpError::Ok) { running_ = false; cleanup_required_ = false; }

		return result;
	}

	void HttpServer::onNotFound(std::function<void(Request &, Response &)> callback)
	{
		if(!running_ && !cleanup_required_) not_found_ = std::move(callback);
	}

	HeaderDisposition HttpServer::prepareHead(HttpExchange &exchange, HttpRequestState &state)
	{
		return prepareHttpHead(exchange, state, handlers_, count_, defaults_, options_, limits_, header_pipeline_,
		           static_cast<bool>(not_found_));
	}

	ExchangeDisposition HttpServer::handle(HttpExchange &exchange)
	{
		return dispatchHttp(exchange, handlers_, count_, defaults_, options_, limits_, not_found_, header_pipeline_);
	}

	HeaderDisposition prepareHttpHead(HttpExchange &exchange, HttpRequestState &state,
	    HandlerBase *const *handlers, std::size_t count, DefaultHeaders defaults,
	    ResponseOptions options, const HttpLimits &limits, HeaderPipeline header_pipeline, bool has_not_found)
	{
		if(state.prepared) return state.disposition;

		state.prepared = true;
		state.disposition = HeaderDisposition::Reply;
		StateExchange bound(exchange, state);
		HeaderRequest request(bound);
		Response response(bound.responseTransport(), defaults, options);
		const auto head = exchange.head();
		if(head.target.empty() || head.target.front() != '/' || head.target.size() > limits.max_target_size)
		{
			(void)response.text(400, "Invalid request target");
			return state.disposition;
		}

		// Exact paths precede prefixes; among prefixes the longest wins. Method
		// mismatch on the selected path never falls through to a broader route.
		bool path_found = false, best_exact = false;
		std::size_t best_length = 0;
		unsigned allowed_methods = 0;
		const auto path = request.path();
		for(std::size_t index = 0; index < count; ++index)
		{
			const auto descriptors = handlers[index]->routes();
			for(std::size_t item = 0; item < descriptors.count; ++item)
			{
				const auto &descriptor = descriptors.data[item];
				if(!descriptor.path) continue;

				const std::string_view candidate(descriptor.path);
				const bool exact = descriptor.match == RouteMatch::Exact;
				if(exact ? path != candidate : path.substr(0, candidate.size()) != candidate) continue;

				const bool better = !path_found || (exact && !best_exact) ||
				    (exact == best_exact && candidate.size() > best_length);
				const bool same = path_found && exact == best_exact && candidate.size() == best_length;
				if(!better && !same) continue;

				if(better)
				{
					path_found = true; best_exact = exact; best_length = candidate.size(); state.route = nullptr;
					allowed_methods = 0;
				}

				allowed_methods |= 1u << static_cast<unsigned>(descriptor.method);
				if(!state.route && (descriptor.method == HttpMethod::Any || descriptor.method == head.method))
				{
					state.route = handlers[index]; state.route_index = item;
				}
			}
		}
		if(path_found && !state.route)
		{
			constexpr const char *method_names[]{"", "GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"};
			char allowed[64];
			std::size_t used = 0;
			for(unsigned method = 1; method < sizeof(method_names) / sizeof(method_names[0]); ++method)
			{
				if(!(allowed_methods & (1u << method))) continue;

				if(used) { allowed[used++] = ','; allowed[used++] = ' '; }

				const auto length = std::strlen(method_names[method]);
				std::memcpy(allowed + used, method_names[method], length);
				used += length;
			}
			(void)response.header("Allow", {allowed, used});
			(void)response.text(405, "Method not allowed");
			return state.disposition;
		}

		if(!state.route && !has_not_found)
		{
			(void)response.text(404, "Route not found");
			return state.disposition;
		}

		state.body = state.route ? state.route->bodyOptions() : BodyOptions{};
		const auto body_limit = state.body.max_bytes ? state.body.max_bytes : limits.max_body_size;
		if((state.body.mode == BodyMode::None && head.content_length) || head.content_length > body_limit)
		{
			(void)response.text(413, "Request body exceeds route limit");
			return state.disposition;
		}

		const auto access = state.route ? state.route->accessPolicy() : AccessPolicy{};
		HeaderContext context{request, response, access};
		auto admitted = [](HeaderContext &) {
				return Result::Complete;
			};
		const auto route_pipeline = state.route ? state.route->headerPipeline() : HeaderPipeline{};
		auto route = [&](HeaderContext &ctx)
			{
				return route_pipeline.run(ctx, HeaderEndpointRef(admitted), true);
			};
		auto result = header_pipeline.run(context, HeaderEndpointRef(route), false, false);
		if(result == Result::Complete && !response.started()) result = checkAccess(context);

#if NAWSERVER_AUTH
		state.principal = context.principal;
		state.authenticated_provider = context.authenticated_provider;
#endif
		if(result == Result::Drop || response.dropped())
		{
			return state.disposition = HeaderDisposition::Drop;
		}

		if(response.started() || response.failed())
		{
			return state.disposition;
		}

		if(result != Result::Complete)
		{
			if(result == Result::Unauthorized)
			{
#if NAWSERVER_AUTH
				if(context.authentication_challenge)
					(void)response.header("WWW-Authenticate", context.authentication_challenge);

#endif

				(void)response.send(401, "text/plain", {reinterpret_cast<const uint8_t *>("Unauthorized"), 12});
			}
			else if(result == Result::Forbidden)
				(void)response.send(403, "text/plain", {reinterpret_cast<const uint8_t *>("Forbidden"), 9});
			else (void)response.text(500, "Header middleware failed");

			return state.disposition;
		}

		return state.disposition = HeaderDisposition::Continue;
	}

	ExchangeDisposition dispatchHttp(HttpExchange &exchange, HandlerBase *const *handlers,
	    std::size_t count, DefaultHeaders defaults, ResponseOptions options,
	    const HttpLimits &limits, const std::function<void(Request &, Response &)> &not_found,
	    HeaderPipeline header_pipeline)
	{
		HttpRequestState local_state;
		auto &state = exchange.requestState() ? *exchange.requestState() : local_state;
		const auto admitted = prepareHttpHead(exchange, state, handlers, count, defaults, options, limits,
		        header_pipeline, static_cast<bool>(not_found));
		if(admitted != HeaderDisposition::Continue)
			return admitted ==
			       HeaderDisposition::Drop ? ExchangeDisposition::Drop : ExchangeDisposition::CloseConnection;

		StateExchange bound(exchange, state);
		Request request(bound);
		Response response(bound.responseTransport(), defaults, options);
		const auto head = exchange.head();
		if(state.body.mode == BodyMode::Stream)
		{
			if(!state.body_streamed)
			{
				(void)response.text(501, "Streaming input requires a streaming backend");
				return ExchangeDisposition::CloseConnection;
			}
		}
		else if(const auto prepared = exchange.preparedBody())
		{
			if(prepared->size() != head.content_length || (prepared->size() && !prepared->data()))
			{
				(void)response.text(400, "Invalid prepared request body");
				return ExchangeDisposition::CloseConnection;
			}

			request.borrowBody(*prepared);
		}
		else
		{
			std::vector<uint8_t> body(head.content_length);
			std::size_t received = 0;
			while(received < body.size())
			{
				const auto result = exchange.readBody(body.data() + received, body.size() - received);
				if(result.status != BodyReadStatus::Data || !result.size || result.size > body.size() - received)
				{
					(void)response.text(result.status == BodyReadStatus::Timeout ? 408 : 400,
					    "Incomplete request body");
					return ExchangeDisposition::CloseConnection;
				}

				received += result.size;
			}
			request.setBody(std::move(body));
		}

		if(!request.prepareParameters())
		{
			(void)response.text(400, "Invalid query/form encoding");
			return ExchangeDisposition::CloseConnection;
		}

		if(state.route) state.route->handleRequest(&request, response);
		else if(not_found) not_found(request, response);
		else (void)response.text(404, "Route not found");

		if(response.dropped()) return ExchangeDisposition::Drop;

		if(response.state() == ResponseState::External) return ExchangeDisposition::Complete;

		if(!response.started() && !response.failed()) (void)response.text(500, "Handler did not send a response");

		if(response.state() == ResponseState::Sending) response.fail();

		return response.failed() ? ExchangeDisposition::CloseConnection : ExchangeDisposition::Complete;
	}

}
