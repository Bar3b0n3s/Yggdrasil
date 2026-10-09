#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Selection: visible edges are solid and occluded edges are dim" * doctest::skip(true))
		{
			// Selected opaque/mask/mirrored/double-sided meshes; no stencil.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("Selection: deselection clears stale mask and resize releases view targets" * doctest::skip(true))
		{
			// Independent multiple views; no retained binding sets/leaked textures.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

}
