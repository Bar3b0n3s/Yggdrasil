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

		TEST_CASE("TrimCounter: counts every TrimUnused call of its registry")
		{
			GpuRecreationRegistry registry;
			const Test::TrimCounter counter(registry);
			CHECK(registry.TrimUnused() == 1);
			CHECK(registry.TrimUnused() == 1);
			CHECK(counter.GetTrims() == 2);
		}
	}

}
