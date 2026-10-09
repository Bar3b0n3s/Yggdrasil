#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("GraphicsDevice: allocation count follows actual NVRHI and host-image memory" * doctest::skip(true))
		{
			FAIL("M9 contract: count native allocations, including pooled and deferred memory, before removing skip");
		}

		TEST_CASE("GraphicsDevice: failed allocations and a replacement device do not leak allocation counts" * doctest::skip(true))
		{
			FAIL("M9 contract: verify failed allocation and sequential device lifetimes before removing skip");
		}
	}

}
