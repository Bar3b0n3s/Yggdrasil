#include "EnginePCH.h"
#include "Engine/Core/Assert.h"

#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"

#include <iterator>

namespace Engine {

	// The installed handler (process-level state, Architecture §3 rule 5). Constant-initialized, so an assertion that
	// fails during static initialization already reaches the default handler.
	static std::atomic<AssertHandler> s_AssertHandler{ &DefaultAssertHandler };
	// Set while this thread reports an assertion, so that an assertion failing inside a handler (or inside the logging it
	// does) ends the process instead of recursing.
	static thread_local bool s_IsReportingAssertion = false;

	namespace Utils {

		static std::string_view GetFailurePrefix(AssertKind kind)
		{
			switch (kind)
			{
				case AssertKind::Assert:      return "Assertion failed";
				case AssertKind::Verify:      return "Verify failed";
				case AssertKind::Unreachable: return "Unreachable code reached";
			}

			// No assert here: this runs while an assertion is being reported, and the kind came from the macros.
			return "Assertion failed";
		}

	}

	AssertHandler SetAssertHandler(AssertHandler handler)
	{
		return s_AssertHandler.exchange(handler != nullptr ? handler : &DefaultAssertHandler);
	}

	AssertHandler GetAssertHandler()
	{
		return s_AssertHandler.load();
	}

	void DefaultAssertHandler(const AssertInfo& info)
	{
		const std::string description = FormatAssertInfo(info);

		// The entry carries the location of the failed assertion, not of this function. AssertInfo holds views, so the
		// strings handed to spdlog as C strings are copied first.
		const std::string file(info.File);
		const std::string function(info.Function);
		const spdlog::source_loc location{ file.c_str(), static_cast<int>(info.Line), function.c_str() };
		Log::GetLogger(info.IsClient ? LogChannel::App : LogChannel::Engine).log(location, spdlog::level::critical, "{}", description);

		FatalError(FatalErrorKind::Assert, description);
	}

	std::string FormatAssertInfo(const AssertInfo& info)
	{
		std::string text(Utils::GetFailurePrefix(info.Kind));

		// ENGINE_UNREACHABLE has no condition; any expression is ignored for it.
		if (info.Kind != AssertKind::Unreachable && !info.Expression.empty())
		{
			text += ": ";
			text += info.Expression;
		}
		if (!info.Message.empty())
		{
			text += ": ";
			text += info.Message;
		}

		std::format_to(std::back_inserter(text), " ({}:{}", info.File, info.Line);
		if (!info.Function.empty())
		{
			text += ", ";
			text += info.Function;
		}
		text += ')';
		return text;
	}

	std::string_view AssertKindToString(AssertKind kind)
	{
		switch (kind)
		{
			case AssertKind::Assert:      return "Assert";
			case AssertKind::Verify:      return "Verify";
			case AssertKind::Unreachable: return "Unreachable";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AssertKind {}", std::to_underlying(kind));
		return "Unknown";
	}

	namespace Detail {

		void ReportAssertFailure(AssertKind kind, bool isClient, std::string_view expression, std::string_view message,
			const std::source_location& location)
		{
			AssertInfo info;
			info.Kind = kind;
			info.Expression = expression;
			info.Message = message;
			info.File = location.file_name();
			info.Line = location.line();
			info.Function = location.function_name();
			info.IsClient = isClient;

			if (s_IsReportingAssertion)
				FatalError(FatalErrorKind::Assert, std::format("{} (while reporting another assertion)", FormatAssertInfo(info)));
			s_IsReportingAssertion = true;

			GetAssertHandler()(info);

			// A handler must not return (Assert.h); one that does still ends the process the documented way.
			FatalError(FatalErrorKind::Assert, FormatAssertInfo(info));
		}

	}

}
