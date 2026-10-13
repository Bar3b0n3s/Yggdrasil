#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Platform/Input/KeyCodes.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Scripting/ScriptProxy.h"
#include "Engine/Scripting/ScriptTestHost.h"

#include <lua.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace Engine {

	namespace Utils {

		static IScriptTestHost& Host(ScriptCall& call)
		{
			if (!call.Engine || !call.Engine->IsTestMode() || !call.Engine->GetTestHost())
				Lua::RaiseError(call, "Test is only available in test runs");
			return *call.Engine->GetTestHost();
		}

		static void Checked(ScriptCall& call, Status result)
		{
			if (!result)
				Lua::RaiseError(call, result.error());
		}

		// Reported messages are diagnostics the runner keeps: authored locations, at most MaxScriptLogMessageBytes bytes.
		static std::string Message(ScriptCall& call, int index, std::string_view fallback)
		{
			return Lua::IsNoneOrNil(call, index) ? std::string(fallback) : Lua::CheckDiagnosticText(call, index);
		}

		static int Report(ScriptCall& call, ScriptTestSignal signal, std::string message)
		{
			const auto location = Detail::ScriptCallerLocation(call);
			ScriptTestReport report{ signal, std::move(message), location.File, location.Line };
			if (signal == ScriptTestSignal::ExpectationFailed)
				Host(call).Report(report);
			else
			{
				Checked(call, Detail::ScriptEngineAccess::EndTestCase(call, report));
				if (lua_isyieldable(call.State))
					return lua_yield(call.State, 0);
				return Lua::RaiseError(call, report.Message);
			}
			return 0;
		}

		static int Case(ScriptCall& call)
		{
			static_cast<void>(Host(call));
			const auto name = Lua::CheckBoundedString(call, 1, MaxScriptTestNameBytes);
			if (name.empty() || !lua_isfunction(call.State, 2))
				return Lua::RaiseError(call, "Test.Case needs a nonempty name and a function");
			uint32_t timeout = 0;
			if (!Lua::IsNoneOrNil(call, 3))
			{
				if (!lua_istable(call.State, 3))
					return Lua::RaiseError(call, "Test.Case options must be a table");
				lua_pushnil(call.State);
				while (lua_next(call.State, 3))
				{
					if (lua_type(call.State, -2) != LUA_TSTRING || Lua::Check<std::string>(call, -2) != "TimeoutTicks")
						return Lua::RaiseError(call, "unknown Test.Case option; expected TimeoutTicks");
					timeout = Lua::Check<uint32_t>(call, -1);
					if (timeout == 0)
						return Lua::RaiseError(call, "TimeoutTicks must be a positive integer");
					lua_pop(call.State, 1);
				}
			}
			Checked(call, Detail::ScriptEngineAccess::RegisterTestCase(call, 2, name, timeout, Detail::ScriptCallerLocation(call)));
			return 0;
		}

		static int Expect(ScriptCall& call)
		{
			const bool condition = Lua::Check<bool>(call, 1);
			const auto message = Message(call, 2, "expected condition to be true");
			return condition ? 0 : Report(call, ScriptTestSignal::ExpectationFailed, message);
		}

		static int ExpectEqual(ScriptCall& call)
		{
			if (Lua::GetArgumentCount(call) < 2)
				return Lua::RaiseError(call, "Test.ExpectEqual needs two values");
			const auto message = Message(call, 3, "expected values to be equal");
			return lua_equal(call.State, 1, 2) ? 0 : Report(call, ScriptTestSignal::ExpectationFailed, message);
		}

		static int ExpectNear(ScriptCall& call)
		{
			const double epsilon = Lua::IsNoneOrNil(call, 3) ? 0.00001 : Lua::Check<double>(call, 3);
			if (epsilon < 0)
				return Lua::RaiseError(call, "Test.ExpectNear epsilon must be nonnegative");
			bool near = false;
			if (lua_isvector(call.State, 1) && lua_isvector(call.State, 2))
			{
				const auto a = Lua::Check<glm::vec3>(call, 1), b = Lua::Check<glm::vec3>(call, 2);
				near = std::abs(static_cast<double>(a.x) - b.x) <= epsilon && std::abs(static_cast<double>(a.y) - b.y) <= epsilon && std::abs(static_cast<double>(a.z) - b.z) <= epsilon;
			}
			else
				near = std::abs(Lua::Check<double>(call, 1) - Lua::Check<double>(call, 2)) <= epsilon;
			const auto message = Message(call, 4, "values differ by more than epsilon");
			return near ? 0 : Report(call, ScriptTestSignal::ExpectationFailed, message);
		}

		static int Fail(ScriptCall& call)
		{
			return Report(call, ScriptTestSignal::Fail, Lua::CheckDiagnosticText(call, 1));
		}
		static int Skip(ScriptCall& call)
		{
			return Report(call, ScriptTestSignal::Skip, Lua::CheckDiagnosticText(call, 1));
		}

		static int Wait(ScriptCall& call, Detail::ScriptTestWait request)
		{
			request.Location = Detail::ScriptCallerLocation(call);
			auto armed = Detail::ScriptEngineAccess::ArmTestWait(call, request);
			if (!request.Predicate.IsNull())
				call.Engine->ReleaseReference(request.Predicate);
			if (!armed)
				return Lua::RaiseError(call, armed.error());
			return *armed < 0 ? lua_yield(call.State, 0) : *armed;
		}

		static uint64_t Ticks(ScriptCall& call, int index)
		{
			const double ticks = Lua::Check<double>(call, index);
			if (ticks < 0 || ticks > 9007199254740991.0 || std::floor(ticks) != ticks)
				Lua::RaiseError(call, "wait ticks must be an exactly representable nonnegative integer");
			return static_cast<uint64_t>(ticks);
		}

		static int WaitTicks(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::Ticks;
			request.Ticks = Ticks(call, 1);
			return Wait(call, std::move(request));
		}

		static int WaitUntil(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::Predicate;
			request.Ticks = Ticks(call, 2);
			auto predicate = Detail::ScriptEngineAccess::RetainFunction(call, 1);
			if (!predicate)
				return Lua::RaiseError(call, predicate.error());
			request.Predicate = *predicate;
			return Wait(call, std::move(request));
		}

		static int ExpectScriptError(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::ExpectedError;
			request.Pattern = Lua::CheckBoundedString(call, 1, MaxScriptErrorPatternBytes);
			request.Ticks = Ticks(call, 2);
			return Wait(call, std::move(request));
		}

		static int CaptureAudio(ScriptCall& call)
		{
			Detail::ScriptTestWait request;
			request.Kind = Detail::ScriptWaitKind::Audio;
			request.Ticks = Lua::Check<uint32_t>(call, 1);
			return Wait(call, std::move(request));
		}

		static int Inject(ScriptCall& call, const Json& value)
		{
			Checked(call, call.CheckWritable());
			Checked(call, Host(call).InjectInput(value));
			return 0;
		}

		// The InputEventState enum (Down, Up, Tap), also the automation input vocabulary of §13.6. Registry dispatch has
		// validated and counted an enum argument before a callback runs; these map the value to its canonical spelling.
		constexpr std::array<std::string_view, 3> InputEventStates{ "Down", "Up", "Tap" };

		static std::string_view EventState(ScriptCall& call, int index)
		{
			const int64_t value = Lua::CheckEnum(call, index, "InputEventState");
			if (value < 0 || value >= static_cast<int64_t>(InputEventStates.size()))
				Lua::RaiseError(call, "invalid InputEventState value");
			return InputEventStates[static_cast<size_t>(value)];
		}

		static int InjectAction(ScriptCall& call)
		{
			Json value = { { "type", "action" }, { "name", Lua::Check<std::string>(call, 1) } };
			if (!Lua::IsNoneOrNil(call, 3))
			{
				// The third argument selects the analog overload. Validate the documented state argument even though the
				// canonical input event contains only Value (State and Value are mutually exclusive in the host parser).
				if (!Lua::IsNoneOrNil(call, 2) && EventState(call, 2) != "Down")
					return Lua::RaiseError(call, "analog Test.InjectAction requires Down or nil state");
				value["value"] = Lua::Check<float>(call, 3);
			}
			else
				value["state"] = EventState(call, 2);
			return Inject(call, value);
		}

		static int InjectKey(ScriptCall& call)
		{
			const auto key = KeyToString(static_cast<Key>(Lua::CheckEnum(call, 1, "Key")));
			return Inject(call, { { "type", "key" }, { "key", key }, { "state", EventState(call, 2) } });
		}

		static int InjectMouse(ScriptCall& call)
		{
			const auto button = MouseButtonToString(static_cast<MouseButton>(Lua::CheckEnum(call, 1, "MouseButton")));
			Json value = { { "type", "mouseButton" }, { "button", button }, { "state", EventState(call, 2) } };
			if (!Lua::IsNoneOrNil(call, 3))
			{
				const auto position = Lua::Check<glm::vec2>(call, 3);
				value["position"] = { position.x, position.y };
			}
			return Inject(call, value);
		}

		static int InjectGamepad(ScriptCall& call)
		{
			// The third argument's type selects the overload, and with it the enum of the second (EnumParameters).
			const auto index = Lua::Check<uint32_t>(call, 1);
			if (lua_type(call.State, 3) == LUA_TNUMBER)
			{
				const auto axis = GamepadAxisToString(static_cast<GamepadAxis>(Lua::CheckEnum(call, 2, "GamepadAxis")));
				return Inject(call, { { "type", "gamepadAxis" }, { "gamepad", index }, { "axis", axis }, { "value", Lua::Check<float>(call, 3) } });
			}
			if (lua_type(call.State, 3) != LUA_TSTRING)
				return Lua::RaiseError(call, std::format("argument #3 expected string or number, got {}", lua_typename(call.State, lua_type(call.State, 3))));
			const auto button = GamepadButtonToString(static_cast<GamepadButton>(Lua::CheckEnum(call, 2, "GamepadButton")));
			return Inject(call, { { "type", "gamepadButton" }, { "gamepad", index }, { "button", button }, { "state", EventState(call, 3) } });
		}

		static int Screenshot(ScriptCall& call)
		{
			Checked(call, Host(call).CaptureScreenshot(Lua::Check<std::string>(call, 1)));
			return 0;
		}
		static int ReloadScript(ScriptCall& call)
		{
			const auto asset = Lua::Check<AssetHandle>(call, 1);
			Checked(call, Host(call).ReloadScript(asset));
			return 0;
		}
		static int GetStateHash(ScriptCall& call)
		{
			Lua::PushString(call, UUID(Host(call).ComputeStateHash()).ToString());
			return 1;
		}

		static int GetLastExtraction(ScriptCall& call)
		{
			const auto value = Host(call).GetLastExtraction();
			lua_createtable(call.State, 0, 2);
			Lua::Push(call, value.Alpha);
			lua_setfield(call.State, -2, "Alpha");
			Lua::Push(call, static_cast<double>(value.Frame));
			lua_setfield(call.State, -2, "Frame");
			return 1;
		}

		static int GetExtractedPosition(ScriptCall& call)
		{
			const auto entity = Lua::Check<ScriptEntityIdentity>(call, 1);
			Checked(call, ScriptProxy::ValidateEntity(call.Engine->GetHost(), entity));
			const auto position = Host(call).GetExtractedPosition(entity.ID);
			if (position)
				Lua::Push(call, *position);
			else
				Lua::PushNil(call);
			return 1;
		}

		static int GetScriptErrors(ScriptCall& call)
		{
			uint64_t since = 0;
			if (!Lua::IsNoneOrNil(call, 1))
			{
				const double value = Lua::Check<double>(call, 1);
				if (value < 0 || value > 9007199254740991.0 || std::floor(value) != value)
					return Lua::RaiseError(call, "error cursor must be an exactly representable nonnegative integer");
				since = static_cast<uint64_t>(value);
			}
			const auto errors = call.Engine->GetErrors().Read(since, std::numeric_limits<uint32_t>::max());
			lua_createtable(call.State, 0, 0);
			int index = 1;
			for (const auto& error : errors)
			{
				lua_createtable(call.State, 0, 14);
				const auto string = [&call](const char* name, std::string_view value)
				{
					Lua::PushString(call, value);
					lua_setfield(call.State, -2, name);
				};
				const auto number = [&call](const char* name, uint64_t value)
				{
					Lua::Push(call, static_cast<double>(value));
					lua_setfield(call.State, -2, name);
				};
				number("id", error.ID);
				string("kind", ScriptErrorKindToString(error.Kind));
				string("script", error.Script);
				number("line", error.Line);
				number("column", error.Column);
				string("message", error.Message);
				string("callback", error.Callback);
				string("entity", error.Entity.ToString());
				string("entityName", error.EntityName);
				number("tick", error.Tick);
				number("count", error.Count);
				string("jsonPointer", error.JsonPointer);
				lua_createtable(call.State, 0, 0);
				int frameIndex = 1;
				for (const auto& frame : error.Traceback)
				{
					lua_createtable(call.State, 0, 3);
					string("script", frame.Script);
					number("line", frame.Line);
					string("function", frame.Function);
					lua_rawseti(call.State, -2, frameIndex++);
				}
				lua_setfield(call.State, -2, "traceback");
				lua_rawseti(call.State, -2, index++);
			}
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterTest(ScriptApiRegistry& api)
		{
			ENGINE_TRY(api.RegisterAlias("TestSuiteOptions", "{CaseTimeoutTicks: number?}", "Default positive tick timeout for cases in a suite."));
			ENGINE_TRY(api.RegisterAlias("TestCaseOptions", "{TimeoutTicks: number?}", "Positive tick timeout overriding the suite default."));
			ENGINE_TRY(api.RegisterAlias("TestSuite", "{Name: string, Body: () -> (), Options: TestSuiteOptions}", "Authenticated suite constructor result."));
			ENGINE_TRY(api.RegisterAlias("TestAudioLevels", "{RmsLeft: number, RmsRight: number, Peak: number}", "Stereo RMS and absolute peak of the requested simulation sample interval."));
			ENGINE_TRY(api.RegisterAlias("TestExtraction", "{Alpha: number, Frame: number}", "Last completed render extraction interpolation and frame."));
			ENGINE_TRY(api.RegisterAlias("ScriptError", "{id: number, kind: string, script: string, line: number, column: number, message: string, callback: string, entity: string, entityName: string, tick: number, count: number, jsonPointer: string, traceback: {{script: string, line: number, [\"function\"]: string}}}", "Owned script diagnostic with exclusive occurrence cursor."));
			ScriptMemberOptions pure;
			pure.Environments = ScriptApiEnvironment::Test;
			pure.Mutates = false;
			ScriptMemberOptions mutation = pure;
			mutation.Mutates = true;
			ScriptMemberOptions all = pure;
			all.Environments = ScriptApiEnvironment::All;
			ScriptMemberOptions editor = mutation;
			editor.Modes = RunModes::EditorOnly;
			// String-enum parameters (§11.4): typed in Engine.d.luau, validated before the callback and counted per value.
			// Editor and Runtime registries also reflect the identical automation enum; Freeze merges identical definitions.
			EnumInfo states("InputEventState", "The state of a button-like input event (§13.6).");
			states.AddEntry({ std::string(Utils::InputEventStates[0]), 0, "Pressed on its tick." });
			states.AddEntry({ std::string(Utils::InputEventStates[1]), 1, "Released on its tick." });
			states.AddEntry({ std::string(Utils::InputEventStates[2]), 2, "Pressed on its tick and released on the next, so both edges are observed." });
			ENGINE_TRY(api.RegisterEnum(states));
			ScriptMemberOptions action = mutation;
			action.EnumParameters = { { .ArgumentIndex = 2, .EnumName = "InputEventState", .Optional = true } };
			ScriptMemberOptions key = mutation;
			key.EnumParameters = { { .ArgumentIndex = 1, .EnumName = "Key" }, { .ArgumentIndex = 2, .EnumName = "InputEventState" } };
			ScriptMemberOptions mouse = mutation;
			mouse.EnumParameters = { { .ArgumentIndex = 1, .EnumName = "MouseButton" }, { .ArgumentIndex = 2, .EnumName = "InputEventState" } };
			ScriptMemberOptions gamepad = mutation;
			gamepad.EnumParameters = {
				{ .ArgumentIndex = 2, .EnumName = "GamepadButton", .SelectorIndex = 3, .Selector = ScriptArgumentType::String },
				{ .ArgumentIndex = 3, .EnumName = "InputEventState", .SelectorIndex = 3, .Selector = ScriptArgumentType::String },
				{ .ArgumentIndex = 2, .EnumName = "GamepadAxis", .SelectorIndex = 3, .Selector = ScriptArgumentType::Number },
			};
			const std::string description = std::format("Feature test suites, assertions, waits and deterministic input. A reported message shows authored locations "
														"and keeps at most {} bytes, truncated and marked like a log entry. A case's report keeps at most {} failures for "
														"expectations and unexpected script errors, the last then counting any further ones as [truncated: <n> more "
														"failures], and a suite at most {} such failures. Test.Inject* queues at most {} events per input tick.",
				MaxScriptLogMessageBytes, MaxScriptTestFailuresPerCase, MaxScriptTestFailuresPerSuite, MaxScriptInjectedInputsPerTick);
			api.Module("Test", description)
				.Function("Suite", Detail::CreateTestSuite, "(name: string, body: () -> (), options: TestSuiteOptions?) -> TestSuite",
					std::format("Construct a suite without executing its body; the name is UTF-8 without NUL and at most {} bytes.", MaxScriptTestNameBytes), all)
				.Function("Case", Utils::Case, "(name: string, fn: () -> (), options: TestCaseOptions?) -> ()",
					std::format("Register a case while collecting the suite body; the name is nonempty, unique in the suite and at most {} bytes, and a suite has at most {} cases.", MaxScriptTestNameBytes, MaxScriptTestCasesPerSuite), pure)
				.Function("Expect", Utils::Expect, "(condition: boolean, message: string?) -> ()", "Record a failed condition and continue the case.", pure)
				.Function("ExpectEqual", Utils::ExpectEqual, "(a: any, b: any, message: string?) -> ()", "Record a failed Luau equality comparison.", pure)
				.Function("ExpectNear", Utils::ExpectNear, "(a: number | vector, b: number | vector, epsilon: number?, message: string?) -> ()", "Compare finite numbers or vector components with an absolute epsilon, default 0.00001.", pure)
				.Function("Fail", Utils::Fail, "(message: string) -> ()", "End this case as failed. A case thread, or a task a behaviour spawned (which is cancelled), never runs past the call, even inside pcall; a lifecycle callback cannot yield, so there the call raises a script error that unwinds to the nearest pcall. The outcome stays latched either way.", pure)
				.Function("Skip", Utils::Skip, "(reason: string) -> ()", "End this case as skipped without erasing earlier failures; it stops its caller the way Test.Fail does, and the outcome stays latched.", pure)
				.Function("WaitTicks", Utils::WaitTicks, "(ticks: number) -> ()", "Yield a test thread for simulation ticks.", pure)
				.Function("WaitUntil", Utils::WaitUntil, "(predicate: () -> boolean, timeoutTicks: number) -> ()", "Wait for a predicate through a bounded simulation deadline.", pure)
				.Function("ExpectScriptError", Utils::ExpectScriptError, "(pattern: string, withinTicks: number) -> ()", std::format("Claim one nonfatal error occurrence in the current case using a Luau message pattern of at most {} bytes; the case keeps at most {} unclaimed errors to claim.", MaxScriptErrorPatternBytes, MaxScriptTestUnclaimedErrors), pure)
				.Function("CaptureAudio", Utils::CaptureAudio, "(ticks: number) -> TestAudioLevels", "Capture exactly the requested current-and-following ticks; yield until grouped audio is ready.", mutation)
				.Function("InjectAction", Utils::InjectAction, "(name: string, state: InputEventState?, value: number?) -> ()", "Queue a named button state or analog value (with a nil or Down state) for the next unapplied input tick.", action)
				.Function("InjectKey", Utils::InjectKey, "(key: Key, state: InputEventState) -> ()", "Queue a key state for the next unapplied input tick.", key)
				.Function("InjectMouse", Utils::InjectMouse, "(button: MouseButton, state: InputEventState, position: vector?) -> ()", "Queue a mouse button and optional finite XY position.", mouse)
				.Function("InjectGamepad", Utils::InjectGamepad, "((index: number, button: GamepadButton, state: InputEventState) -> ()) & ((index: number, axis: GamepadAxis, value: number) -> ())",
					"Queue a gamepad button state, or an engine-convention axis value when the third argument is a number.", gamepad)
				.Function("Screenshot", Utils::Screenshot, "(name: string) -> ()",
					std::format("Capture a screenshot through the owning host; a suite takes at most {} screenshots, and a later call raises an error without capturing.", MaxScriptTestScreenshotsPerSuite), mutation)
				.Function("ReloadScript", Utils::ReloadScript, "(asset: AssetRef) -> ()", "Synchronously reload a script in an editor test.", editor)
				.Function("GetStateHash", Utils::GetStateHash, "() -> string", "Read the canonical simulated state hash.", pure)
				.Function("GetLastExtraction", Utils::GetLastExtraction, "() -> TestExtraction", "Read the last completed render extraction.", pure)
				.Function("GetExtractedPosition", Utils::GetExtractedPosition, "(entity: Entity) -> vector?", "Read an entity's position in the last render snapshot.", pure)
				.Function("GetScriptErrors", Utils::GetScriptErrors, "(since: number?) -> {ScriptError}", std::format("Read occurrences newer than an exclusive error cursor; claimed errors remain visible. The VM keeps the {} distinct errors with the latest occurrences.", MaxScriptErrorEntries), pure);
			return {};
		}

	}

}
