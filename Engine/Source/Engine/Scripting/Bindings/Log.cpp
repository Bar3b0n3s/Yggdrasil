#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/LogContext.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <algorithm>
#include <format>
#include <limits>
#include <string>

namespace Engine {

	namespace Utils {

		// Admits one entry into this VM's per-frame window (ScriptLimits.h). A new (tick, frame) window first reports how many
		// entries the previous window dropped; the first entry dropped in a window warns once.
		static bool AdmitScriptLogEntry(ScriptCall& call)
		{
			const auto frame = call.Engine->GetHost().GetFrameState();
			auto& window = Detail::SandboxAccess::GetLogWindow(*call.Sandbox);
			if (!window.Started || window.Tick != frame.Tick || window.Frame != frame.Frame)
			{
				Detail::SandboxAccess::ReportDroppedLogEntries(window);
				window = { .Tick = frame.Tick, .Frame = frame.Frame, .Started = true };
			}
			if (window.Entries < MaxScriptLogEntriesPerFrame)
			{
				++window.Entries;
				return true;
			}
			if (window.Dropped++ == 0)
				Detail::LogWrite(LogChannel::Script, LogLevel::Warn, "", 0, "", "script log limit of {} entries per frame reached in tick {} frame {}; further entries in this frame are dropped", MaxScriptLogEntriesPerFrame, frame.Tick, frame.Frame);
			return false;
		}

		template<LogLevel Level>
		static int ScriptLog(ScriptCall& call)
		{
			if constexpr (Level == LogLevel::Trace && !Detail::LogTraceEnabled)
				return 0;
			// Bounded assembly: one VM string aliased by many arguments cannot build an unbounded host message (F16), and the
			// cap holds after private locations are rewritten.
			const std::string message = Detail::SandboxAccess::FormatPrintArguments(*call.Sandbox, call.State);
			if constexpr (Level == LogLevel::Info)
				Detail::SandboxAccess::CapturePrint(*call.Sandbox, message);
			if (!AdmitScriptLogEntry(call))
				return 0;
			const auto location = Detail::ScriptCallerLocation(call);
			const auto execution = Detail::SandboxAccess::GetContext(*call.Sandbox);
			LogContext context = LogContextScope::GetCurrent();
			context.Entity = execution.Entity;
			context.Tick = call.Engine->GetHost().GetFrameState().Tick;
			context.ScriptFile = location.File;
			context.ScriptLine = location.Line;
			const LogContextScope scope(context);
			const auto line = static_cast<int>(std::min(location.Line, static_cast<uint32_t>(std::numeric_limits<int>::max())));
			Detail::LogWrite(LogChannel::Script, Level, location.File.c_str(), line, "", "{}", message);
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterLog(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			const std::string description = std::format("Script-channel logging with authored source, line, entity and simulation tick; arguments use tostring and tab separators. "
														"An entry keeps at most {} bytes (longer text is truncated and marked), and one VM writes at most {} entries per frame, dropping and counting the rest.",
				MaxScriptLogMessageBytes, MaxScriptLogEntriesPerFrame);
			api.Module("Log", description)
				.Function("Trace", &Utils::ScriptLog<LogLevel::Trace>, "(...any) -> ()", "Writes a Trace entry; compiled out of Dist.", read)
				.Function("Info", &Utils::ScriptLog<LogLevel::Info>, "(...any) -> ()", "Writes an Info entry and captures evaluation output. The print global aliases this registered function.", read)
				.Function("Warn", &Utils::ScriptLog<LogLevel::Warn>, "(...any) -> ()", "Writes a Warn entry without raising a script error.", read)
				.Function("Error", &Utils::ScriptLog<LogLevel::Error>, "(...any) -> ()", "Writes an Error entry without throwing or disabling the script instance.", read);
			return {};
		}

	}

}
