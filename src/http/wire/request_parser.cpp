// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include "http/wire/request_parser.h"
#include "detail/ascii.h"
#include "http/header_validation.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace NaWServer::http::wire
{

	namespace
	{

		HttpMethod parseMethod(std::string_view method)
		{
			if(method == "GET")
				return HttpMethod::Get;

			if(method == "POST")
				return HttpMethod::Post;

			if(method == "PUT")
				return HttpMethod::Put;

			if(method == "DELETE")
				return HttpMethod::Delete;

			if(method == "PATCH")
				return HttpMethod::Patch;

			if(method == "OPTIONS")
				return HttpMethod::Options;

			if(method == "HEAD")
				return HttpMethod::Head;

			return HttpMethod::Unknown;
		}

		bool validVersion(std::string_view version)
		{
			// RFC's HTTP-version grammar has one digit on either side of the dot.
			return version.size() == 8 && version.substr(0, 5) == "HTTP/" &&
			       version[5] >= '0' && version[5] <= '9' && version[6] == '.' &&
			       version[7] >= '0' && version[7] <= '9';
		}

		bool parseLength(std::string_view text, std::size_t &length)
		{
			if(text.empty())
				return false;

			std::size_t parsed = 0;
			for(unsigned char character : text)
			{
				if(character < '0' || character > '9')
					return false;

				const std::size_t digit = character - '0';
				if(parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10)
					return false;

				parsed = parsed * 10 + digit;
			}

			length = parsed;
			return true;
		}

	} // namespace

	RequestParser::RequestParser(char *storage, std::size_t storage_capacity,
	    HeaderEntry *entries, std::size_t entry_capacity, const HttpLimits &limits)
	  : storage_(storage), storage_capacity_(storage_capacity), entries_(entries),
	    entry_capacity_(entry_capacity), limits_(limits)
	{
	}

	bool RequestParser::validConfiguration() const
	{
		return storage_ && limits_.max_header_bytes > 0 && limits_.max_header_bytes <= storage_capacity_ &&
		       limits_.max_target_size > 0 && limits_.max_headers <= entry_capacity_ &&
		       (limits_.max_headers == 0 || entries_);
	}

	ParseResult RequestParser::reject(int status)
	{
		result_ = {ParseStatus::Reject, status, result_.head_bytes};
		return result_;
	}

	ParseResult RequestParser::advance(std::size_t used)
	{
		if(!validConfiguration() || used > storage_capacity_ || used < used_)
		{
			result_ = {ParseStatus::InvalidState, 0, 0};
			return result_;
		}

		used_ = used;
		if(result_.state != ParseStatus::NeedMore)
			return result_;

		const std::size_t limit = std::min(used, limits_.max_header_bytes);
		while(scanned_ < limit)
		{
			const std::size_t index = scanned_++;
			const unsigned char character = static_cast<unsigned char>(storage_[index]);
			if(character == 0 || (character == '\n' && (index == 0 || storage_[index - 1] != '\r')) ||
			    (index > 0 && storage_[index - 1] == '\r' && character != '\n'))
			{
				return reject(400);
			}

			if(index >= 3 && storage_[index - 3] == '\r' && storage_[index - 2] == '\n' &&
			    storage_[index - 1] == '\r' && character == '\n')
			{
				return parseHead(index + 1);
			}
		}

		if(used >= limits_.max_header_bytes)
			return reject(431);

		return result_;
	}

	ParseResult RequestParser::parseHead(std::size_t head_bytes)
	{
		result_.head_bytes = head_bytes;
		const std::string_view bytes(storage_, head_bytes);
		const std::size_t first_end = bytes.find("\r\n");
		const std::string_view line = bytes.substr(0, first_end);
		const std::size_t first_space = line.find(' ');
		if(first_space == line.npos)
			return reject(400);

		const std::size_t second_space = line.find(' ', first_space + 1);
		if(second_space == line.npos || line.find(' ', second_space + 1) != line.npos)
			return reject(400);

		const std::string_view method = line.substr(0, first_space);
		const std::string_view version = line.substr(second_space + 1);
		if(!NaWServer::detail::isValidHeaderName(method) || !validVersion(version))
			return reject(400);

		if(version != "HTTP/1.1" && version != "HTTP/1.0")
			return reject(505);

		http11_ = version == "HTTP/1.1";
		head_.method = parseMethod(method);
		head_.target = line.substr(first_space + 1, second_space - first_space - 1);
		if(head_.target.empty() || head_.target.front() != '/' || head_.target.find('#') != head_.target.npos)
			return reject(400);

		if(head_.target.size() > limits_.max_target_size)
			return reject(414);

		for(unsigned char character : head_.target)
			if(character <= 32 || character == 127)
				return reject(400);

		bool length_seen = false;
		bool host_seen = false;
		bool transfer_seen = false;
		bool expect_seen = false;
		std::size_t cursor = first_end + 2;
		while(cursor < head_bytes - 2)
		{
			const std::size_t end = bytes.find("\r\n", cursor);
			const std::string_view field = bytes.substr(cursor, end - cursor);
			const std::size_t colon = field.find(':');
			if(colon == field.npos)
				return reject(400);

			const std::string_view field_name = field.substr(0, colon);
			std::string_view field_value = field.substr(colon + 1);
			while(!field_value.empty() && (field_value.front() == ' ' || field_value.front() == '\t'))
				field_value.remove_prefix(1);

			while(!field_value.empty() && (field_value.back() == ' ' || field_value.back() == '\t'))
				field_value.remove_suffix(1);

			if(!NaWServer::detail::isValidHeaderName(field_name) ||
			    !NaWServer::detail::isValidHeaderValue(field_value))
			{
				return reject(400);
			}

			if(header_count_ == limits_.max_headers)
				return reject(431);

			if(NaWServer::detail::asciiEqualIgnoreCase(field_name, "Content-Length"))
			{
				if(length_seen || !parseLength(field_value, head_.content_length))
					return reject(400);

				length_seen = true;
			}

			if(NaWServer::detail::asciiEqualIgnoreCase(field_name, "Host"))
			{
				if(host_seen || field_value.empty())
					return reject(400);

				host_seen = true;
			}

			if(NaWServer::detail::asciiEqualIgnoreCase(field_name, "Transfer-Encoding"))
				transfer_seen = true;

			if(NaWServer::detail::asciiEqualIgnoreCase(field_name, "Expect"))
				expect_seen = true;

			entries_[header_count_++] = {cursor, field_name.size(),
				                         static_cast<std::size_t>(field_value.data() - storage_), field_value.size()};
			cursor = end + 2;
		}

		if(http11_ && !host_seen)
			return reject(400);

		if(transfer_seen)
			return reject(length_seen ? 400 : 501);

		if(expect_seen)
			return reject(417);

		if(head_.content_length > limits_.max_body_size)
			return reject(400);

		result_.state = ParseStatus::Complete;
		return result_;
	}

	void RequestParser::reset()
	{
		used_ = 0;
		scanned_ = 0;
		header_count_ = 0;
		head_ = {HttpMethod::Unknown, {}, 0};
		http11_ = false;
		result_ = {};
	}

	HttpRequestHead RequestParser::head() const
	{
		return head_;
	}

	std::string_view RequestParser::rawHead() const
	{
		if(result_.state != ParseStatus::Complete)
			return {};

		return {storage_, result_.head_bytes};
	}

	bool RequestParser::http11() const
	{
		return http11_;
	}

	std::string_view RequestParser::name(const HeaderEntry &entry) const
	{
		return {storage_ + entry.name_offset, entry.name_size};
	}

	std::string_view RequestParser::value(const HeaderEntry &entry) const
	{
		return {storage_ + entry.value_offset, entry.value_size};
	}

	std::optional<std::string_view> RequestParser::headerValue(std::string_view requested_name) const
	{
		for(std::size_t index = 0; index < header_count_; ++index)
			if(NaWServer::detail::asciiEqualIgnoreCase(name(entries_[index]), requested_name))
				return value(entries_[index]);

		return std::nullopt;
	}

	std::size_t RequestParser::headerCount(std::string_view requested_name) const
	{
		std::size_t count = 0;
		for(std::size_t index = 0; index < header_count_; ++index)
			if(NaWServer::detail::asciiEqualIgnoreCase(name(entries_[index]), requested_name))
				++count;

		return count;
	}

	std::size_t RequestParser::headerCount() const
	{
		return header_count_;
	}

	HeaderReadResult RequestParser::readHeader(const char *requested_name, char *destination,
	    std::size_t capacity) const
	{
		if(!requested_name || !*requested_name || (!destination && capacity != 0))
			return {HeaderReadStatus::InvalidArgument, 0};

		// Bounded singular reads must not silently choose one credential/identity.
		// List-valued protocol fields use headerValue/headerCount explicitly.
		if(headerCount(requested_name) > 1)
			return {HeaderReadStatus::InvalidArgument, 0};

		const auto field = headerValue(requested_name);
		if(!field)
			return {HeaderReadStatus::NotFound, 0};

		if(capacity <= field->size())
			return {HeaderReadStatus::BufferTooSmall, field->size()};

		if(!field->empty())
			std::memcpy(destination, field->data(), field->size());

		destination[field->size()] = '\0';
		return {HeaderReadStatus::Ok, field->size()};
	}

} // namespace NaWServer::http::wire
