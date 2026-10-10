#include "TestsPCH.h"

#include "Engine/Session/ReplayPlayer.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Session")
	{
		TEST_CASE("ReplayPlayer: a fresh session replays all input events and expectations" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: tick zero and final tick expectations run at completed boundaries" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: strict hash detects divergence independently of recorded configuration" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: false and faulting expectations return located outcomes" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: repeated runs and host budget partitions produce identical final hashes" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: scene transitions evaluate bytecode in the replacement VM" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: session replacement cancels without touching the new session" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: invalid event streams queue no partial input" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("ReplayPlayer: cooked playback does not require automation or a compiler" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}
	}

}
