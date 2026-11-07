// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "sse_stream.h"

namespace NaWServer::tcp::detail
{

	TcpSseExchange::TcpSseExchange(TcpSseLeaseProvider &provider, Token token)
	  : provider_(provider), token_(token)
	{
	}

	TcpSseExchange::~TcpSseExchange()
	{
		if(lease_) lease_->rollback();
	}

	const void *TcpSseExchange::owner() const
	{
		return provider_.identity();
	}

	std::shared_ptr<sse::Stream> TcpSseExchange::retain()
	{
		if(retain_attempted_ || callback_complete_) return {};

		retain_attempted_ = true;
		lease_ = provider_.retainSse(token_);
		return lease_;
	}

	bool TcpSseExchange::callbackComplete()
	{
		if(callback_complete_) return external_;

		callback_complete_ = true;
		external_ = lease_ && lease_->isOpen();
		if(lease_) lease_->callbackComplete();

		return external_;
	}

} // namespace NaWServer::tcp::detail
