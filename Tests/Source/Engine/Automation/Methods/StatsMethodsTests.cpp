#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("StatsGet: renderer none reports counts with unavailable GPU samples" * doctest::skip(true))
		{
			// Editor and exported Runtime, no state mutation.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}

		TEST_CASE("StatsGet: per-view pass summaries retain delayed GPU frame identities" * doctest::skip(true))
		{
			// Injected CPU stats provider; screenshot does not change viewport sample.
			FAIL("M9 contract: implement this acceptance case before removing skip");
		}
	}

}
