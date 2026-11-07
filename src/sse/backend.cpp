// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/sse_backend.h>

namespace NaWServer::sse
{
	Backend::Reservation::~Reservation()
	{
		if(owner_) owner_->rollback(*this);
	}

}
