#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

// Assertions (Architecture §4.5, CodeStyle §11).
//
//   Macro                                    Debug / Release              Dist
//   ENGINE_CORE_ASSERT(cond, fmt, ...)       evaluated -> handler         compiled out (never evaluated)
//   ENGINE_CORE_VERIFY(cond, fmt, ...)       evaluated -> handler         evaluated -> handler (fatal, exit 4)
//   ENGINE_UNREACHABLE(fmt, ...)             handler                      handler (fatal, exit 4)
//
// ENGINE_ASSERT and ENGINE_VERIFY are the client spellings (Editor, Runtime) with the same behaviour; their reports
// carry IsClient = true so the default handler logs them to the App logger instead of the Engine logger.
//
// The format string and its arguments follow std::format and are checked at compile time in every configuration,
// Dist included. A compiled-out assert evaluates neither the condition nor the arguments, so neither may have side
// effects. An assert is never the only guard against external input (files, scenes, scripts, automation).
//
// Failure path: the macro formats the message and calls the installed AssertHandler. A handler never returns: the
// default handler and the Tests handler end in FatalError(FatalErrorKind::Assert), which exits with code 4. Should a
// handler return anyway, the process is terminated the same way. The failure functions are deliberately not
// [[noreturn]], so that the safe fallback code CodeStyle §11 requires after ENGINE_CORE_ASSERT(false, ...) and
// ENGINE_UNREACHABLE does not trigger unreachable-code warnings (MSVC C4702).

namespace Engine {

	enum class AssertKind : uint8_t
	{
		Assert,
		Verify,
		Unreachable
	};

	// One failed assertion. The views point into storage owned by the failure path and are valid only for the duration
	// of the handler call.
	struct AssertInfo
	{
		AssertKind Kind = AssertKind::Assert;
		std::string_view Expression; // the condition as written; empty for ENGINE_UNREACHABLE
		std::string_view Message;    // the formatted message
		std::string_view File;
		uint32_t Line = 0;
		std::string_view Function;
		bool IsClient = false; // ENGINE_ASSERT / ENGINE_VERIFY rather than the ENGINE_CORE_ spellings
	};

	// Called on a failed assertion, on the thread that failed. It must not return (see above), must not throw and must
	// be safe to call from any thread.
	using AssertHandler = void (*)(const AssertInfo& info);

	// Installs `handler` as the process-wide assert handler (process-level state, Architecture §3 rule 5) and returns
	// the previous one. nullptr restores DefaultAssertHandler. Thread-safe; the handler in effect when an assertion
	// fails is the one that runs. ProcessContext (M2) installs the handler that also breaks into an attached debugger
	// and writes a crash report (§4.13); the Tests main installs the recording handler (Tests/Source/Support).
	AssertHandler SetAssertHandler(AssertHandler handler);

	// The handler currently installed (DefaultAssertHandler when none was set).
	[[nodiscard]] AssertHandler GetAssertHandler();

	// Logs FormatAssertInfo(info) at Critical level (Engine logger, or App logger when IsClient), then calls
	// FatalError(FatalErrorKind::Assert, ...), which flushes the logs and exits with code 4.
	void DefaultAssertHandler(const AssertInfo& info);

	// The single-line description every handler reports, so death tests can match a stable substring:
	//     "Assertion failed: <expression>: <message> (<file>:<line>, <function>)"
	//     "Verify failed: <expression>: <message> (<file>:<line>, <function>)"
	//     "Unreachable code reached: <message> (<file>:<line>, <function>)"
	// An empty message drops ": <message>".
	[[nodiscard]] std::string FormatAssertInfo(const AssertInfo& info);

	// "Assert", "Verify" or "Unreachable".
	[[nodiscard]] std::string_view AssertKindToString(AssertKind kind);

	namespace Detail {

		// The failure path behind the macros: builds an AssertInfo and calls the installed handler. If the handler
		// returns, it calls FatalError(FatalErrorKind::Assert, ...). Never returns in practice; not [[noreturn]] (see
		// the top of this file).
		void ReportAssertFailure(AssertKind kind, bool isClient, std::string_view expression, std::string_view message,
			const std::source_location& location);

		template<typename... Args>
		void AssertFailed(AssertKind kind, bool isClient, std::string_view expression, const std::source_location& location,
			std::format_string<Args...> format, Args&&... args)
		{
			ReportAssertFailure(kind, isClient, expression, std::format(format, std::forward<Args>(args)...), location);
		}

		// Never called: it only lets a compiled-out assert type-check its format string and arguments.
		template<typename... Args>
		constexpr void DiscardAssertArguments(std::format_string<Args...> /*format*/, Args&&... /*args*/)
		{
		}

	}

}

#define ENGINE_ASSERT_IMPL(kind, isClient, condition, ...) \
	do \
	{ \
		if (!(condition)) [[unlikely]] \
			::Engine::Detail::AssertFailed(kind, isClient, #condition, ::std::source_location::current(), __VA_ARGS__); \
	} while (false)

// Type-checks the condition and the message without evaluating either (the discarded branch is never executed).
#define ENGINE_ASSERT_DISCARD(condition, ...) \
	do \
	{ \
		if constexpr (false) \
		{ \
			static_cast<void>(!(condition)); \
			::Engine::Detail::DiscardAssertArguments(__VA_ARGS__); \
		} \
	} while (false)

#if defined(ENGINE_DIST)
	#define ENGINE_CORE_ASSERT(condition, ...) ENGINE_ASSERT_DISCARD(condition, __VA_ARGS__)
	#define ENGINE_ASSERT(condition, ...)      ENGINE_ASSERT_DISCARD(condition, __VA_ARGS__)
#else
	#define ENGINE_CORE_ASSERT(condition, ...) ENGINE_ASSERT_IMPL(::Engine::AssertKind::Assert, false, condition, __VA_ARGS__)
	#define ENGINE_ASSERT(condition, ...)      ENGINE_ASSERT_IMPL(::Engine::AssertKind::Assert, true, condition, __VA_ARGS__)
#endif

#define ENGINE_CORE_VERIFY(condition, ...) ENGINE_ASSERT_IMPL(::Engine::AssertKind::Verify, false, condition, __VA_ARGS__)
#define ENGINE_VERIFY(condition, ...)      ENGINE_ASSERT_IMPL(::Engine::AssertKind::Verify, true, condition, __VA_ARGS__)

#define ENGINE_UNREACHABLE(...) \
	::Engine::Detail::AssertFailed(::Engine::AssertKind::Unreachable, false, ::std::string_view(), \
		::std::source_location::current(), __VA_ARGS__)
