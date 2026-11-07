# Source code style

## Automatic formatting

Use Uncrustify 0.83.0_f with the root `uncrustify.cfg`.
It uses tabs with width 4, preserves existing single-line `if` statements,
does not join multiline `if` statements and adds a blank line between checks.
Leave one blank line between method and function definitions
(`nl_after_func_body = 2`: two newline characters).
Definitions inside classes and single-line bodies also use
`nl_after_func_body_class = 2` and `nl_after_func_body_one_liner = 2`.
Checked on a copy of `MemoryExchange`: methods are separated by blank lines.
Uncrustify 0.83.0_f limitation: when expanding the nested single-line `Reset`
destructor in `memory.cpp`, the blank line after it may appear only on a second pass.
The single-line, multiline and mixed `digit()` samples and both examples below
were checked; a second pass leaves the output unchanged.

From the repository root in PowerShell:

```powershell
.\uncrustify-check.cmd
.\uncrustify-apply.cmd
```

The first command only checks formatting. The second changes files.
Both find `git.exe` and `uncrustify.exe` through PATH; no machine paths are in the scripts.
Only Git-tracked C/C++ files are processed. Deleted files, generated `assets.hpp`
and vendor/third_party directories are skipped. New untracked files are excluded.
Neither command creates a commit. Apply does not create backup or MD5 files.
The root `.gitattributes` checks maintained C/C++ out with LF, including on
Windows with `core.autocrlf=true`. Vendor/generated/font sources and `assets.hpp`
are excluded from this policy as well as from formatting. A fresh checkout must
pass Check without running Apply first. Do not broadly renormalize vendor files.
Exit codes: 0 means success, 1 means formatting differences during a check or
file processing errors, and 2 means setup/argument errors. Checks continue after
differences and report the total failure count.

Install the official Uncrustify 0.83.0_f executable and add its directory to PATH.
Open a new terminal after changing PATH; restart the editor for its integrated terminal.
Do not run apply unless the owner requests source formatting.

The previous clang-format configuration and scripts are parked in ignored
`local/clang-format-archive/`. They are not active or included in Git. To reuse them,
restore them to the repository root first; their paths assume that location.
Trial samples and outputs remain in ignored `local/formatting-trial/`.

Read this file before writing or formatting source code. Examples and notes
define the formatting style for the indicated language and scope. The initial
examples are an editable starting point for the owner; follow owner edits here.
Formatting alone must not change logic or identifiers.
Do not reformat the whole repository unless explicitly requested.

## Example 1: declarations, classes and headers

Scope: C++ declarations in project headers.

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace NaWServer
{

	// The buffer is borrowed and must outlive this object.
	class MessageBuffer
	{
	public:
		MessageBuffer(std::uint8_t *storage, std::size_t capacity);

		bool append(const std::uint8_t *data, std::size_t size);
		std::size_t size() const;

	private:
		std::uint8_t *storage_;
		std::size_t capacity_;
		std::size_t size_ = 0;
	};

} // namespace NaWServer
```

Notes (edit these together with the example):

- Use tabs for indentation, displayed as 4 spaces. Spaces may be used for alignment within a line.
- Put opening braces on a new line. Align public/private with the class braces.
- Put pointer and reference symbols next to the name: `const Request &request`, `std::uint8_t *data`.
- Do not add extra spaces to align declarations in columns.

## Example 2: functions, conditions and line breaks

Scope: C++ function definitions and control flow.

```cpp
MessageBuffer::MessageBuffer(std::uint8_t *storage, std::size_t capacity)
  : storage_(storage), capacity_(capacity)
{
}

bool MessageBuffer::append(const std::uint8_t *data, std::size_t size)
{
	if(data == nullptr)
		return false;

	if(data == nullptr || storage_ == nullptr || size > capacity_ - size_)
	{
		return false;
	}

	for(std::size_t index = 0; index < size; ++index)
	{
		storage_[size_ + index] = data[index];
	}

	size_ += size;
	return true;
}

std::size_t MessageBuffer::size() const
{
	return size_;
}

// For a long call, put each argument on its own line.
const bool published = publish_device_status(
	device_identifier,
	current_connection_count,
	available_memory_bytes,
	last_rejection_reason);
```

Notes:

- Do not add a space after if/for/while or inside parentheses.
- Put spaces around operators.
- Put a space after each semicolon inside parentheses.
- Use braces for if/else/for/while blocks, even with one statement. You may omit braces for a simple condition with a simple action, as shown above.
- Put one statement on each line, except existing single-line `if` statements with
  a simple action: preserve them. Do not join multiline `if` statements.
  Use multiple lines even for short functions.
- Keep lines within 120 characters. Split long calls as shown above.
- Separate logical steps with one blank line. Put comments before the related code.

These examples define formatting only. They do not introduce APIs or naming rules.
Keep existing names and required SDK identifiers.
For other languages, follow their local rules. Do not apply this C++ style automatically.

