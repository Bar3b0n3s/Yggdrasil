#include "TestsPCH.h"
#include "Engine/Scripting/Watchdog.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Random.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace Engine {

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Watchdog: infinite loop raises timeout")
		{
			Random random(19);
			double now = 0.0;
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.ClockSeconds = [&now]
			{
				now += 0.02;
				return now;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Scripts/Spin.luau");
			REQUIRE(path.has_value());
			const Status result = (*sandbox)->RunSource(*path, "while true do end");
			REQUIRE_FALSE(result.has_value());
			CHECK(result.error().GetCode() == ErrorCode::Timeout);
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Kind == ScriptErrorKind::Timeout);
			CHECK(error->Line == 1);
			// §11.1's text names "<script>.<callback>"; a module top level has no callback.
			CHECK(error->Message == "script exceeded 1000 ms in Spin.<module top> (possible infinite loop)");
		}

		TEST_CASE("Watchdog: a runaway callback names its script and callback")
		{
			double now = 0.0;
			bool armed = false;
			Test::ScriptTestFixtureSpecification specification{};
			specification.ClockSeconds = [&now, &armed]
			{
				if (armed)
					now += 0.02;
				return now;
			};
			Test::ScriptTestFixture fixture(specification);
			REQUIRE(fixture.AddScript(AssetHandle(211), "Assets/Scripts/Board.luau", "local Board = {}\nfunction Board:OnFixedUpdate()\n\twhile true do end\nend\nreturn Script.Define(\"Board\", Board)"));
			fixture.GetScene().CreateEntity("Board").AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(AssetHandle(211));
			REQUIRE(fixture.Start());
			armed = true;
			Test::ExpectLog expected(LogLevel::Error, "script exceeded 1000 ms in Board.OnFixedUpdate (possible infinite loop)");
			fixture.GetEngine()->FixedUpdate();
			CHECK(expected.GetMatchCount() == 1);
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Kind == ScriptErrorKind::Timeout);
			CHECK(fixture.Errors[0].Message == "script exceeded 1000 ms in Board.OnFixedUpdate (possible infinite loop)");
			CHECK(fixture.Errors[0].Line == 3);
		}

		TEST_CASE("ScriptWatchdog: caught timeout stays latched and message text cannot spoof the error kind")
		{
			Random random(59);
			double now = 0.0;
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.IsTestRun = true;
			specification.CallbackBudgetMs = 1000;
			specification.ClockSeconds = [&now]
			{
				now += 0.02;
				return now;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/CaughtTimeout.luau");
			REQUIRE(path.has_value());
			CHECK_FALSE((*sandbox)->Evaluate("pcall(function() while true do end end); return 1", *path).has_value());
			const auto timeout = (*sandbox)->GetLastError();
			REQUIRE(timeout.has_value());
			CHECK(timeout->Kind == ScriptErrorKind::Timeout);
			CHECK_FALSE((*sandbox)->Evaluate("error('possible infinite loop; memory limit exceeded')", *path).has_value());
			const auto runtime = (*sandbox)->GetLastError();
			REQUIRE(runtime.has_value());
			CHECK(runtime->Kind == ScriptErrorKind::Runtime);
			CHECK((*sandbox)->Evaluate("return 1", *path).has_value());
			CHECK_FALSE((*sandbox)->GetLastError().has_value());
		}

		TEST_CASE("Watchdog: no error raised from a GC interrupt")
		{
			// A real VM: Luau calls the interrupt with gc >= 0 at every GC step. The engine's handler must return at once,
			// without sampling the safety clock or raising, even when the deadline has passed.
			Random random(61);
			uint64_t samples = 0;
			uint64_t expireAfter = std::numeric_limits<uint64_t>::max();
			double offset = 0.0; // keeps the clock monotonic across evaluations, as the watchdog requires
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.IsTestRun = true;
			specification.CallbackBudgetMs = 1000;
			specification.MemoryLimitMB = 16;
			specification.ClockSeconds = [&samples, &expireAfter, &offset]
			{
				return offset + (++samples > expireAfter ? 5.0 : 0.0);
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Scripts/Garbage.luau");
			REQUIRE(path.has_value());
			// Identical opcodes (one CALL and one loop back-edge per iteration), so identical non-GC interrupts. The first
			// discards 128 MiB of strings, which forces many GC steps; the second allocates nothing.
			const std::string_view allocating = "for i = 1, 2000 do local s = string.rep('x', 65536) end\nreturn true";
			const std::string_view plain = "for i = 1, 2000 do local s = string.len('x') end\nreturn true";
			const auto count = [&sandbox, &samples, &offset, &path](std::string_view source) -> std::pair<bool, uint64_t>
			{
				offset += 10.0;
				samples = 0;
				const bool succeeded = (*sandbox)->Evaluate(source, *path).has_value();
				return { succeeded, samples };
			};
			const auto garbage = count(allocating);
			CHECK(garbage.first);
			CHECK((*sandbox)->GetMemoryState().PeakBytes < 16ull * 1024 * 1024); // collected while running: GC steps ran
			const auto quiet = count(plain);
			CHECK(quiet.first);
			CHECK(garbage.second == quiet.second); // no GC step sampled the clock
			// With the deadline expired halfway, both fail at the same non-GC interrupt, located in the loop.
			expireAfter = quiet.second / 2;
			const auto expiredGarbage = count(allocating);
			auto error = (*sandbox)->GetLastError();
			CHECK_FALSE(expiredGarbage.first);
			REQUIRE(error.has_value());
			CHECK(error->Kind == ScriptErrorKind::Timeout);
			CHECK(error->Line == 1);
			const auto expiredQuiet = count(plain);
			CHECK_FALSE(expiredQuiet.first);
			CHECK(expiredGarbage.second == expiredQuiet.second);
			CHECK_FALSE((*sandbox)->IsStopped());
		}

		TEST_CASE("Watchdog: OnCreate inside Instantiate inherits the outer deadline")
		{
			double now = 0.0;
			Test::ScriptTestFixtureSpecification specification{};
			specification.Settings.CallbackBudgetMs = 1000;
			specification.ClockSeconds = [&now]
			{
				return now;
			};
			Test::ScriptTestFixture fixture(specification);
			// Each host request made inside the running callback advances the safety clock by 0.6 s.
			fixture.OnHostRequest = [&now](std::string_view)
			{
				now += 0.6;
			};
			REQUIRE(fixture.AddScript(AssetHandle(201), "Assets/Scripts/Child.luau", R"(local Child = {}
function Child:OnCreate()
	Scene.CreateEntity("Marker")
	for _ = 1, 3 do end
	self.Created = true
end
return Script.Define("Child", Child))"));
			{
				Test::SceneTestFixture source;
				const Entity root = source.GetScene().CreateEntity("ChildRoot");
				root.AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(AssetHandle(201));
				const auto prefab = Prefab::CreateFromEntity(root, "ChildPrefab");
				REQUIRE(prefab.has_value());
				const auto document = prefab->ToJson();
				REQUIRE(document.has_value());
				auto data = CreateRef<PrefabData>();
				data->Document = CreateRef<const Json>(*document);
				fixture.GetAssetManager().Publish(AssetHandle(202), data, "Assets/Prefabs/Child.prefab");
			}
			REQUIRE(fixture.AddScript(AssetHandle(203), "Assets/Scripts/Spawner.luau", R"(local Spawner = {}
function Spawner:OnFixedUpdate()
	if self.Spawned then return end
	self.Spawned = true
	Scene.Instantiate(Assets.Load("Assets/Prefabs/Child.prefab"))
	self.AfterInstantiate = Time.GetTick()
end
return Script.Define("Spawner", Spawner))"));
			const UUID spawner = fixture.GetScene().CreateEntity("Spawner").GetUUID();
			fixture.GetScene().FindEntityByID(spawner).AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(AssetHandle(203));
			REQUIRE(fixture.Start());
			// The callback enters at 0 s (deadline 1 s). Instantiate's host request moves the clock to 0.6 s before the
			// child's OnCreate enters; its own request then moves it to 1.2 s. A fresh nested deadline (1.6 s) would let
			// OnCreate finish; the inherited one ends it at its next interrupt.
			{
				Test::ExpectLog expected(LogLevel::Error, "script exceeded 1000 ms in Child.OnCreate (possible infinite loop)");
				fixture.GetEngine()->FixedUpdate();
				CHECK(expected.GetMatchCount() == 1);
			}
			REQUIRE(fixture.Errors.size() == 1);
			const auto& error = fixture.Errors[0];
			CHECK(error.Kind == ScriptErrorKind::Timeout);
			CHECK(error.Script == "Assets/Scripts/Child.luau");
			CHECK(error.Callback == "OnCreate");
			CHECK(error.Line >= 3);
			CHECK(error.Line <= 5);
			CHECK(now == doctest::Approx(1.2));
			// The timeout stays latched for the whole slice: the outer callback stopped too.
			const auto instances = fixture.GetEngine()->GetInstances();
			const auto child = std::ranges::find_if(instances, [&fixture](const ScriptInstanceInfo& instance)
			{
				return fixture.GetScene().FindEntityByID(instance.Entity).GetName() == "ChildRoot";
			});
			REQUIRE(child != instances.end());
			CHECK(child->Disabled);
			fixture.OnHostRequest = {};
			const auto after = fixture.Evaluate("return self.AfterInstantiate == nil", spawner);
			REQUIRE_MESSAGE(after.has_value(), (after ? "" : after.error().ToString()));
			CHECK(after->Value.Get() == Json(true));
		}

		TEST_CASE("ScriptWatchdog: independent resumes restart the deadline and invalid entries do not alter depth")
		{
			ScriptWatchdog watchdog;
			REQUIRE(watchdog.Enter(250, 1.0).has_value());
			CHECK(watchdog.CheckInterrupt(-1, 1.25));
			watchdog.Leave();
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 2.0));
			REQUIRE(watchdog.Enter(250, 2.0).has_value());
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 2.1));
			CHECK_FALSE(watchdog.Enter(0, 2.1).has_value());
			CHECK_FALSE(watchdog.Enter(250, 1.0).has_value());
			CHECK(watchdog.GetDepth() == 1);
			watchdog.Leave();
		}

		TEST_CASE("ScriptWatchdog: callback budgets enforce the minimum and preserve exact test overrides")
		{
			const auto invalid = ScriptWatchdog::ResolveCallbackBudget(9, true);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(ScriptWatchdog::ResolveCallbackBudget(10, true).value_or(0) == 10);
			CHECK(ScriptWatchdog::ResolveCallbackBudget(9000, false).value_or(0) == 9000);
#if defined(ENGINE_DIST)
			CHECK(ScriptWatchdog::ResolveCallbackBudget(1000, false).value_or(0) == 5000);
#else
			CHECK(ScriptWatchdog::ResolveCallbackBudget(1000, false).value_or(0) == 1000);
#endif
			// Tests has no Dist target: exported FeatureTest must exercise the Dist policy too.
		}
	}

}
