// SPDX-License-Identifier: MIT
#pragma once
#include "platform.hpp"
#include <NaWServer/backends/tcp.h>

namespace compact
{

	class Demo
	{
		struct Impl;
		Impl *impl_;
		void *storage_ = nullptr;
	public:
		Demo(NaWServer::TcpHttpBackend &backend, Platform &platform);
		~Demo();
		Demo(const Demo &) = delete;
		Demo &operator=(const Demo &) = delete;
		bool start();
		bool stop();
		std::size_t activeStreams() const;
		void tick();
		static NaWServer::TcpOptions options();
		static NaWServer::HttpLimits limits();
	};

}
