// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov
#pragma once
#include <array>
#include <cstddef>
#include <type_traits>
#include <NaWServer/response.h>
#include <NaWServer/access_policy.h>
#include <NaWServer/header_response.h>
namespace NaWServer
{
	class Request;
	class HeaderRequest;
	enum class Result { Complete, Unauthorized, Forbidden, Error, InvalidNext, Drop };
	inline Result responseResult(SendResult result)
	{
		return result == SendResult::Ok ? Result::Complete : Result::Error;
	}

	template <class RequestType> struct BasicContext
	{
		RequestType &request;
		Response &response;
		const AccessPolicy &access;
#if NAWSERVER_AUTH
		Principal principal{};
		const char *authentication_challenge{};
		const void *authenticated_provider{};
#endif
	};
	using Context = BasicContext<Request>;
	struct HeaderContext
	{
		HeaderRequest &request;
		HeaderResponse response;
		const AccessPolicy &access;
#if NAWSERVER_AUTH
		Principal principal{};
		const char *authentication_challenge{};
		const void *authenticated_provider{};
#endif
	};
	template <class C> inline Result checkAccess(const C &context)
	{
		if(!context.access.requires_authentication && !context.access.required_permissions) return Result::Complete;

#if NAWSERVER_AUTH
		if(!context.principal.authenticated) return Result::Unauthorized;

		if((context.principal.permissions & context.access.required_permissions) != context.access.required_permissions)
			return Result::Forbidden;

		return Result::Complete;
#else
		return Result::Unauthorized;
#endif
	}

	namespace detail
	{
		template <class C> struct Execution;
		template <class T> void *borrowAddress(T &object)
		{
			return const_cast<void *>(static_cast<const void *>(&object));
		}

	}
	// Borrowed continuation: one call, on this stack, on this execution context.
	template <class C> class BasicNext
	{
		friend struct detail::Execution<C>;
		detail::Execution<C> &execution_;
		std::size_t index_;
		bool called_{};
		BasicNext(detail::Execution<C> &execution, std::size_t index)
		  : execution_(execution), index_(index)
		{
		}

	public:
		BasicNext(const BasicNext &) = delete;
		BasicNext &operator=(const BasicNext &) = delete;
		Result operator()();
	};
	using Next = BasicNext<Context>;
	using HeaderNext = BasicNext<HeaderContext>;
	template <class C> class BasicMiddlewareRef
	{
		void *object_;
		Result (*invoke_)(void *, C &, BasicNext<C> &);
	public:
		template <class M, std::enable_if_t<!std::is_same_v<std::remove_cv_t<M>, BasicMiddlewareRef>, int> = 0>
		explicit BasicMiddlewareRef(M &middleware)
		  : object_(detail::borrowAddress(middleware)), invoke_([](void *object, C &context, BasicNext<C> &next)
		{
			return (*static_cast<M *>(object))(context, next);
		})
		{
		}

		Result operator()(C &context, BasicNext<C> &next) const
		{
			return invoke_(object_, context, next);
		}

	};
	using MiddlewareRef = BasicMiddlewareRef<Context>;
	using HeaderMiddlewareRef = BasicMiddlewareRef<HeaderContext>;
	template <class M> MiddlewareRef middlewareRef(M &middleware)
	{
		return MiddlewareRef(middleware);
	}

	template <class M> HeaderMiddlewareRef headerMiddlewareRef(M &middleware)
	{
		return HeaderMiddlewareRef(middleware);
	}

	template <class C> class BasicEndpointRef
	{
		using RequestType = std::remove_reference_t<decltype(std::declval<C>().request)>;
		using ResponseType = std::remove_reference_t<decltype(std::declval<C>().response)>;
		void *object_{};
		Result (*invoke_)(void *, C &)
		{
		};
		Result (*function_)(C &)
		{
		};
		SendResult (*pair_)(RequestType &, ResponseType &)
		{
		};
		SendResult (*response_)(ResponseType &)
		{
		};
	public:
		BasicEndpointRef(SendResult(*function)(RequestType &, ResponseType &))
		  : pair_(function)
		{
		}

		BasicEndpointRef(SendResult(*function)(ResponseType &))
		  : response_(function)
		{
		}

		BasicEndpointRef(Result(*function)(C &))
		  : function_(function)
		{
		}

		template <class E, std::enable_if_t<!std::is_function_v<E> && !std::is_same_v<std::remove_cv_t<E>,
		    BasicEndpointRef>, int> = 0>
		explicit BasicEndpointRef(E &endpoint)
		  : object_(detail::borrowAddress(endpoint)), invoke_([](void *object, C &context)
		{
			auto &endpoint = *static_cast<E *>(object);
			if constexpr(std::is_invocable_r_v<Result, E &, C &>) return endpoint(context);
			else if constexpr(std::is_invocable_r_v<SendResult, E &, RequestType &, ResponseType &>)
				return responseResult(endpoint(context.request, context.response));
			else return responseResult(endpoint(context.response));
		})
		{
		}

		Result operator()(C &context) const
		{
			if(invoke_) return invoke_(object_, context);

			if(function_) return function_(context);

			if(pair_) return responseResult(pair_(context.request, context.response));

			return response_ ? responseResult(response_(context.response)) : Result::Error;
		}

	};
	using EndpointRef = BasicEndpointRef<Context>;
	using HeaderEndpointRef = BasicEndpointRef<HeaderContext>;
	template <class C> class BasicPipeline
	{
		const BasicMiddlewareRef<C> *items_{};
		std::size_t size_{};
	public:
		constexpr BasicPipeline() = default;
		constexpr BasicPipeline(const BasicMiddlewareRef<C> *items, std::size_t size)
		  : items_(items), size_(size)
		{
		}

		Result run(C &context, BasicEndpointRef<C> endpoint, bool retain_identity = false,
		    bool enforce_access = true) const;
		constexpr std::size_t size() const
		{
			return size_;
		}

	};
	using Pipeline = BasicPipeline<Context>;
	using HeaderPipeline = BasicPipeline<HeaderContext>;
	template <class C, std::size_t N> class BasicFixedChain
	{
		std::array<BasicMiddlewareRef<C>, N> items_;
	public:
		template <class ... Refs,
		    std::enable_if_t<sizeof...(Refs) == N && (std::is_same_v<Refs, BasicMiddlewareRef<C> > && ...), int> = 0>
		explicit BasicFixedChain(Refs... refs)
		  : items_{{refs ...}}
		{
		}

		BasicFixedChain(const BasicFixedChain &) = delete;
		BasicFixedChain &operator=(const BasicFixedChain &) = delete;
		BasicPipeline<C> view() const &
		{
			return {items_.data(), N};
		}

		BasicPipeline<C> view() const && = delete;
		operator BasicPipeline<C>() const & { return view(); }
		operator BasicPipeline<C>() const && = delete;
		Result run(C &context, BasicEndpointRef<C> endpoint) const
		{
			return view().run(context, endpoint);
		}

	};
	template <std::size_t N> using FixedChain = BasicFixedChain<Context, N>;
	template <std::size_t N> using HeaderFixedChain = BasicFixedChain<HeaderContext, N>;
	template <class ... M> class Chain : public FixedChain<sizeof...(M)>
	{
	public:
		explicit Chain(M &... middleware)
		  : FixedChain<sizeof...(M)>(middlewareRef(middleware)...)
		{
		}

	};
	template <class ... M> class HeaderChain : public HeaderFixedChain<sizeof...(M)>
	{
	public:
		explicit HeaderChain(M &... middleware)
		  : HeaderFixedChain<sizeof...(M)>(headerMiddlewareRef(middleware)...)
		{
		}

	};
	template <class ... M> Chain<M...> makeChain(M &... middleware)
	{
		return Chain<M...>(middleware ...);
	}

	template <class ... M> HeaderChain<M...> makeHeaderChain(M &... middleware)
	{
		return HeaderChain<M...>(middleware ...);
	}

	namespace detail
	{
		// Both stages use this executor. Only the request view/capabilities differ.
		template <class C> struct Execution
		{
			const BasicMiddlewareRef<C> *items;
			std::size_t size;
			C &context;
			BasicEndpointRef<C> endpoint;
			bool invalid_next{};
			bool enforce_access = true;
			Result invoke(std::size_t index)
			{
				if(context.response.dropped()) return Result::Drop;

				if(index == size)
				{
					const auto access = enforce_access ? checkAccess(context) : Result::Complete;
					const auto result = access == Result::Complete ? endpoint(context) : access;
					if(result == Result::Drop) context.response.drop();

					return context.response.dropped() ? Result::Drop : result;
				}

				BasicNext<C> next(*this, index + 1);
				const auto result = items[index](context, next);
				if(result == Result::Drop) context.response.drop();

				return context.response.dropped() ? Result::Drop : result;
			}

		};
	}
	template <class C> Result BasicNext<C>::operator()()
	{
		if(called_) { execution_.invalid_next = true; return Result::InvalidNext; }

		called_ = true;
		return execution_.invoke(index_);
	}

	template <class C> Result BasicPipeline<C>::run(C &context, BasicEndpointRef<C> endpoint, bool retain_identity,
	    bool enforce_access) const
	{
		if(size_ && !items_) return Result::Error;

#if NAWSERVER_AUTH
		if(!retain_identity)
		{
			context.principal = {};
			context.authenticated_provider = nullptr;
		}

		context.authentication_challenge = nullptr;
#else
		(void)retain_identity;
#endif
		detail::Execution<C> execution{items_, size_, context, endpoint, false, enforce_access};
		const auto result = execution.invoke(0);
		if(result == Result::Drop || context.response.dropped()) { context.response.drop(); return Result::Drop; }

		return execution.invalid_next ? Result::InvalidNext : result;
	}

}
