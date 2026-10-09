#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Application: submitted hook follows the scene and UI list and covers queued pick copies" * doctest::skip(true))
		{
			FAIL("M9 contract: verify actual submission IDs, image/table pairing and frame-slot retirement");
		}

		TEST_CASE("Application: skipped frames do not publish submissions or consume another view's picks" * doctest::skip(true))
		{
			FAIL("M9 contract: cover resize, minimized frames and intervening screenshot submissions");
		}

		TEST_CASE("Application: a requested minimized UI frame renders offscreen without swapchain acquisition" * doctest::skip(true))
		{
			FAIL("M10 contract: verify fresh UI and submission hooks without restoring the window");
		}
	}

}
