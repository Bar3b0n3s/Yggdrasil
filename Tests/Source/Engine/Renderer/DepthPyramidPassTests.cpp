#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Projection: orthographic reverse-Z mapping and view-ray reconstruction" * doctest::skip(true))
		{
			// Near/far endpoints, off-centre rays, +Y/NVRHI convention.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("DepthPyramid: perspective sky and finite depth do not create false occluders" * doctest::skip(true))
		{
			// Zero depth is sentinel; half-float clamp; odd and one-pixel extents.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("DepthPyramid: GPU linear depth matches CPU references for both projections" * doctest::skip(true))
		{
			// Odd/small dimensions, all mips, sky and maximum representable depth.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("DepthPyramid: odd and one-dimensional reductions retain the final row and column" * doctest::skip(true))
		{
			// Independent footprints for 5x3, 1x5, 5x1, 1x1; unique near sample at final corner reaches every ancestor mip.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("DepthPyramid: odd-edge GPU minima match conservative footprint oracles" * doctest::skip(true))
		{
			// Unique near samples at last row/column, overlapping odd footprints and all available mips.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
