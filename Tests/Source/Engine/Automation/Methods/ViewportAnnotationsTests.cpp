#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportAnnotations: strict options parse with located errors" * doctest::skip(true))
		{
			// All valid enum/boolean values, unknown names and malformed objects.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("ViewportAnnotations: capture annotations do not persist to the next capture" * doctest::skip(true))
		{
			// Collider alpha, label IDs, bounds and axes; frame settings preserved.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportAnnotations: selection aliases and explicit EntityRefs resolve atomically" * doctest::skip(true))
		{
			// all/selection/selected/none, [] and UUID/prefix/path lists; duplicate IDs collapse; bad index located; Runtime explicit IDs.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
