#include "TestsPCH.h"

#include "Support/TestOptions.h"

namespace Engine {

	using namespace doctest;

	TEST_SUITE("Core")
	{
		// The allowed control, unqualified after the using-directive.
		TEST_CASE("Timer: the target never returns" * test_suite(Test::ChildTargetSuite) * skip())
		{
		}

		TEST_CASE("Timer: elapsed time grows" * skip()) // Seeded defect: an unqualified skip.
		{
		}

		TEST_CASE("Timer: a member named like the decorator is not one")
		{
			const struct
			{
				bool skip = false;
			} options;
			CHECK_FALSE(options.skip);
		}
	}

}
