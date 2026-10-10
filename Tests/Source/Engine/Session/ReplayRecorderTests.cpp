#include "TestsPCH.h"

#include "Engine/Session/ReplayRecorder.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Session")
	{
		TEST_CASE("ReplayRecorder: recording begins only at tick zero of one session" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayRecorder: every applied input source is captured in order" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayRecorder: coalesced device input and releaseAll record their applied events" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayRecorder: header retains scene parameters seed and fixed rate" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayRecorder: external edits and hot reload preserve the first invalidation reason" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayRecorder: cancellation discards unwritten data without a later poll" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayRecorder: gameplay scene loads preserve the recording timeline" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayRecorder: final tick and state hash describe the completed boundary" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}
	}

}
