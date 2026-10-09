#include "TestsPCH.h"
#include "EditorCore/EditorContext.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorSelection: a runtime-only UUID can be selected without touching the edit scene" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement target-aware selection before removing this skip");
		}

		TEST_CASE("EditorSelection: invalid selection is atomic and Stop restores edit UUIDs" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement target-aware selection before removing this skip");
		}
	}

}
