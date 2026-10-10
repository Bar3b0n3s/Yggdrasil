#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Callbacks: order and phases match §5.7" * doctest::skip())
		{
			FAIL("M13 contract: exercise setup, fixed, frame, nested creation and child-first destruction; use UUID order different from canonical hierarchy order");
		}

		TEST_CASE("Errors: location, traceback and instance disabling" * doctest::skip())
		{
			FAIL("M13 contract: callback and coroutine faults retain frames and disable only their owning instance");
		}

		TEST_CASE("HotReload: instance state survives" * doctest::skip())
		{
			FAIL("M13 contract: patch the class in place, preserve state and add new field defaults");
		}

		TEST_CASE("HotReload: editing a required module updates dependents" * doctest::skip())
		{
			FAIL("M13 contract: reload transitive dependents and their captured functions in dependency order");
		}

		TEST_CASE("HotReload: deferred while lockstep owns time" * doctest::skip())
		{
			FAIL("M13 contract: lockstep, recording, replay and test ownership defer ordinary reloads");
		}

		TEST_CASE("HotReload: a failing dependent rolls back the chain" * doctest::skip())
		{
			FAIL("M13 contract: a late error preserves all prior cached tables, functions and instance state");
		}

		TEST_CASE("ScriptEngine: retained references never survive their VM or alias released work" * doctest::skip())
		{
			FAIL("M13 contract: foreign, wrong-kind, released and previous-scene references are rejected without VM access");
		}
	}

}
