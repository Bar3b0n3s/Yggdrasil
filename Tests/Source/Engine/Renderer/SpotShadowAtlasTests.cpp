#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SpotShadowAtlas: importance ties use UUID and tiles do not overlap" * doctest::skip(true))
		{
			// Eight tiles maximum, deterministic row order, two-texel guards.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("SpotShadowAtlas: excess shadowed spots are reported without dropping their light" * doctest::skip(true))
		{
			// Nine visible spots -> eight tiles plus one budget diagnostic.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("SpotShadowAtlas: invalid indices and non-finite lights fail" * doctest::skip(true))
		{
			// Reject duplicate indices and invalid range/direction.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

}
