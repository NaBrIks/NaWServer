// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#pragma once

#include <NaWServer/pipeline.h>
#include <NaWServer/header_request.h>
#include <array>
#include <charconv>
#include <cstring>

#ifndef NAWSERVER_PAGE_ID_BYTES
#define NAWSERVER_PAGE_ID_BYTES 32
#endif

namespace NaWServer
{
#if NAWSERVER_CLIENT_QUOTAS

	enum class ClientIdentitySource { PeerAddress, Page, AuthenticatedUser, PeerAndUser };

	// Optional header-stage identity resolver. Page labels are untrusted metadata,
	// never authentication. Only the admission policy can recognize a protected key.
	// No heap, decoded parameter maps or borrowed strings survive this invocation.
	class ClientIdentity
	{
		ClientIdentitySource source_;
		const char *page_header_;
		std::string_view page_parameter_;

		static bool safePage(std::string_view page)
		{
			if(page.empty() || page.size() > NAWSERVER_PAGE_ID_BYTES) return false;

			for(const auto character : page)
				if(!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
				    (character >= 'A' && character <= 'Z') || character == '-' || character == '_')) return false;

			return true;
		}

	public:
		explicit ClientIdentity(ClientIdentitySource source = ClientIdentitySource::PeerAddress,
		    const char *page_header = "X-Page-Id", std::string_view page_parameter = "page_id")
		  : source_(source), page_header_(page_header), page_parameter_(page_parameter)
		{
		}

		HeaderReadResult resolve(const HeaderContext &context, char *destination, std::size_t capacity) const
		{
			std::array<char, NAWSERVER_PAGE_ID_BYTES + 32> key{};
			auto *cursor = key.data();
			const auto end = key.data() + key.size();
			const auto peer = context.request.peerAddress();
			const bool needs_peer = source_ != ClientIdentitySource::AuthenticatedUser;
			if(needs_peer)
			{
				if(!peer) return {HeaderReadStatus::InvalidArgument, 0};

				*cursor++ = 'i';
				const auto number = std::to_chars(cursor, end, *peer, 16);
				if(number.ec != std::errc{}) return {HeaderReadStatus::InvalidArgument, 0};

				cursor = number.ptr;
			}

			if(source_ == ClientIdentitySource::AuthenticatedUser || source_ == ClientIdentitySource::PeerAndUser)
			{
#if NAWSERVER_AUTH
				if(!context.principal.authenticated) return {HeaderReadStatus::InvalidArgument, 0};

				*cursor++ = ':';
				*cursor++ = 'u';
				const auto number = std::to_chars(cursor, end, context.principal.user_id, 16);
				if(number.ec != std::errc{}) return {HeaderReadStatus::InvalidArgument, 0};

				cursor = number.ptr;
#else
				return {HeaderReadStatus::InvalidArgument, 0};
#endif
			}

			if(source_ == ClientIdentitySource::Page)
			{
				std::array<char, NAWSERVER_PAGE_ID_BYTES + 1> header{};
				if(!page_header_ || page_parameter_.empty()) return {HeaderReadStatus::InvalidArgument, 0};

				const auto field = context.request.readHeader(page_header_, header.data(), header.size());
				if(field.status != HeaderReadStatus::Ok && field.status != HeaderReadStatus::NotFound) return field;

				std::string_view page = field.status == HeaderReadStatus::Ok ?
				    std::string_view(header.data(), field.size) : std::string_view{};
				bool query_seen = false;
				auto query = context.request.rawQuery();
				while(!query.empty())
				{
					const auto separator = query.find('&');
					const auto pair = query.substr(0, separator);
					const auto equals = pair.find('=');
					if(pair.substr(0, equals) == page_parameter_)
					{
						if(query_seen || equals == std::string_view::npos)
							return {HeaderReadStatus::InvalidArgument, 0};

						query_seen = true;
						const auto value = pair.substr(equals + 1);
						if(field.status == HeaderReadStatus::Ok && page != value)
							return {HeaderReadStatus::InvalidArgument, 0};

						page = value;
					}

					if(separator == std::string_view::npos) break;

					query.remove_prefix(separator + 1);
				}
				if(field.status == HeaderReadStatus::Ok || query_seen)
				{
					if(!safePage(page)) return {HeaderReadStatus::InvalidArgument, 0};

					*cursor++ = ':';
					*cursor++ = 'p';
					std::memcpy(cursor, page.data(), page.size());
					cursor += page.size();
				}

				// Before JS creates a page label, bootstrap/static requests share
				// the peer identity. They do not invent another persistent page.
			}

			const auto size = static_cast<std::size_t>(cursor - key.data());
			if(!destination || capacity <= size) return {HeaderReadStatus::BufferTooSmall, size};

			std::memcpy(destination, key.data(), size);
			destination[size] = '\0';
			return {HeaderReadStatus::Ok, size};
		}

		static HeaderReadResult callback(void *context, const HeaderContext &request, char *destination,
		    std::size_t capacity)
		{
			if(!context) return {HeaderReadStatus::InvalidArgument, 0};

			return static_cast<const ClientIdentity *>(context)->resolve(request, destination, capacity);
		}

	};

#endif
} // namespace NaWServer
