#include "TestsPCH.h"

#include "Shared/ShadowConstants.h"

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Shadows: shared constants match Slang reflection" * doctest::skip(true))
		{
			FAIL("M9 contract: compare every shared member offset with compiled shader reflection");
		}
	}

}
