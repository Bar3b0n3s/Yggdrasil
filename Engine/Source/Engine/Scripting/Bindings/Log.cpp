#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/LogContext.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <limits>

namespace Engine {

	namespace Utils {

		template<LogLevel Level>
		static int ScriptLog(ScriptCall& call)
		{
			if constexpr (Level == LogLevel::Trace && !Detail::LogTraceEnabled)
				return 0;
			std::string message;
			const int count = Lua::GetArgumentCount(call);
			for (int index = 1; index <= count; ++index)
			{
				size_t length = 0;
				const char* text = luaL_tolstring(call.State, index, &length);
				if (index != 1)
					message += '\t';
				message.append(text, length);
				lua_pop(call.State, 1);
			}
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
			if constexpr (Level == LogLevel::Info)
				Detail::SandboxAccess::CapturePrint(*call.Sandbox, message);
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterLog(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Log", "Script-channel logging with authored source, line, entity and simulation tick; arguments use tostring and tab separators.")
				.Function("Trace", &Utils::ScriptLog<LogLevel::Trace>, "(...any) -> ()", "Writes a Trace entry; compiled out of Dist.", read)
				.Function("Info", &Utils::ScriptLog<LogLevel::Info>, "(...any) -> ()", "Writes an Info entry and captures evaluation output. The print global aliases this registered function.", read)
				.Function("Warn", &Utils::ScriptLog<LogLevel::Warn>, "(...any) -> ()", "Writes a Warn entry without raising a script error.", read)
				.Function("Error", &Utils::ScriptLog<LogLevel::Error>, "(...any) -> ()", "Writes an Error entry without throwing or disabling the script instance.", read);
			return {};
		}

	}

}
