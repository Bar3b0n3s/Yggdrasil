#include "TestsPCH.h"
#include "Engine/Scripting/TaskScheduler.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <limits>

#include <vector>

namespace Engine {

	namespace Test {

		void RunTaskBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			fixture.Frame.FixedDeltaTime = 0.25;
			const Entity target = fixture.GetScene().CreateEntity("Target");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local target = Scene.FindByName("Target")
Task.Spawn(function()
	target.Name = "A"
	Task.Wait(0)
	target.Name = "B"
	Task.WaitTicks(2)
	target.Name ..= "C"
end)
assert(target.Name == "A")
Task.Delay(0, function() target.Name ..= "D" end)
local cancelled = Task.Delay(0.25, function() target.Name = "cancelled" end)
Task.Cancel(cancelled)
Task.Cancel(cancelled)
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(target.GetName() == "A");
			fixture.GetEngine()->ResumeTasks();
			CHECK(target.GetName() == "A");
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(target.GetName() == "BD");
			fixture.Frame.Tick = 2;
			fixture.GetEngine()->ResumeTasks();
			CHECK(target.GetName() == "BD");
			fixture.Frame.Tick = 3;
			fixture.GetEngine()->ResumeTasks();
			CHECK(target.GetName() == "BDC");
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Task")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("TaskBindings: spawn is immediate while waits and delays follow simulation ticks")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunTaskBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("TaskBindings: fractional delays round up and completed handles cancel harmlessly")
		{
			Test::ScriptTestFixture fixture;
			fixture.Frame.FixedDeltaTime = 0.25;
			const Entity target = fixture.GetScene().CreateEntity("Target");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local target = Scene.FindByName("Target")
Task.Cancel(Task.Spawn(function() end))
Task.Delay(0.26, function() target.Name = "Delayed" end)
return true
)");
			REQUIRE(result.has_value());
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(target.GetName() == "Target");
			fixture.Frame.Tick = 2;
			fixture.GetEngine()->ResumeTasks();
			CHECK(target.GetName() == "Delayed");
		}

		TEST_CASE("TaskBindings: malformed waits and handles never start tasks")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(not pcall(Task.Wait, 0))
assert(not pcall(Task.WaitTicks, 1))
for _, value in {-1, math.huge, 0/0, "1"} do
	assert(not pcall(Task.Delay, value, function() error("ran") end))
	assert(not pcall(Task.Wait, value))
end
for _, value in {-1, 0.5, 9007199254740992} do assert(not pcall(Task.WaitTicks, value)) end
assert(not pcall(Task.Spawn, {}) and not pcall(Task.Delay, 0, 3))
assert(not pcall(Task.Cancel, {}) and not pcall(Task.Cancel, Quat.Identity()))
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.ExternalMutations.empty());
			fixture.Frame.Tick = std::numeric_limits<uint64_t>::max();
			const auto overflow = fixture.Evaluate("return not pcall(Task.Delay, 0, function() end)");
			REQUIRE(overflow.has_value());
			CHECK(overflow->Value.Get() == Json(true));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
		}

		TEST_CASE("TaskBindings: read-only evaluation refuses scheduling before invoking callbacks")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local ran = false
assert(not pcall(Task.Spawn, function() ran = true end))
assert(not pcall(Task.Delay, 0, function() ran = true end))
assert(not pcall(Task.Wait, 0))
assert(not pcall(Task.WaitTicks, 0))
return not ran
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.ExternalMutations.empty());
		}

		TEST_CASE("TaskBindings: cross-script spawning belongs to the caller rather than the receiver")
		{
			for (const bool destroyCaller : { true, false })
			{
				Test::ScriptTestFixture fixture;
				const AssetHandle receiverScript(701);
				const AssetHandle callerScript(702);
				REQUIRE(fixture.AddScript(receiverScript, "Assets/Receiver.luau", R"(
local Receiver = {}
function Receiver.Schedule(self)
	Task.Delay(0.1, function() Scene.FindByName("Target").Name = "Ran" end)
end
return Script.Define("Receiver", Receiver)
)")
						.has_value());
				REQUIRE(fixture.AddScript(callerScript, "Assets/Caller.luau", R"(
local Caller = {}
function Caller.OnStart(self)
	Scene.FindByName("Receiver"):GetScript():Schedule()
end
return Script.Define("Caller", Caller)
)")
						.has_value());
				const Entity receiver = fixture.GetScene().CreateEntity("Receiver");
				receiver.AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(receiverScript);
				const Entity caller = fixture.GetScene().CreateEntity("Caller");
				caller.AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(callerScript);
				const Entity target = fixture.GetScene().CreateEntity("Target");
				REQUIRE(fixture.Start().has_value());
				REQUIRE(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
				fixture.GetScene().DestroyEntity(destroyCaller ? caller : receiver);
				static_cast<void>(fixture.GetEngine()->PrepareDestroyFlush());
				fixture.GetScene().FlushPendingDestroys();
				fixture.GetEngine()->FinishDestroyFlush();
				fixture.Frame.Tick = 7;
				fixture.GetEngine()->ResumeTasks();
				CHECK(target.GetName() == (destroyCaller ? "Target" : "Ran"));
				CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
				CHECK(fixture.Errors.empty());
			}
		}
	}

}
