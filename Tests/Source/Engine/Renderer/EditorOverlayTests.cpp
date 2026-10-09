#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("EditorOverlay: grid and procedural icons support both camera projections" * doctest::skip(true))
		{
			// Orthographic cameras never use perspective distance attenuation.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("EditorOverlay: no camera or disabled view flags produce no overlay" * doctest::skip(true))
		{
			// Capture defaults clean; behind-camera icons skipped.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Wireframe: portable triangle edges respect mirrored double-sided and budget rules" * doctest::skip(true))
		{
			// No polygon-line feature needed; CPU winding/degenerate cases, deterministic overflow count, disabled flags append nothing.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Wireframe: solid picking remains unchanged while mesh interiors become background" * doctest::skip(true))
		{
			// Synthetic front/occluded/masked/blend geometry; compare exact EntityId before/after, one-pixel LineList under both API caps.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
