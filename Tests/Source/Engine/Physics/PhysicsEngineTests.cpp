#include "TestsPCH.h"

#include "Engine/Physics/PhysicsEngine.h"

#include "Engine/App/ProcessContext.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Support/DeathTest.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <thread>

// Jolt's process-level state (Architecture §9.1, §4.1; Roadmap M11). The Tests main's ProcessContext runs the Physics
// step, so these tests see an initialized engine; the one that shuts it down brings it back before it ends.

namespace Engine {

	namespace {

		// Shuts the process's physics engine down for its lifetime and initializes it again, with the worker count it had, on
		// every exit path, so no later test finds it missing (tests never depend on order, CodeStyle §14).
		class ScopedPhysicsEngineShutdown
		{
		public:
			ScopedPhysicsEngineShutdown()
				: m_WorkerThreads(PhysicsEngine::GetWorkerThreadCount())
			{
				PhysicsEngine::Shutdown();
			}

			~ScopedPhysicsEngineShutdown()
			{
				if (!PhysicsEngine::IsInitialized())
					CHECK(PhysicsEngine::Initialize({ .WorkerThreads = m_WorkerThreads }).has_value());
			}

			ScopedPhysicsEngineShutdown(const ScopedPhysicsEngineShutdown&) = delete;
			ScopedPhysicsEngineShutdown& operator=(const ScopedPhysicsEngineShutdown&) = delete;
		private:
			uint32_t m_WorkerThreads = 0;
		};

	}

	ENGINE_DEATH_TEST("Physics/EngineInitializedTwice")
	{
		static_cast<void>(PhysicsEngine::Initialize({}));
	}

	ENGINE_DEATH_TEST("Physics/EngineShutDownUnderLiveWorld")
	{
		Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
		if (world.has_value())
			PhysicsEngine::Shutdown();
	}

	TEST_SUITE("Physics")
	{
		TEST_CASE("PhysicsEngine: the process context initializes Jolt once, with a verified version")
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

		TEST_CASE("PhysicsEngine: the worker thread count changes between steps and is checked")
		{
			// The Tests main's ProcessContext takes the default count (Architecture §4.11: clamp(hw - 2, 1, 4)).
			const uint32_t original = PhysicsEngine::GetWorkerThreadCount();
			CHECK(original >= PhysicsEngineSpecification::MinDefaultWorkerThreads);
			CHECK(original <= PhysicsEngineSpecification::MaxDefaultWorkerThreads);
			{
				// Restores the original count on every exit path, a failed REQUIRE included.
				const Test::ScopedWorkerThreadCount restore(original);
				for (const uint32_t count : { 0u, 1u, 8u })
				{
					CAPTURE(count);
					REQUIRE(PhysicsEngine::SetWorkerThreadCount(count).has_value());
					CHECK(PhysicsEngine::GetWorkerThreadCount() == count);
				}
				const Status tooMany = PhysicsEngine::SetWorkerThreadCount(PhysicsEngineSpecification::MaxWorkerThreads + 1);
				REQUIRE_FALSE(tooMany.has_value());
				CHECK(tooMany.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK(PhysicsEngine::GetWorkerThreadCount() == original);
		}

		TEST_CASE("PhysicsEngine: the default worker thread count is clamp(hardware concurrency - 2, 1, 4)")
		{
			const int64_t hardware = static_cast<int64_t>(std::thread::hardware_concurrency());
			const auto expected = static_cast<uint32_t>(std::clamp<int64_t>(hardware - 2, 1, 4));
			// The Tests main's ProcessContext passes no WorkerThreads.
			CHECK(PhysicsEngine::GetWorkerThreadCount() == expected);
		}

		TEST_CASE("PhysicsEngine: worlds and bodies are counted while they live")
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

		TEST_CASE("PhysicsEngine: without the engine shapes, worlds and thread counts are refused, and Initialize checks its threads")
		{
			const ScopedPhysicsEngineShutdown shutdown;
			REQUIRE_FALSE(PhysicsEngine::IsInitialized());
			// Shutdown is idempotent.
			PhysicsEngine::Shutdown();

			const Result<Ref<const PhysicsShape>> shape = PhysicsShape::Create({ .Colliders = { ColliderShapeDescription{} } });
			REQUIRE_FALSE(shape.has_value());
			CHECK(shape.error().GetCode() == ErrorCode::InvalidState);
			const Result<Scope<PhysicsWorld>> world = PhysicsWorld::Create({});
			REQUIRE_FALSE(world.has_value());
			CHECK(world.error().GetCode() == ErrorCode::InvalidState);
			const Status threads = PhysicsEngine::SetWorkerThreadCount(1);
			REQUIRE_FALSE(threads.has_value());
			CHECK(threads.error().GetCode() == ErrorCode::InvalidState);

			// Too many threads initialize nothing.
			const Status tooMany = PhysicsEngine::Initialize({ .WorkerThreads = PhysicsEngineSpecification::MaxWorkerThreads + 1 });
			REQUIRE_FALSE(tooMany.has_value());
			CHECK(tooMany.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(PhysicsEngine::IsInitialized());

			// Initialized again (here with no worker thread: every job runs on the stepping thread), the engine works.
			REQUIRE(PhysicsEngine::Initialize({ .WorkerThreads = 0 }).has_value());
			CHECK(PhysicsEngine::GetWorkerThreadCount() == 0);
			Result<Scope<PhysicsWorld>> restarted = PhysicsWorld::Create({});
			REQUIRE(restarted.has_value());
			CHECK((*restarted)->Step(1.0f / 60.0f, 1).has_value());
			restarted->reset();
			// Shut down again, so the guard initializes the engine with the count the process started with.
			PhysicsEngine::Shutdown();
		}

		TEST_CASE("PhysicsEngine: initializing twice or shutting down under a live world is a programmer error that asserts")
		{
			ENGINE_CHECK_DEATH("Physics/EngineInitializedTwice", "already initialized");
			ENGINE_CHECK_DEATH("Physics/EngineShutDownUnderLiveWorld", "destroy every PhysicsWorld first");
		}
	}

}
