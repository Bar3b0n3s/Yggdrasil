#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"

#include <expected>
#include <format>
#include <string>
#include <utility>

namespace Engine {

	// The return type of every operation that can fail in an expected way (Architecture §4.6). Every function returning
	// a Result or Status is [[nodiscard]].
	template<typename T = void>
	using Result = std::expected<T, Error>;

	using Status = Result<void>;

	// An error to return from a Result- or Status-returning function, with a std::format message checked at compile time:
	//     return MakeError(ErrorCode::NotFound, "no file '{}'", path.ToString());
	template<typename... Args>
	[[nodiscard]] std::unexpected<Error> MakeError(ErrorCode code, std::format_string<Args...> format, Args&&... args)
	{
		return std::unexpected<Error>(std::in_place, code, std::format(format, std::forward<Args>(args)...));
	}

	// Returns `result` unchanged on success, or with `context` appended to its error (Error::WithContext). Lets a
	// propagation site describe what it was doing:
	//     ENGINE_TRY(WithContext(vfs.ReadText(path), std::format("while loading '{}'", path.ToString())));
	template<typename T>
	[[nodiscard]] Result<T> WithContext(Result<T> result, std::string context)
	{
		if (!result)
			return std::unexpected(std::move(result).error().WithContext(std::move(context)));
		return result;
	}

}

// Evaluates `expression` (a Result or Status) once; on failure returns its error from the enclosing function, whose
// return type must be a Result or Status (of any value type). A statement; usable anywhere a statement is.
#define ENGINE_TRY(expression) \
	do \
	{ \
		auto&& engineTryResult = (expression); \
		if (!engineTryResult) [[unlikely]] \
			return ::std::unexpected(::std::move(engineTryResult).error()); \
	} while (false)

// Evaluates `expression` (a non-void Result) once; on failure returns its error from the enclosing function, otherwise
// declares `declaration` from the moved value:
//     ENGINE_TRY_ASSIGN(Buffer bytes, vfs.ReadFile(path));
//     ENGINE_TRY_ASSIGN(const auto count, reader.ReadU32());
// It expands to several statements in the enclosing block (the declaration must stay visible), so it cannot be the
// unbraced body of an if, for or while. Pass a prvalue: an lvalue Result is moved from.
#define ENGINE_TRY_ASSIGN(declaration, expression) \
	auto&& ENGINE_CONCAT(engineTryAssign, __LINE__) = (expression); \
	if (!ENGINE_CONCAT(engineTryAssign, __LINE__)) [[unlikely]] \
		return ::std::unexpected(::std::move(ENGINE_CONCAT(engineTryAssign, __LINE__)).error()); \
	declaration = ::std::move(*ENGINE_CONCAT(engineTryAssign, __LINE__))
