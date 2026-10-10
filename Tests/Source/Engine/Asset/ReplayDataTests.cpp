#include "TestsPCH.h"

#include "Engine/Asset/ReplayData.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("ReplayData: canonical source round trips every input event and header parameter" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayData: scene handles survive moves and configuration is informational" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayData: malformed fields and newer versions return located errors" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayData: event and expectation tick boundaries are validated without reordering ties" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayData: cooked expectations load without a Luau compiler" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayData: cooked payload rejects truncation corrupt hashes and mismatched expectation counts" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayData: seeded source and binary mutations never assert or crash" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}
	}

}
