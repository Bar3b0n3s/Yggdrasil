#include "TestsPCH.h"
#include "Engine/Scripting/TaskScheduler.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Tasks: Task.Wait in OnStart raises the documented error" * doctest::skip())
		{
			FAIL("M13 contract: reject a non-yieldable callback before creating a wait");
		}

		TEST_CASE("Tasks: Task.Wait inside Task.Spawn resumes at the expected tick" * doctest::skip())
		{
			FAIL("M13 contract: immediate first slice, stable due order and simulation-only waits");
		}

		TEST_CASE("Tasks: yielding inside a metamethod raises the VM error, caught as a script error" * doctest::skip())
		{
			FAIL("M13 contract: a resumed task cannot yield across a non-yieldable metamethod");
		}

		TEST_CASE("TaskScheduler: destruction and case cancellation release every owned task" * doctest::skip())
		{
			FAIL("M13 contract: cancel active, waiting and delayed work without resuming it");
		}
	}

}
