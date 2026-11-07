// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once
#include <NaWServer/handler.h>
#include <NaWServer/pipeline.h>
#include <optional>

namespace NaWServer
{
	// Exact HTTP route, all dependencies borrowed. WS has its own transport and
	// cannot be wrapped with this route to claim pre-upgrade authentication.
	class Route : public HandlerBase
	{
		const char *path_;
		EndpointRef endpoint_;
		Pipeline pipeline_;
		AccessPolicy access_;
		std::optional<ResponseOptions> options_;
		RouteDescriptor descriptor_;
		HeaderPipeline header_pipeline_;
		BodyOptions body_;

	public:
		static constexpr HttpMethod any_method = HttpMethod::Any;
		Route(
			const char *path,
			HttpMethod method,
			EndpointRef endpoint,
			Pipeline pipeline = {},
			AccessPolicy access = AccessPolicy::public_access(),
			std::optional<ResponseOptions> options = {},
			HeaderPipeline header_pipeline = {}, BodyOptions body = {})
		  : path_(path), endpoint_(endpoint), pipeline_(pipeline), access_(access), options_(options),
		    descriptor_{path, method}, header_pipeline_(header_pipeline), body_(body)
		{
		}

		HttpError validate() const override;
		RouteDescriptors routes() const override
		{
			return {&descriptor_, 1};
		}

		HeaderPipeline headerPipeline() const override
		{
			return header_pipeline_;
		}

		AccessPolicy accessPolicy() const override
		{
			return access_;
		}

		BodyOptions bodyOptions() const override
		{
			return body_;
		}

		void handleRequest(Request *request, Response &response) override;
	};
} // namespace NaWServer
