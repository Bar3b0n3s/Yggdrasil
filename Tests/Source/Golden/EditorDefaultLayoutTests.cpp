#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: EditorDefaultLayout" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: capture editor.screenshot on the headless null platform with an isolated user layout");
		}
	}

}
