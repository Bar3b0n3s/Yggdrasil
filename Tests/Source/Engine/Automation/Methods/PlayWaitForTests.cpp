#include "TestsPCH.h"
#include "Engine/Automation/Methods/PlayMethods.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("PlayWaitFor: evaluates after every tick and returns the satisfying value" * doctest::skip())
		{
			FAIL("M13 contract: compile once, share the play VM and partition work by the existing frame budget");
		}

		TEST_CASE("PlayWaitFor: timeout reports false while script faults retain their source location" * doctest::skip())
		{
			FAIL("M13 contract: tick timeout is distinct from a predicate VM failure or cancellation");
		}
	}

}
