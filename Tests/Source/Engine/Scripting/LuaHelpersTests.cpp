#include "TestsPCH.h"
#include "Engine/Scripting/LuaHelpers.h"

#include "Engine/Core/Base.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <doctest/doctest.h>

#include <type_traits>

namespace Engine {

	static_assert(std::is_same_v<ScriptNativeFunction, int (*)(ScriptCall&)>);
	static_assert(std::is_same_v<decltype(&Lua::Check<float>), float (*)(ScriptCall&, int)>);
	static_assert(std::is_same_v<decltype(&Lua::Push<glm::vec3>), void (*)(ScriptCall&, const glm::vec3&)>);

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Bindings: NaN and Inf are rejected with located errors" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Through ScriptEngine evaluate NaN and both infinities as scalar/vector/Quat/Color arguments and reflected "
				"writes; assert member, argument, source line, no invalid system inputs and unchanged scene");
		}

		TEST_CASE("LuaHelpers: checks reject coercions fractional integers and narrowing overflow" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Bind typed probes and test exact signed/unsigned boundaries, float narrowing, bool/string distinctions, embedded "
				"NUL strings, nonzero quaternions and vector2 z == 0");
		}

		TEST_CASE("LuaHelpers: pushes and checks preserve supported engine values" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Round-trip every declared specialization through the public ScriptEngine evaluation API; verify native vectors, "
				"tagged Quat/Color/AssetRef identities and nil invalid references without private test access");
		}

		TEST_CASE("LuaHelpers: reflected marshalling traverses fields without losing type identity" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Round-trip scalar/enum/array/map/struct/Variant schemas in canonical order, retain Color-versus-vector and "
				"asset-versus-entity types, reject cycles and unsupported script values, and leave proxy counters untouched");
		}

		TEST_CASE("LuaHelpers: protected callbacks capture traceback before unwinding" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Trigger nested callback and argument errors via ScriptEngine, verify RAII cleanup, owned frames and stack "
				"balance, then call a healthy instance; assert one structured publication and correct instance isolation");
		}

		TEST_CASE("LuaHelpers: protected resumes capture the failed thread and distinguish yielding" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Run task/case coroutines to yield, complete and fail; preserve frames until capture, retain yielded result "
				"counts, restore resume ownership, and prove healthy calls continue without duplicate errors");
		}

		TEST_CASE("LuaHelpers: protected native setup contains marshalling and allocation errors" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Exercise binding setup and argument creation under the native protected trampoline, including memory errors "
				"before user bytecode; no VM exception escapes the public sandbox/engine boundary");
		}

		TEST_CASE("LuaHelpers: pure load-time calls never require a runtime engine or host" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Use public LoadTimeVm to run Script/Field/Math/Quat/Color/Test.Suite with a pure Sandbox; blocked host APIs "
				"return located errors before dereferencing a null Engine and do not touch runtime coverage");
		}

		TEST_CASE("LuaHelpers: writable checks protect nested and deferred execution" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Read-only eval cannot gain writes through nested require, callback aliases, resumed work or task scheduling; VM "
				"policy and host-facing checks agree while local allocations and local generators remain allowed; read-only "
				"InitializeInstances supplies resolved field tables/class links without lifecycle callbacks or task execution");
		}

		TEST_CASE("LuaHelpers: external writes invalidate recording before side effects and later faults" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Observe public IScriptHost notification before eval/test-driver entity writes and shared random advancement; "
				"notify once through nested helpers and callbacks, retain invalidation after a later fault, and leave recording "
				"valid for rejected read-only/invalid writes, pure local values, normal gameplay and recorded Test.Inject input");
		}

		TEST_CASE("LuaHelpers: deferred work retains origin and renews mutation notification per resume" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Spawn from eval/test-driver/gameplay contexts and resume on later ticks; nested first slices inherit the outer "
				"deadline and notification latch, later slices use fresh latches with the retained origin, and a newly started "
				"recording is invalidated before a later external write without retaining stack-context pointers");
		}
	}

}
