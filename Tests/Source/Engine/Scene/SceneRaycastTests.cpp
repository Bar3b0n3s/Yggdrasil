#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("Raycast: CPU ray hits expected triangle" * doctest::skip(true))
		{
			// Known mesh triangle; distance, barycentrics, geometric normal and UUID.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Raycast: ties use UUID then submesh and triangle" * doctest::skip(true))
		{
			// Same ray in Debug/Release/Dist gives same geometry choice.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Raycast: masks filter visual geometry without requiring a physics world" * doctest::skip(true))
		{
			// Default and inherited layer policy under ADR contract review.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Raycast: invalid rays fail and a zero mask is an empty hit" * doctest::skip(true))
		{
			// Finite validation, singular mesh transform and degenerate triangle cases.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Raycast: disabled invisible and pending-destruction meshes are excluded" * doctest::skip(true))
		{
			// No stale entity or physics handle accesses.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Raycast: play queries use the rendered pose at the view alpha" * doctest::skip(true))
		{
			// Runtime interpolation/reset tags match extraction.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Raycast: an excluded near hit cannot hide an in-interval triangle" * doctest::skip(true))
		{
			// MinDistance applied per candidate, inclusive endpoints; exact near-plane zero-distance orthographic hit allowed.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
