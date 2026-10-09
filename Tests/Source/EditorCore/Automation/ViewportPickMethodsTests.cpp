#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportPick: scene and game rays use the current framebuffer extent" * doctest::skip(true))
		{
			// Headless stored size, scene camera override and game primary camera.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("ViewportPick: boundary pixels and missing cameras return located errors" * doctest::skip(true))
		{
			// No GPU dependency; no selection/history side effects.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportPick: camera clipping uses ray intervals in both projections" * doctest::skip(true))
		{
			// Perspective off-axis c=.8 and FarClip=10 sees z=-9 at t=11.25; near-clipped foreground skipped; ortho length=far-near.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportPick: scene and game extents remain independent across resize and screenshots" * doctest::skip(true))
		{
			// GetPixelSize is actual rendered extent; pending panel size/screenshot never changes it; zero returns InvalidState.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
