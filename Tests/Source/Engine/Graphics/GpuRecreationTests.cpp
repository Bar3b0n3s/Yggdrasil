#include "TestsPCH.h"

#include "Engine/Graphics/GpuRecreation.h"

namespace Engine {

	namespace {

		// Hooks that log "release <name>" and "recreate <name>", and whose Recreate fails when `fail` is set.
		GpuRecreationHooks MakeLoggingHooks(std::vector<std::string>& calls, std::string name, bool fail = false)
		{
			std::vector<std::string>* log = &calls;
			GpuRecreationHooks hooks;
			hooks.Name = name;
			hooks.Release = [log, name]()
			{
				log->push_back("release " + name);
			};
			hooks.Recreate = [log, name, fail]() -> Status
			{
				log->push_back("recreate " + name);
				if (fail)
					return MakeError(ErrorCode::Gpu, "cannot create the owner's objects");
				return {};
			};
			return hooks;
		}

	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GpuRecreationRegistry: Release runs last registered first, and Recreate first registered first")
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decision 26): §8.14 item 4's teardown order, and
			// creation again in construction order.
			GpuRecreationRegistry registry;
			std::vector<std::string> calls;
			GpuRecreationRegistration cache = registry.Register(MakeLoggingHooks(calls, "GpuResourceCache"));
			GpuRecreationRegistration pipelines = registry.Register(MakeLoggingHooks(calls, "SceneRendererPipelines"));
			GpuRecreationRegistration renderer = registry.Register(MakeLoggingHooks(calls, "SceneRenderer"));
			CHECK(cache.IsRegistered());
			CHECK(registry.GetOwnerCount() == 3);
			CHECK(registry.GetOwnerNames() == std::vector<std::string>{ "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" });

			registry.ReleaseAll();
			const Status recreated = registry.RecreateAll();
			REQUIRE_MESSAGE(recreated.has_value(), recreated.error().ToString());
			CHECK(calls == std::vector<std::string>{
					  "release SceneRenderer",
					  "release SceneRendererPipelines",
					  "release GpuResourceCache",
					  "recreate GpuResourceCache",
					  "recreate SceneRendererPipelines",
					  "recreate SceneRenderer",
				  });
			CHECK_FALSE(registry.IsRunning());
		}

		TEST_CASE("GpuRecreationRegistry: a dropped registration is never called, and Recreate stops at the first error")
		{
			// M18 acceptance (C18.3, decision 26): a registration lives as long as its owner, moves with it, and an owner's
			// failed Recreate fails the recovery, naming the owner, with the owners after it left released.
			GpuRecreationRegistry registry;
			std::vector<std::string> calls;
			{
				const GpuRecreationRegistration gone = registry.Register(MakeLoggingHooks(calls, "Gone"));
				CHECK(registry.GetOwnerCount() == 1);
			}
			CHECK(registry.GetOwnerCount() == 0);
			GpuRecreationRegistration moved = registry.Register(MakeLoggingHooks(calls, "Swapchain"));
			GpuRecreationRegistration owner = std::move(moved);
			CHECK(owner.IsRegistered());
			CHECK(registry.GetOwnerCount() == 1);
			GpuRecreationRegistration failing = registry.Register(MakeLoggingHooks(calls, "ImGuiRenderer", true));
			GpuRecreationRegistration after = registry.Register(MakeLoggingHooks(calls, "EditorHostViewports"));
			GpuRecreationRegistration released = registry.Register(MakeLoggingHooks(calls, "Released"));
			released.Reset();
			CHECK_FALSE(released.IsRegistered());
			CHECK(registry.GetOwnerNames() == std::vector<std::string>{ "Swapchain", "ImGuiRenderer", "EditorHostViewports" });

			registry.ReleaseAll();
			const Status recreated = registry.RecreateAll();
			REQUIRE_FALSE(recreated.has_value());
			CHECK(recreated.error().GetCode() == ErrorCode::Gpu);
			CHECK(recreated.error().ToString().contains("ImGuiRenderer"));
			CHECK(calls == std::vector<std::string>{
					  "release EditorHostViewports",
					  "release ImGuiRenderer",
					  "release Swapchain",
					  "recreate Swapchain",
					  "recreate ImGuiRenderer",
				  });
			owner.Reset();
			failing.Reset();
			after.Reset();
			CHECK(registry.GetOwnerCount() == 0);
		}

		TEST_CASE("GpuRecreationRegistry: TrimUnused calls every trim hook, last registered first, and skips owners without one"
			* doctest::skip(true))
		{
			// M18 acceptance (C18.4, Docs/Decisions/0029-m18-contract.md decision 43): an owner trims before the owners it
			// uses, as Release runs, an owner without a TrimUnused hook is skipped, and trimming releases nothing else.
			GpuRecreationRegistry registry;
			std::vector<std::string> calls;
			std::vector<std::string>* log = &calls;
			const auto withTrim = [log](GpuRecreationHooks hooks)
			{
				const std::string name = hooks.Name;
				hooks.TrimUnused = [log, name]()
				{
					log->push_back("trim " + name);
				};
				return hooks;
			};
			GpuRecreationRegistration cache = registry.Register(withTrim(MakeLoggingHooks(calls, "GpuResourceCache")));
			GpuRecreationRegistration pipelines = registry.Register(MakeLoggingHooks(calls, "SceneRendererPipelines"));
			GpuRecreationRegistration renderer = registry.Register(withTrim(MakeLoggingHooks(calls, "SceneRenderer")));

			CHECK(registry.TrimUnused() == 2);
			CHECK(calls == std::vector<std::string>{ "trim SceneRenderer", "trim GpuResourceCache" });
			CHECK_FALSE(registry.IsRunning());

			// A dropped owner is never trimmed.
			renderer.Reset();
			calls.clear();
			CHECK(registry.TrimUnused() == 1);
			CHECK(calls == std::vector<std::string>{ "trim GpuResourceCache" });
			cache.Reset();
			pipelines.Reset();
			CHECK(registry.TrimUnused() == 0);
		}
	}

}
