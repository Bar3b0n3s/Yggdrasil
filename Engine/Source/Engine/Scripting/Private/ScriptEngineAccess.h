#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Scripting/Private/ScriptCall.h"
#include "Engine/Scripting/ScriptReference.h"
#include "Engine/Scripting/ScriptTestHost.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Engine {

	class Sandbox;

	namespace Detail {

		enum class ScriptWaitKind : uint8_t
		{
			Ticks,
			Predicate,
			ExpectedError,
			Audio
		};

		struct ScriptTestWait
		{
			ScriptWaitKind Kind = ScriptWaitKind::Ticks;
			uint64_t Ticks = 0;
			ScriptReference Predicate{};
			std::string Pattern{};
			ErrorLocation Location{};
		};

		enum class ScriptThreadKind : uint8_t
		{
			Task,
			Case
		};

		// Internal operations on ScriptEngine's one VM. Stack operations require a protected native entry. References
		// are independently retained, kind checked and bound to that VM; callers release their own references.
		struct ScriptEngineAccess
		{
			[[nodiscard]] static Sandbox& GetSandbox(ScriptEngine& engine);
			// Pushes the existing instance or nil, without creating an instance or running lifecycle callbacks.
			[[nodiscard]] static Status PushInstance(ScriptCall& call, UUID entity);
			[[nodiscard]] static Result<ScriptReference> RetainFunction(ScriptCall& call, int index);
			[[nodiscard]] static Status PushFunction(ScriptCall& call, ScriptReference reference);
			// The executing caller's ownership, never the receiver of an Entity/Component method.
			[[nodiscard]] static ScriptTaskOwner GetOwner(const ScriptCall& call);
			// Protected wrappers save/restore the actual executing child, including evaluation/module child states.
			// The pointer is borrowed for the synchronous protected entry only; nullptr restores an idle engine.
			[[nodiscard]] static lua_State* ExchangeActiveThread(ScriptEngine& engine, lua_State* thread);
			// A latched terminal case or cancelled running task cannot perform another native API effect even when
			// Luau pcall caught its signal. Registry dispatch/write guards call this before invoking native code.
			[[nodiscard]] static Status CheckExecution(const ScriptCall& call);
			// Scheduler infrastructure failures use the engine's ordinary publication/isolation path exactly once.
			static void PublishTaskFailure(ScriptEngine& engine, const Error& error, ScriptTaskOwner owner);

			// Only during suite collection; copies the location/name and independently retains the stack function.
			[[nodiscard]] static Status RegisterTestCase(ScriptCall& call, int functionIndex, std::string_view name,
				uint32_t timeoutTicks, const ErrorLocation& location);
			// Validates a resumed, yieldable case/task before any effect. Predicate is independently retained. Returns
			// the number of immediate results when already satisfied, or -1 when the caller must lua_yield(state, 0).
			// Expected-error patterns are validated here. Audio starts here after validation, never in the binding.
			[[nodiscard]] static Result<int> ArmTestWait(ScriptCall& call, const ScriptTestWait& wait);
			// Publishes the report and latches Fail/Skip so pcall cannot swallow a terminal test outcome.
			[[nodiscard]] static Status EndTestCase(ScriptCall& call, const ScriptTestReport& report);
			[[nodiscard]] static Status ArmTaskWait(ScriptCall& call, uint64_t ticks);

			// Scheduler operations are host calls; allocation-producing setup enters the protected VM boundary.
			[[nodiscard]] static Result<ScriptReference> CreateThread(ScriptEngine& engine, ScriptReference function,
				ScriptTaskOwner owner, ScriptThreadKind kind);
			[[nodiscard]] static Result<ScriptCaseResume> ResumeThread(ScriptEngine& engine, ScriptReference thread);
			[[nodiscard]] static Result<uint64_t> GetDueTick(ScriptEngine& engine, ScriptReference thread);
			[[nodiscard]] static Status ValidateTask(ScriptEngine& engine, ScriptReference thread);
			static void CancelThread(ScriptEngine& engine, ScriptReference thread);
		};

	}

}
