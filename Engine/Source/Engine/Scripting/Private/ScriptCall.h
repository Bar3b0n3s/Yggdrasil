#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptError.h"

#include <lua.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Engine {

	class Sandbox;
	class ScriptEngine;

	enum class ScriptExecutionOrigin : uint8_t
	{
		Pure,
		Gameplay,
		TestDriver,
		Eval
	};

	// Owned by the outer protected execution slice, never by a ScriptCall. Nested calls borrow this same context,
	// including lifecycle callbacks triggered by eval/test-driver writes. Ordinary frame callbacks use Gameplay;
	// suite/case bodies use TestDriver; evaluations use Eval; hostless extraction/setup uses Pure. Task records copy
	// only Origin, never this address. A later task/case resume creates a fresh context/latch with that retained origin,
	// so starting another recording between slices cannot hide a subsequent external write. No script can set origin.
	struct ScriptExecutionContext
	{
		ScriptExecutionOrigin Origin = ScriptExecutionOrigin::Pure;
		bool ExternalMutationNotified = false;
	};

	// Private, synchronous stack facade. State borrows the active main state or resumed coroutine; Sandbox borrows its
	// owning sandbox. Both outlive this call and are nonnull on dispatch. Engine borrows the runtime ScriptEngine and
	// is null for a pure LoadTimeVm, which has no IScriptHost. Availability guards run BEFORE any engine/host access;
	// pure helpers work from State/Sandbox alone, and load-time calls never touch runtime counters. The VM trampoline
	// constructs this on the C++ stack; it must never be copied into userdata, a Luau upvalue, a task or a stored lambda.
	// MemberName borrows immutable registry text for uniform errors. Runtime host services come from Engine::GetHost(); there
	// are no Session/Testing/App includes or pointers. Native VM ownership uses its private allocator/thread context,
	// never an exposed userdata/lightuserdata pointer or script-visible global. No stack string survives the entry.
	struct ScriptCall
	{
		// Permission only: InvalidState if read-only, no runtime host/context, or Origin is Pure. Does not notify the
		// recorder. Registry dispatch checks getter/setter/function Mutates before native entry; helpers repeat the check.
		// Local values need no host write permission. Test.Inject* still checks this before validated input queuing.
		[[nodiscard]] Status CheckWritable() const;
		// Rechecks permission, then for Eval/TestDriver latches and calls IScriptHost::OnExternalMutation(MemberName)
		// once per execution slice, BEFORE the first gameplay-state/shared-random side effect. Call after validation,
		// immediately before a write that may partially succeed and then fault; a later fault never undoes notification.
		// Gameplay never notifies. Recorded Test.Inject*, pure local values and local random streams do not call this.
		// Mutates metadata alone cannot trigger notification: permission and input-record invalidation are distinct.
		[[nodiscard]] Status PrepareHostMutation() const;

		lua_State* State = nullptr;
		::Engine::Sandbox* Sandbox = nullptr;
		ScriptEngine* Engine = nullptr;
		std::string_view MemberName{};
		// Borrowed for this entry from the outer protected slice; required for runtime dispatch. Never stored in Lua,
		// a task or a deferred lambda. The VM/engine access bridge supplies it from its private execution-context stack.
		ScriptExecutionContext* Execution = nullptr;
	};

	// A VM execution outcome, not yet published. Failure owns traceback and source strings before the thread/stack
	// changes. Failure implies !Yielded and ResultCount == 0; otherwise ResultCount values remain for the caller.
	// ScriptEngine publishes/deduplicates runtime Failure and applies instance isolation exactly once; LoadTimeVm maps
	// pure failures into import diagnostics without any host. The outer Result is
	// reserved for host precondition errors (and Unsupported during this contract), distinct from a script fault.
	struct ScriptCallResult
	{
		int ResultCount = 0;
		bool Yielded = false;
		std::optional<ScriptError> Failure{};
	};

	namespace Lua {

		// Stack is [..., function, arguments]. Counts must be nonnegative (resultCount may be LUA_MULTRET). lua_pcall
		// receives an error handler which captures frames BEFORE unwinding; nested calls inherit the current deadline.
		// On fault, restore the caller stack below the function and return an owned structured Failure. Callbacks cannot
		// yield. Host precondition failures do not change the stack; VM statuses are always checked, never caught in C++.
		[[nodiscard]] Result<ScriptCallResult> ProtectedCall(ScriptCall& call, int argumentCount, int resultCount);
		// Protected native setup/invocation, including allocation-producing binding setup and argument marshalling.
		// Runs function via a VM protected trampoline; never directly calls it from unprotected engine code. Starts with
		// an empty argument frame and retains its results, restoring the original stack on error. Same deadline rule.
		[[nodiscard]] Result<ScriptCallResult> ProtectedCall(ScriptCall& call, ScriptNativeFunction function);
		// Resumes call.State, a coroutine; from is null for the host or a state in the same VM. Nested resumes inherit the
		// active deadline and execution context; an outermost resume gets a fresh deadline/context with retained origin.
		// The wrapper balances watchdog entry/exit. LUA_YIELD is success with Yielded=true. lua_resume has no error handler: capture
		// the failed thread's frames BEFORE reset/pop and return owned Failure. The wrapper does not reset the coroutine.
		// A fault leaves no consumable results (ResultCount=0), but retains its frames until the owner disposes of it.
		[[nodiscard]] Result<ScriptCallResult> ProtectedResume(ScriptCall& call, lua_State* from, int argumentCount);

	}

}
