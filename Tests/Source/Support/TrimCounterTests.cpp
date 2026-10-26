#include "TestsPCH.h"

#include "Support/TrimCounter.h"

#include <string>
#include <vector>

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("TrimCounter: is registered with its registry while it lives")
		{
			GpuRecreationRegistry registry;
			{
				const Test::TrimCounter counter(registry);
				CHECK(registry.GetOwnerNames() == std::vector<std::string>{ "TrimCounter" });
				CHECK(counter.GetTrims() == 0);
			}
			CHECK(registry.GetOwnerCount() == 0);
		}

		TEST_CASE("TrimCounter: counts every TrimUnused call of its registry" * doctest::skip(true))
		{
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 43): skipped until W18.4's stream B implements
			// GpuRecreationRegistry::TrimUnused.
			GpuRecreationRegistry registry;
			const Test::TrimCounter counter(registry);
			CHECK(registry.TrimUnused() == 1);
			CHECK(registry.TrimUnused() == 1);
			CHECK(counter.GetTrims() == 2);
		}
	}

}
