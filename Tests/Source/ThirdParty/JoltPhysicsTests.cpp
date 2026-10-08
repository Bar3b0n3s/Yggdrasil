#include "TestsPCH.h"

#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>

namespace Engine {

	// A failed Jolt assert (JPH_ENABLE_ASSERTS is defined for Debug and Release, the configurations Tests exists in).
	ENGINE_DEATH_TEST("Physics/JoltAssertFails")
	{
		JPH_ASSERT(false, "a deliberately failed Jolt assert");
	}

	TEST_SUITE("ThirdParty")
	{
		// Jolt encodes its ABI-relevant defines in JPH_VERSION_ID (Jolt/Core/Core.h): feature bit N (1-based) sits at
		// bit N - 1 of JPH_VERSION_FEATURES, which starts at bit 24 of the ID. JPH_CROSS_PLATFORM_DETERMINISTIC is
		// feature bit 2. VerifyJoltVersionID() compares the library's ID with this translation unit's, so it fails if
		// the library and this consumer were built with different defines (Architecture §2.2, §9.1).
		TEST_CASE("Jolt: version ID includes JPH_CROSS_PLATFORM_DETERMINISTIC")
		{
			// JPH_VERSION_ID expands to an unqualified uint64(...), as it is meant to be used inside namespace JPH.
			using JPH::uint64;

			constexpr uint64 FeatureShift = 24;
			constexpr uint64 CrossPlatformDeterministicFeature = uint64{ 1 } << 1;
			constexpr uint64 VersionID = JPH_VERSION_ID;

			CHECK(((VersionID >> FeatureShift) & CrossPlatformDeterministicFeature) != 0);
			CHECK(JPH::VerifyJoltVersionID());
		}

		// PhysicsEngine::Initialize routes JPH::Trace to the Engine logger (Architecture §9.1), from any thread.
		TEST_CASE("Jolt: trace lines reach the engine log")
		{
			const Test::ExpectLog expected(LogLevel::Info, "Jolt: a trace line with 42 and text");
			JPH::Trace("a trace line with %d and %s", 42, "text");
			CHECK(expected.GetMatchCount() == 1);
		}

		// ... and JPH::AssertFailed to the engine's assert handler: a failed Jolt assert ends the process like
		// ENGINE_CORE_ASSERT, naming Jolt's message.
		TEST_CASE("Jolt: a failed Jolt assert reaches the engine's assert handler")
		{
			ENGINE_CHECK_DEATH("Physics/JoltAssertFails", "Jolt: a deliberately failed Jolt assert");
		}
	}

}
