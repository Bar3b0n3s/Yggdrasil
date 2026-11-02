#include "TestsPCH.h"
#include "Engine/Scripting/Watchdog.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Random.h"
#include "Engine/Platform/ThreadClock.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Engine {

	namespace {

		// Keeps the calling thread busy until its own CPU clock has advanced by `seconds`.
		void SpendCpu(double seconds)
		{
			const double start = ThreadClock::GetCpuSeconds();
			std::atomic<uint64_t> spins{ 0 };
			while (ThreadClock::GetCpuSeconds() - start < seconds)
				spins.fetch_add(1, std::memory_order_relaxed);
		}

		// An entity that runs the published behaviour `script` in its first slot.
		[[nodiscard]] UUID AddBehaviour(Test::ScriptTestFixture& fixture, std::string_view name, AssetHandle script)
		{
			Entity entity = fixture.GetScene().CreateEntity(std::string(name));
			entity.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(script), .Fields = {}, .ExecutionOrder = 0 });
			return entity.GetUUID();
		}

	}

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
			fixture.GetScene().CreateEntity("Board").AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(AssetHandle(211)), .Fields = {}, .ExecutionOrder = 0 });
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

		// Docs/Decisions/0039-script-budget-cpu-time.md: on a loaded machine a legitimate Game.OnStart was descheduled past its
		// 1,000 ms of wall time and timed out, so test outcomes and replay hashes depended on how busy the machine was.
		TEST_CASE("Watchdog: a callback whose wall time passes its budget while its CPU time stays below it finishes")
		{
			double wall = 0.0;
			double cpu = 0.0;
			Test::ScriptTestFixtureSpecification specification{};
			specification.Settings.CallbackBudgetMs = 1000;
			specification.ClockSeconds = [&wall]
			{
				return wall;
			};
			specification.CpuClockSeconds = [&cpu]
			{
				return cpu;
			};
			Test::ScriptTestFixture fixture(specification);
			// Each host request made inside the callback stands for the thread being descheduled: 0.6 s of wall time pass
			// while it spends 0.01 s of CPU time.
			fixture.OnHostRequest = [&wall, &cpu](std::string_view)
			{
				wall += 0.6;
				cpu += 0.01;
			};
			REQUIRE(fixture.AddScript(AssetHandle(221), "Assets/Scripts/Game.luau", R"(local Game = {}
function Game:OnStart()
	for _ = 1, 4 do
		Scene.CreateEntity("Piece")
	end
	self.Started = true
end
return Script.Define("Game", Game))"));
			const UUID game = AddBehaviour(fixture, "Game", AssetHandle(221));
			REQUIRE(fixture.Start());
			CHECK(fixture.Errors.empty());
			CHECK(wall == doctest::Approx(2.4));
			fixture.OnHostRequest = {};
			const auto started = fixture.Evaluate("return self.Started == true", game);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			CHECK(started->Value.Get() == Json(true));
		}

		// The default clocks, lua_clock compared first and the thread's CPU time counted: a callback that blocks while another
		// thread works spends none of its own CPU time, however long the wait takes.
		TEST_CASE("Watchdog: a callback that waits for another thread's work is not timed out by the wait")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.Settings.CallbackBudgetMs = 100;
			Test::ScriptTestFixture fixture(specification);
			fixture.OnHostRequest = [](std::string_view)
			{
				// A quarter of a second of the worker's CPU time, so at least that much wall time less one accounting tick (about
				// 15.6 ms on Windows), more than twice the budget, while this thread only waits for it.
				std::thread worker([]
				{
					SpendCpu(0.25);
				});
				worker.join();
			};
			REQUIRE(fixture.AddScript(AssetHandle(222), "Assets/Scripts/Loader.luau", R"(local Loader = {}
function Loader:OnStart()
	Scene.CreateEntity("Level")
	for _ = 1, 3 do end
	self.Started = true
end
return Script.Define("Loader", Loader))"));
			const UUID loader = AddBehaviour(fixture, "Loader", AssetHandle(222));
			REQUIRE(fixture.Start());
			CHECK(fixture.Errors.empty());
			fixture.OnHostRequest = {};
			const auto started = fixture.Evaluate("return self.Started == true", loader);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			CHECK(started->Value.Get() == Json(true));
		}

		TEST_CASE("Watchdog: a runaway loop whose CPU time passes its budget still times out")
		{
			// A loaded machine gives the loop half of its thread's time: each wall sample advances 0.02 s, the CPU time 0.01 s.
			double wall = 0.0;
			double cpu = 0.0;
			bool armed = false;
			Test::ScriptTestFixtureSpecification specification{};
			specification.ClockSeconds = [&wall, &cpu, &armed]
			{
				if (armed)
				{
					wall += 0.02;
					cpu += 0.01;
				}
				return wall;
			};
			specification.CpuClockSeconds = [&cpu]
			{
				return cpu;
			};
			Test::ScriptTestFixture fixture(specification);
			REQUIRE(fixture.AddScript(AssetHandle(223), "Assets/Scripts/Board.luau", "local Board = {}\nfunction Board:OnFixedUpdate()\n\twhile true do end\nend\nreturn Script.Define(\"Board\", Board)"));
			static_cast<void>(AddBehaviour(fixture, "Board", AssetHandle(223)));
			REQUIRE(fixture.Start());
			armed = true;
			Test::ExpectLog expected(LogLevel::Error, "script exceeded 1000 ms in Board.OnFixedUpdate (possible infinite loop)");
			fixture.GetEngine()->FixedUpdate();
			CHECK(expected.GetMatchCount() == 1);
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Kind == ScriptErrorKind::Timeout);
			CHECK(fixture.Errors[0].Callback == "OnFixedUpdate");
			CHECK(fixture.Errors[0].Line == 3);
			// The CPU budget ended it, after twice the budget had passed on the wall clock.
			CHECK(cpu >= 1.0);
			CHECK(wall >= 2.0);
		}

		TEST_CASE("Watchdog: a GC interrupt samples neither clock")
		{
			// Every non-GC interrupt finds the wall deadline passed (each wall sample advances 2 s, twice the budget) with CPU
			// budget left (the CPU clock stands still), so each samples the CPU clock; Luau's GC-step interrupts must not.
			Random random(67);
			double wall = 0.0;
			uint64_t samples = 0;
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.IsTestRun = true;
			specification.CallbackBudgetMs = 1000;
			specification.MemoryLimitMB = 16;
			specification.ClockSeconds = [&wall]
			{
				wall += 2.0;
				return wall;
			};
			specification.CpuClockSeconds = [&samples]
			{
				++samples;
				return 0.0;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Scripts/Garbage.luau");
			REQUIRE(path.has_value());
			const auto count = [&sandbox, &samples, &path](std::string_view source) -> std::pair<bool, uint64_t>
			{
				samples = 0;
				const bool succeeded = (*sandbox)->Evaluate(source, *path).has_value();
				return { succeeded, samples };
			};
			// Identical opcodes, so identical non-GC interrupts; the first discards 128 MiB of strings, forcing many GC steps.
			const auto garbage = count("for i = 1, 2000 do local s = string.rep('x', 65536) end\nreturn true");
			CHECK(garbage.first);
			CHECK((*sandbox)->GetMemoryState().PeakBytes < 16ull * 1024 * 1024); // collected while running: GC steps ran
			const auto quiet = count("for i = 1, 2000 do local s = string.len('x') end\nreturn true");
			CHECK(quiet.first);
			CHECK(quiet.second >= 2000); // every iteration's interrupt sampled the CPU clock
			CHECK(garbage.second == quiet.second);
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
				root.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(AssetHandle(201)), .Fields = {}, .ExecutionOrder = 0 });
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
			fixture.GetScene().FindEntityByID(spawner).AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(AssetHandle(203)), .Fields = {}, .ExecutionOrder = 0 });
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

		// ADR 0022 "Integration": host work inside a protected call (a screenshot capture, a test reimport) moves the deadline
		// later by its duration; Luau the host work runs after all, and Luau before and after it, still counts.
		TEST_CASE("ScriptWatchdog: host time moves the deadline while Luau time and latched timeouts still count")
		{
			ScriptWatchdog watchdog;
			// Idle: nothing to move.
			watchdog.BeginHostTime(0.0);
			watchdog.EndHostTime(9.0);
			CHECK(watchdog.GetDeadlineSeconds() == 0.0);

			// 0.4 s of Luau, 5 s of host work, then 0.1 s of Luau reach the 0.5 s budget.
			REQUIRE(watchdog.Enter(500, 10.0).has_value());
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 10.4));
			watchdog.BeginHostTime(10.4);
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 13.0)); // inside the host work the deadline moves with the clock
			watchdog.EndHostTime(15.4);
			CHECK(watchdog.GetDeadlineSeconds() == doctest::Approx(15.5));
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 15.45));
			CHECK(watchdog.CheckInterrupt(-1, 15.5));
			watchdog.Leave();

			// Nested periods credit once; a protected entry inside a period ends the excluded time.
			REQUIRE(watchdog.Enter(500, 20.0).has_value());
			watchdog.BeginHostTime(20.1);
			watchdog.BeginHostTime(20.2);
			watchdog.EndHostTime(21.1);
			CHECK(watchdog.GetDeadlineSeconds() == doctest::Approx(20.5)); // the inner end credits nothing
			REQUIRE(watchdog.Enter(500, 22.1).has_value());                // Luau the host work ran: 2 s were host time
			CHECK(watchdog.GetDeadlineSeconds() == doctest::Approx(22.5));
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 22.4));
			watchdog.Leave();
			watchdog.EndHostTime(30.0); // the rest of the period counts
			CHECK(watchdog.GetDeadlineSeconds() == doctest::Approx(22.5));
			CHECK(watchdog.CheckInterrupt(-1, 30.0));
			watchdog.Leave();

			// A latched timeout stays latched, and host work that starts after the deadline cannot hide it.
			REQUIRE(watchdog.Enter(500, 40.0).has_value());
			CHECK(watchdog.CheckInterrupt(-1, 41.0));
			watchdog.BeginHostTime(41.0);
			watchdog.EndHostTime(50.0);
			CHECK(watchdog.CheckInterrupt(-1, 50.0));
			watchdog.Leave();
			REQUIRE(watchdog.Enter(500, 60.0).has_value());
			watchdog.BeginHostTime(61.0);
			CHECK(watchdog.CheckInterrupt(-1, 62.0));
			watchdog.EndHostTime(63.0);
			CHECK(watchdog.CheckInterrupt(-1, 63.0));
			watchdog.Leave();
			CHECK(watchdog.GetDepth() == 0);
		}

		// Docs/Decisions/0039-script-budget-cpu-time.md: the budget counts the script thread's CPU time, and the wall clock is
		// only the fast path that tells when to look at it. The samples below are binary fractions, so every sum is exact.
		TEST_CASE("ScriptWatchdog: past the wall deadline a callback with CPU budget left continues and one without expires")
		{
			double cpu = 1.0;
			uint32_t samples = 0;
			ScriptWatchdog watchdog;
			watchdog.SetCpuClock([&cpu, &samples]
			{
				++samples;
				return cpu;
			});
			REQUIRE(watchdog.Enter(500, 10.0).has_value());
			CHECK(samples == 1); // the budget's CPU time counts from the outermost entry
			// Before the wall deadline an interrupt is one comparison.
			cpu = 1.125;
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 10.25));
			CHECK(samples == 1);
			// Descheduled: 2 s of wall time, 0.125 s of CPU time. The deadline moves to now plus the 0.375 s left.
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 12.0));
			CHECK(samples == 2);
			CHECK(watchdog.GetDeadlineSeconds() == 12.375);
			cpu = 1.25;
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 12.25));
			CHECK(samples == 2);
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 12.375));
			CHECK(samples == 3);
			CHECK(watchdog.GetDeadlineSeconds() == 12.625);
			// 0.5 s of CPU time spent: the timeout latches, and the latch samples nothing more.
			cpu = 1.5;
			CHECK(watchdog.CheckInterrupt(-1, 12.625));
			CHECK(samples == 4);
			CHECK(watchdog.CheckInterrupt(-1, 12.75));
			CHECK(samples == 4);
			watchdog.Leave();
			// The next outermost entry, a task or test-case resume, starts a fresh budget.
			cpu = 1.75;
			REQUIRE(watchdog.Enter(500, 20.0).has_value());
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 20.25));
			watchdog.Leave();
		}

		TEST_CASE("ScriptWatchdog: host work's CPU time is excluded from the budget")
		{
			double cpu = 0.0;
			ScriptWatchdog watchdog;
			watchdog.SetCpuClock([&cpu]
			{
				return cpu;
			});
			// 0.25 s of Luau, host work taking 5 s of wall time and 4 s of the thread's CPU time, then Luau again: only the
			// Luau counts, so the 0.5 s budget is spent 0.25 s of CPU time after the host work.
			REQUIRE(watchdog.Enter(500, 10.0).has_value());
			cpu = 0.25;
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 10.25));
			watchdog.BeginHostTime(10.25);
			cpu = 4.25;
			watchdog.EndHostTime(15.25);
			CHECK(watchdog.GetDeadlineSeconds() == 15.5);
			cpu = 4.375;
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 15.625));
			CHECK(watchdog.GetDeadlineSeconds() == 15.75);
			cpu = 4.5;
			CHECK(watchdog.CheckInterrupt(-1, 15.75));
			watchdog.Leave();

			// A host period that begins past the wall deadline with CPU budget left moves the deadline first, so the period's
			// interrupts fire only for a budget that was really spent.
			cpu = 10.0;
			REQUIRE(watchdog.Enter(500, 20.0).has_value());
			cpu = 10.125;
			watchdog.BeginHostTime(23.0);
			CHECK(watchdog.GetDeadlineSeconds() == 23.375);
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 30.0));
			cpu = 12.0;
			watchdog.EndHostTime(30.0);
			CHECK(watchdog.GetDeadlineSeconds() == 30.375);
			cpu = 12.25;
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 30.25));
			cpu = 12.5;
			CHECK(watchdog.CheckInterrupt(-1, 30.375)); // 0.125 s of CPU time before the host work and 0.5 s after it
			watchdog.Leave();

			// Luau the host work runs after all counts: a protected entry inside the period ends the excluded CPU time.
			cpu = 20.0;
			REQUIRE(watchdog.Enter(500, 40.0).has_value());
			watchdog.BeginHostTime(40.0);
			cpu = 22.0;
			REQUIRE(watchdog.Enter(500, 42.0).has_value());
			cpu = 22.625;
			CHECK(watchdog.CheckInterrupt(-1, 42.625));
			watchdog.Leave();
			watchdog.EndHostTime(43.0);
			watchdog.Leave();
			CHECK(watchdog.GetDepth() == 0);
		}

		TEST_CASE("ScriptWatchdog: a nested entry cannot extend the outer CPU budget")
		{
			double cpu = 0.0;
			ScriptWatchdog watchdog;
			watchdog.SetCpuClock([&cpu]
			{
				return cpu;
			});
			REQUIRE(watchdog.Enter(500, 10.0).has_value());
			cpu = 0.25;
			REQUIRE(watchdog.Enter(5000, 10.25).has_value()); // a nested callback that asks for 5 s
			CHECK(watchdog.GetDeadlineSeconds() == 10.5);
			cpu = 0.375;
			CHECK_FALSE(watchdog.CheckInterrupt(-1, 11.0)); // descheduled: 0.375 s of the outer 0.5 s used
			cpu = 0.5;
			CHECK(watchdog.CheckInterrupt(-1, 11.125));
			watchdog.Leave();
			CHECK(watchdog.CheckInterrupt(-1, 11.25)); // the latch outlives the nested scope
			watchdog.Leave();
		}

		TEST_CASE("ScriptWatchdog: a gc interrupt samples no clock and invalid CPU samples fail closed")
		{
			constexpr double NotANumber = std::numeric_limits<double>::quiet_NaN();
			double cpu = 0.0;
			uint32_t samples = 0;
			ScriptWatchdog watchdog;
			watchdog.SetCpuClock([&cpu, &samples]
			{
				++samples;
				return cpu;
			});
			const auto refusal = [&watchdog](double nowSeconds) -> std::optional<ErrorCode>
			{
				const Status entered = watchdog.Enter(500, nowSeconds);
				if (!entered)
					return entered.error().GetCode();
				watchdog.Leave();
				return std::nullopt;
			};
			REQUIRE(watchdog.Enter(500, 10.0).has_value());
			CHECK_FALSE(watchdog.CheckInterrupt(0, 99.0));
			CHECK_FALSE(watchdog.CheckInterrupt(3, NotANumber));
			CHECK(samples == 1);
			// A non-finite CPU sample past the wall deadline expires the callback.
			cpu = NotANumber;
			CHECK(watchdog.CheckInterrupt(-1, 11.0));
			watchdog.Leave();
			// An entry refuses a non-finite, negative or backwards CPU sample without entering.
			CHECK(refusal(20.0) == ErrorCode::InvalidArgument);
			cpu = -1.0;
			CHECK(refusal(20.0) == ErrorCode::InvalidArgument);
			cpu = 2.0;
			REQUIRE(watchdog.Enter(500, 20.0).has_value());
			// A backwards CPU sample past the wall deadline expires the callback.
			cpu = 1.0;
			CHECK(watchdog.CheckInterrupt(-1, 21.0));
			watchdog.Leave();
			CHECK(refusal(30.0) == ErrorCode::InvalidState);
			CHECK(watchdog.GetDepth() == 0);
			// A host period whose first CPU sample is invalid credits nothing: its 3 s of CPU time count.
			cpu = 3.0;
			REQUIRE(watchdog.Enter(500, 40.0).has_value());
			cpu = NotANumber;
			watchdog.BeginHostTime(40.25);
			cpu = 6.0;
			watchdog.EndHostTime(45.0);
			CHECK(watchdog.CheckInterrupt(-1, 45.25));
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
