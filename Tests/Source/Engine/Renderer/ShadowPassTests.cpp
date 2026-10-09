#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Shadows: a 0.37 texel camera move stays below the shimmer threshold" * doctest::skip(true))
		{
			// Measure stable interior shadow edges with deterministic threshold; record metric.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Shadows: masked mirrored and double-sided casters preserve M8 parity" * doctest::skip(true))
		{
			// Match the depth prepass alpha texture test and cull modes.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Shadows: both depth-clamp and extended-near fallback render off-screen casters" * doctest::skip(true))
		{
			// Force capability off for fallback; validation and lifetime counts zero.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("DebugViews: cascades show exact colors with the shadow blend and distance fade" * doctest::skip(true))
		{
			// Synthetic receivers in each cascade and 10 percent blend; no shadow allocation/background black.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
