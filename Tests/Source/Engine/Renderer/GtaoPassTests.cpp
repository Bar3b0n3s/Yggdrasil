#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("GTAO: screen radius for perspective and orthographic" * doctest::skip(true))
		{
			// Perspective inverse depth; orthographic constant screen radius.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("GTAO: quality and half-resolution settings choose the documented sample count" * doctest::skip(true))
		{
			// Low/Medium/High 1/2/3 slices and three steps per side; explicit project half setting.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("GTAO: AO changes indirect light only and preserves direct lighting" * doctest::skip(true))
		{
			// Independent direct/ambient scenes; material AO min and multi-bounce.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("GTAO: denoise preserves depth edges and half resolution upsamples without halos" * doctest::skip(true))
		{
			// Both projections, odd dimensions, no frame-dependent noise.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("DebugViews: AO displays final upsampled occlusion without material AO or post effects" * doctest::skip(true))
		{
			// Known AO field including half/odd size; disabled/background white, direct lights/materialAO/exposure do not change pixels.
			FAIL("M9 reviewed contract: implement this acceptance before removing skip");
		}
	}

}
