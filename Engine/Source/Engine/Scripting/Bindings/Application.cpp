#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

namespace Engine {

	namespace Utils {

		static int ApplicationQuit(ScriptCall& call)
		{
			const int32_t code = Lua::IsNoneOrNil(call, 1) ? 0 : Lua::Check<int32_t>(call, 1);
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			call.Engine->GetHost().RequestQuit(code);
			return 0;
		}

		static int ApplicationEditor(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().IsEditor);
			return 1;
		}

		static int ApplicationHeadless(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().IsHeadless);
			return 1;
		}

		static int ApplicationFocused(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().IsFocused);
			return 1;
		}

		static int ApplicationPlatform(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().Platform);
			return 1;
		}

		static int ApplicationVersion(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().Version);
			return 1;
		}

		static int ApplicationWindow(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().WindowSize);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterApplication(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Application", "Application identity, window state and deferred quit requests.")
				.Function("Quit", &Utils::ApplicationQuit, "(exitCode: number?) -> ()", "Requests exit (default 0): stops editor Play, exits Runtime, or records quit and ends the current test suite. Never destroys the executing VM inline.")
				.Function("IsEditor", &Utils::ApplicationEditor, "() -> boolean", "True in the editor; false in the standalone Runtime.", read)
				.Function("IsHeadless", &Utils::ApplicationHeadless, "() -> boolean", "Returns whether the host has no native window.", read)
				.Function("IsFocused", &Utils::ApplicationFocused, "() -> boolean", "Returns the current native-window focus state.", read)
				.Function("GetPlatform", &Utils::ApplicationPlatform, "() -> string", "Returns the host platform name.", read)
				.Function("GetVersion", &Utils::ApplicationVersion, "() -> string", "Returns the running engine version.", read)
				.Function("GetWindowSize", &Utils::ApplicationWindow, "() -> vector", "Returns window coordinates as vector(width, height, 0).", read);
			return {};
		}

	}

}
