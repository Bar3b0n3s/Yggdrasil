#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("SceneRaycast: in-process round trip returns the triangle and located errors" * doctest::skip(true))
		{
			// No-GPU editor and Runtime contexts; target, mask, nonfinite direction and empty hit.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

}
