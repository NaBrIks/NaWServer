// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/request.h>
#include <cstring>
namespace NaWServer
{
	std::string_view Request::path() const
	{
		const auto uri = exchange_.head().target;
		return uri.substr(0, uri.find('?'));
	}

	std::string_view Request::rawQuery() const
	{
		const auto uri = exchange_.head().target;
		const auto offset = uri.find('?');
		return offset == uri.npos ? std::string_view{} : uri.substr(offset + 1);
	}

	HeaderReadResult Request::readHeader(const char *name, char *destination, std::size_t capacity) const
	{
		if(!name || !*name || (!destination && capacity))
			return {HeaderReadStatus::InvalidArgument, 0};

		return exchange_.readHeader(name, destination, capacity);
	}

	std::optional<std::string> Request::header(const char *name) const
	{
		if(!name || !*name)
			return std::nullopt;

		const auto needed = readHeader(name, nullptr, 0);
		if(needed.status != HeaderReadStatus::BufferTooSmall)
			return std::nullopt;

		std::vector<char> buffer(needed.size + 1);
		if(readHeader(name, buffer.data(), buffer.size()).status != HeaderReadStatus::Ok)
			return std::nullopt;

		return std::string(buffer.data(), needed.size);
	}

	std::optional<std::string_view> Request::find(const ParameterMap &fields, const std::string &name)
	{
		const auto field = fields.find(name);
		return field == fields.end() ? std::nullopt : std::optional<std::string_view>(field->second.value());
	}

	bool Request::prepareParameters()
	{
		if(parameters_prepared_)
			return parameter_result_.status == UrlDecodeStatus::Ok;

		parameters_prepared_ = true;
		ParameterMap query_fields, form_fields;
		parameter_result_ = parseFormParameters(rawQuery(), query_fields);
		if(parameter_result_.status != UrlDecodeStatus::Ok)
		{
			parameter_source_ = ParameterSource::Query;
			return false;
		}

		const auto type = header("Content-Type");
		if(type && isFormContentType(*type))
		{
			const auto raw = body();
			const std::string_view text = raw.empty() ? std::string_view{}
			                                          : std::string_view(
				reinterpret_cast<const char *>(raw.data()),
				raw.size());
			parameter_result_ = parseFormParameters(text, form_fields);
			if(parameter_result_.status != UrlDecodeStatus::Ok)
			{
				parameter_source_ = ParameterSource::Form;
				return false;
			}
		}

		query_params_.swap(query_fields);
		form_params_.swap(form_fields);
		return true;
	}

} // namespace NaWServer
