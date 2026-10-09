#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Shadows: cascade splits" * doctest::skip(true))
		{
			// Practical splits for 1 and 4 cascades, lambda 0 and 1; invalid finite ranges.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Shadows: sub-texel camera move leaves the light matrix unchanged" * doctest::skip(true))
		{
			// Use a move within one known snap cell; verify all entries, not just XY translation.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Shadows: orthographic frustum corners fit" * doctest::skip(true))
		{
			// All eight orthographic corners fit each cascade; finite depth and reverse-Z.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Shadows: off-screen casters remain inside the extended light volume" * doctest::skip(true))
		{
			// Camera frustum culling must not remove shadow casters.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Shadows: softness and blend are continuous across cascade boundaries" * doctest::skip(true))
		{
			// World-space penumbra -> per-cascade UV; 10 percent blending and distance fade.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Shadows: snapped wide orthographic slices remain wholly contained" * doctest::skip(true))
		{
			// Aspect 1000:1, map size 256, centers near half-texel; every corner inside XYZ after snap, stable within same cell.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Shadows: containment radius includes snapping margin without translation refits" * doctest::skip(true))
		{
			// R>=r+R/N for 256..8192, float conversion cannot shrink; invalid/unrepresentable radii return InvalidArgument.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
