#include "TestsPCH.h"

#include "Engine/Session/PlayInputEventCodec.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Session")
	{
		TEST_CASE("PlayInputEventCodec: automation and replay cover the same nine event kinds" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("PlayInputEventCodec: inappropriate members and nonfinite values have located errors" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("PlayInputEventCodec: unknown actions and controls include suggestions" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("PlayInputEventCodec: test injection refuses an explicit tick and queues nothing on error" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("PlayInputEventCodec: applied tap edges encode without a second expansion" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}
	}

}
