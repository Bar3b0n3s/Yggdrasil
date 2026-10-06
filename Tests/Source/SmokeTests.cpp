#include "TestsPCH.h"

namespace Engine {

	TEST_SUITE("Tests")
	{
		TEST_CASE("Smoke: test runner starts")
		{
			const doctest::ContextOptions* options = doctest::getContextOptions();
			REQUIRE(options != nullptr);
			REQUIRE(options->currentTest != nullptr);

			CHECK(std::string_view(options->currentTest->m_name) == "Smoke: test runner starts");
			CHECK(std::string_view(options->currentTest->m_test_suite) == "Tests");
		}
	}

}
