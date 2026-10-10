#include "TestsPCH.h"
#include "Engine/Automation/Methods/ReplayMethods.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("ReplayMethods: recording starts at tick zero and captures every applied input source" * doctest::skip())
		{
			FAIL("M13 contract: real-device, automation and Test.Inject inputs share applied-event recording");
		}

		TEST_CASE("ReplayMethods: replay verifies expectations and the strict final state hash" * doctest::skip())
		{
			FAIL("M13 contract: recorded header, parameters, seed and input reproduce the state in a fresh session");
		}

		TEST_CASE("ReplayMethods: cancellation releases time ownership and never writes an invalid recording" * doctest::skip())
		{
			FAIL("M13 contract: owner disconnect and session replacement cancel pending work safely");
		}

		TEST_CASE("ReplayMethods: verification failures retain every expectation outcome and source location" * doctest::skip())
		{
			FAIL("M13 contract: structured Validation data includes finalTick, hash state and per-expectation outcomes");
		}

		TEST_CASE("ReplayMethods: another active driver is rejected before replacing the session" * doctest::skip())
		{
			FAIL("M13 contract: verify unchanged session serial and input ownership on rejected replay admission");
		}

		TEST_CASE("ReplayMethods: read-only recording start is transient and stop checks write permission" * doctest::skip())
		{
			FAIL("M13 contract: no output or consumed recording on denied Stop; Runtime confines output below user Replays");
		}
	}

}
