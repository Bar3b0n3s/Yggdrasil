#include "TestsPCH.h"

#include "Engine/Physics/PhysicsEngine.h"

#include "Engine/App/ProcessContext.h"
#include "Engine/Physics/PhysicsWorld.h"

#include <algorithm>

// Jolt's process-level state (Architecture §9.1, §4.1; Roadmap M11). The Tests main's ProcessContext runs the Physics
// step, so these tests see an initialized engine. Skipped skeletons of the M11 contract
// (Docs/Decisions/0014-m11-decisions.md): stream A implements the engine and removes the skips.

namespace Engine {

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsEngine: the process context initializes Jolt once, with a verified version" * doctest::skip(true))
		{
			const ProcessContext* context = ProcessContext::GetCurrent();
			REQUIRE(context != nullptr);
			const auto steps = context->GetSteps();
			CHECK(std::find(steps.begin(), steps.end(), ProcessContextStep::Physics) != steps.end());
			CHECK(PhysicsEngine::IsInitialized());
			// No world outlives its test.
			CHECK(PhysicsEngine::GetLiveWorldCount() == 0);
			CHECK(PhysicsEngine::GetLiveBodyCount() == 0);
		}

		TEST_CASE("PhysicsEngine: the worker thread count changes between steps and is checked" * doctest::skip(true))
		{
			// The Tests main's ProcessContext takes the default count (Architecture §4.11: clamp(hw - 2, 1, 4)).
			const uint32_t original = PhysicsEngine::GetWorkerThreadCount();
			CHECK(original >= PhysicsEngineSpecification::MinDefaultWorkerThreads);
			CHECK(original <= PhysicsEngineSpecification::MaxDefaultWorkerThreads);
			for (const uint32_t count : { 0u, 1u, 8u })
			{
				CAPTURE(count);
				REQUIRE(PhysicsEngine::SetWorkerThreadCount(count).has_value());
				CHECK(PhysicsEngine::GetWorkerThreadCount() == count);
			}
			const Status tooMany = PhysicsEngine::SetWorkerThreadCount(PhysicsEngineSpecification::MaxWorkerThreads + 1);
			REQUIRE_FALSE(tooMany.has_value());
			CHECK(tooMany.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(PhysicsEngine::SetWorkerThreadCount(original).has_value());
		}

		TEST_CASE("PhysicsEngine: worlds and bodies are counted while they live" * doctest::skip(true))
		{
			Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			CHECK(PhysicsEngine::GetLiveWorldCount() == 1);
			Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{} } });
			REQUIRE(shape.has_value());
			Result<BodyHandle> body = (*world)->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static });
			REQUIRE(body.has_value());
			CHECK(PhysicsEngine::GetLiveBodyCount() == 1);
			(*world)->DestroyBody(*body);
			CHECK(PhysicsEngine::GetLiveBodyCount() == 0);
			// Destroying the world destroys the bodies still in it.
			REQUIRE((*world)->CreateBody({ .Shape = *shape, .MotionType = PhysicsMotionType::Static }).has_value());
			world->reset();
			CHECK(PhysicsEngine::GetLiveWorldCount() == 0);
			CHECK(PhysicsEngine::GetLiveBodyCount() == 0);
		}
	}

}
