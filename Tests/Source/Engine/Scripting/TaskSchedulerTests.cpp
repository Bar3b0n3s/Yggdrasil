#include "TestsPCH.h"
#include "Engine/Scripting/TaskScheduler.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptTestHost.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

namespace Engine {

	namespace Test {

		static UUID MakeTaskOwner(ScriptTestFixture& fixture, std::string source)
		{
			const AssetHandle handle(0x210000);
			const auto compiled = fixture.AddScript(handle, "Assets/Scripts/Tasks.luau", std::move(source));
			if (!compiled)
				INFO(compiled.error().ToString());
			REQUIRE(compiled);
			const Entity entity = fixture.GetScene().CreateEntity("TaskOwner");
			ScriptComponent component{};
			component.Script.SetHandle(handle);
			entity.AddComponent<ScriptComponent>(component);
			return entity.GetUUID();
		}

		static Json TaskValue(ScriptTestFixture& fixture, UUID owner, std::string_view code)
		{
			const auto value = fixture.Evaluate(code, owner);
			if (!value)
				INFO(value.error().ToString());
			REQUIRE(value);
			return value->Value.Get();
		}

		static int ReleaseScheduledThread(ScriptCall& call)
		{
			// Exercise the public release API while the scheduler still owns the opaque task handle.
			Lua::GetEngine(call)->ReleaseReference(Lua::CheckTaskHandle(call, 1));
			return 0;
		}

		static int EvaluateNestedTasks(ScriptCall& call)
		{
			const auto entity = Lua::Check<ScriptEntityIdentity>(call, 1);
			const auto path = VfsPath::Parse("project://Assets/Tests/Nested.luau");
			if (!path)
				return Lua::RaiseError(call, path.error());
			const auto result = Lua::GetEngine(call)->Evaluate(R"(
Task.Spawn(function()
	self.Value = "inner"
	Task.WaitTicks(1)
	self.Value = "resumed"
end)
return "inner result"
)",
				*path, entity.ID);
			if (!result)
				return Lua::RaiseError(call, result.error());
			Lua::PushString(call, "returned");
			return 1;
		}

		static int EvaluateOwnerlessTask(ScriptCall& call)
		{
			const auto path = VfsPath::Parse("project://Assets/Tests/Ownerless.luau");
			if (!path)
				return Lua::RaiseError(call, path.error());
			const auto result = Lua::GetEngine(call)->Evaluate("Task.Delay(0, function() Scene.CreateEntity('Ownerless') end); return nil", *path);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}

		// Records test outcomes for the case-boundary task tests; screenshots, input and audio are unused.
		class TaskCaseHost final : public IScriptTestHost
		{
		public:
			void Report(const ScriptTestReport& report) override { Reports.push_back(report); }
			void OnScriptError(const ScriptError& error, bool /*fatal*/) override { Errors.push_back(error); }
			void OnExpectedScriptError(const ScriptError& /*error*/) override {}
			Status InjectInput(const Json& /*event*/) override { return MakeError(ErrorCode::Unsupported, "input is unused by task tests"); }
			Status CaptureScreenshot(std::string_view /*name*/) override { return MakeError(ErrorCode::Unsupported, "screenshots are unused by task tests"); }
			Status BeginAudioCapture(uint32_t /*ticks*/) override { return MakeError(ErrorCode::Unsupported, "audio is unused by task tests"); }
			bool IsAudioCaptureReady() const override { return false; }
			Result<AudioLevels> EndAudioCapture() override { return MakeError(ErrorCode::Unsupported, "audio is unused by task tests"); }
			void CancelAudioCapture() override {}
			Status ReloadScript(AssetHandle /*script*/) override { return MakeError(ErrorCode::Unsupported, "reload is unused by task tests"); }
			uint64_t ComputeStateHash() const override { return 0; }
			ScriptExtractionState GetLastExtraction() const override { return {}; }
			std::optional<glm::vec3> GetExtractedPosition(UUID /*entity*/) const override { return std::nullopt; }
			void OnDebugBreak() override {}

			std::vector<ScriptTestReport> Reports{};
			std::vector<ScriptError> Errors{};
		};

		static ScriptCollectedSuite CollectTaskCases(ScriptTestFixture& fixture, TaskCaseHost& host, std::string source)
		{
			const AssetHandle handle(0x2100ff);
			const auto asset = fixture.AddScript(handle, "Assets/Tests/TaskCases.luau", std::move(source));
			REQUIRE_MESSAGE(asset, (asset ? "" : asset.error().ToString()));
			REQUIRE(fixture.Start());
			const auto suite = fixture.GetEngine()->CollectSuite(handle, host);
			REQUIRE_MESSAGE(suite, (suite ? "" : suite.error().ToString()));
			return *suite;
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Tasks: Task.Wait in OnStart raises the documented error")
		{
			Test::ExpectLog expected(LogLevel::Error, "Task.Wait can only be used inside Task.Spawn, Task.Delay or a Test.Case body");
			Test::ScriptTestFixture fixture;
			static_cast<void>(Test::MakeTaskOwner(fixture, R"(local T = {}
function T:OnStart() Task.Wait(1) end
return Script.Define("Tasks", T))"));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors.front().Message.find("Task.Wait can only be used inside Task.Spawn, Task.Delay or a Test.Case body") != std::string::npos);
			CHECK(fixture.Errors.front().Line == 2);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
		}

		TEST_CASE("Tasks: Task.Wait inside Task.Spawn resumes at the expected tick")
		{
			Test::ScriptTestFixture fixture;
			fixture.Frame.FixedDeltaTime = 0.25;
			const UUID owner = Test::MakeTaskOwner(fixture, R"(local T = {}
function T:OnStart()
	self.Events = {}
	Task.Spawn(function()
		table.insert(self.Events, Time.GetTick())
		Task.Wait(0.5)
		table.insert(self.Events, Time.GetTick())
		Task.WaitTicks(0)
		table.insert(self.Events, Time.GetTick())
	end)
end
return Script.Define("Tasks", T))");
			REQUIRE(fixture.Start());
			CHECK(Test::TaskValue(fixture, owner, "return self.Events") == Json::array({ 0 }));
			fixture.GetEngine()->ResumeTasks();
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(Test::TaskValue(fixture, owner, "return self.Events") == Json::array({ 0 }));
			fixture.Frame.Tick = 2;
			fixture.GetEngine()->ResumeTasks();
			fixture.GetEngine()->ResumeTasks();
			CHECK(Test::TaskValue(fixture, owner, "return self.Events") == Json::array({ 0, 2 }));
			fixture.Frame.Tick = 3;
			fixture.GetEngine()->ResumeTasks();
			CHECK(Test::TaskValue(fixture, owner, "return self.Events") == Json::array({ 0, 2, 3 }));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("Tasks: yielding inside a metamethod raises the VM error, caught as a script error")
		{
			Test::ExpectLog expected(LogLevel::Error, "Task.Wait");
			Test::ScriptTestFixture fixture;
			static_cast<void>(Test::MakeTaskOwner(fixture, R"(local T = {}
function T:OnStart()
	Task.Spawn(function()
		local value = setmetatable({}, {__index = function() Task.WaitTicks(1); return 0 end})
		self.Value = value.Missing
	end)
end
return Script.Define("Tasks", T))"));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.size() == 1);
			const auto& error = fixture.Errors.front();
			CHECK(error.Kind == ScriptErrorKind::Runtime);
			CHECK(error.Script == "Assets/Scripts/Tasks.luau");
			CHECK(error.Line == 4);
			CHECK(error.Message.find("Task.Wait") != std::string::npos);
			CHECK_FALSE(error.Traceback.empty());
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
		}

		TEST_CASE("TaskScheduler: destruction and case cancellation release every owned task")
		{
			Test::ScriptTestFixture fixture;
			const UUID owner = Test::MakeTaskOwner(fixture, R"(local T = {}
function T:OnStart()
	Task.Spawn(function() Task.WaitTicks(10); error("destroyed task resumed") end)
	Task.Delay(10, function() error("destroyed delay resumed") end)
end
return Script.Define("Tasks", T))");
			REQUIRE(fixture.Start());
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 2);
			fixture.GetScene().DestroyEntity(fixture.GetScene().FindEntityByID(owner));
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 1);
			fixture.GetEngine()->FinishDestroyFlush();
			fixture.GetScene().FlushPendingDestroys();
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			fixture.Frame.Tick = 1000;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.Errors.empty());
			// Case-owned work has its own regression in ScriptCaseTests, using actual collected case functions.
		}

		TEST_CASE("TaskScheduler: equal due times keep creation order and a new wait never resumes twice in one tick")
		{
			Test::ScriptTestFixture fixture;
			const UUID owner = Test::MakeTaskOwner(fixture, R"(local T = {}
function T:OnStart()
	self.Events = {}
	Task.Delay(0, function()
		table.insert(self.Events, "first")
		Task.Spawn(function()
			table.insert(self.Events, "nested")
			Task.WaitTicks(0)
			table.insert(self.Events, "next")
		end)
	end)
	Task.Delay(0, function() table.insert(self.Events, "second") end)
end
return Script.Define("Tasks", T))");
			REQUIRE(fixture.Start());
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(Test::TaskValue(fixture, owner, "return self.Events") == Json::array({ "first", "nested", "second" }));
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			fixture.Frame.Tick = 2;
			fixture.GetEngine()->ResumeTasks();
			CHECK(Test::TaskValue(fixture, owner, "return self.Events") == Json::array({ "first", "nested", "second", "next" }));
		}

		TEST_CASE("TaskScheduler: releasing a public task reference also removes its scheduled work")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Task lifetime regression.").Function("Release", &Test::ReleaseScheduledThread, "(task: TaskHandle) -> ()", "Release the public task reference before the scheduler resumes it.");
				return {};
			};
			Test::ScriptTestFixture fixture(specification);
			static_cast<void>(Test::MakeTaskOwner(fixture, R"(
local T = {}
function T:OnStart()
	local task = Task.Delay(0, function() error("released thread resumed") end)
	Probe.Release(task)
end
return Script.Define("Tasks", T)
)"));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.empty());
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.Errors.empty());
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
		}

		TEST_CASE("TaskScheduler: a cancelled running task cannot make another native effect through pcall")
		{
			Test::ScriptTestFixture fixture;
			static_cast<void>(Test::MakeTaskOwner(fixture, R"(
local T = {}
function T:OnStart()
	self.Task = Task.Delay(0, function()
		Task.Cancel(self.Task)
		pcall(function() Scene.CreateEntity("After cancellation") end)
	end)
end
return Script.Define("Tasks", T)
)"));
			REQUIRE(fixture.Start());
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.GetScene().GetEntityCount() == 1);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("TaskScheduler: nested evaluation restores its caller and assigns tasks to the evaluation entity")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Nested evaluation regression.").Function("Evaluate", &Test::EvaluateNestedTasks, "(entity: Entity) -> string", "Run a nested evaluation which creates a task for another entity.");
				return {};
			};
			Test::ScriptTestFixture fixture(specification);
			const AssetHandle innerScript(0x210001);
			REQUIRE(fixture.AddScript(innerScript, "Assets/Scripts/Inner.luau", "return Script.Define('Inner', {})"));
			const Entity inner = fixture.GetScene().CreateEntity("Inner");
			ScriptComponent component{};
			component.Script.SetHandle(innerScript);
			inner.AddComponent<ScriptComponent>(component);
			const UUID outer = Test::MakeTaskOwner(fixture, R"(
local T = {}
function T:OnStart()
	Task.Spawn(function()
		self.Value = "before"
		assert(Probe.Evaluate(Scene.FindByName("Inner")) == "returned")
		self.Value = "after"
		Task.WaitTicks(1)
		self.Value = "outer resumed"
	end)
end
return Script.Define("Tasks", T)
)");
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.empty());
			CHECK(Test::TaskValue(fixture, outer, "return self.Value") == Json("after"));
			CHECK(Test::TaskValue(fixture, inner.GetUUID(), "return self.Value") == Json("inner"));
			fixture.GetScene().DestroyEntity(fixture.GetScene().FindEntityByID(outer));
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 1);
			fixture.GetEngine()->FinishDestroyFlush();
			fixture.GetScene().FlushPendingDestroys();
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(Test::TaskValue(fixture, inner.GetUUID(), "return self.Value") == Json("resumed"));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("TaskScheduler: ownerless nested evaluation never inherits its caller's entity")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Ownerless evaluation regression.").Function("Evaluate", &Test::EvaluateOwnerlessTask, "() -> ()", "Queue a task from an explicitly ownerless nested evaluation.");
				return {};
			};
			Test::ScriptTestFixture fixture(specification);
			const UUID owner = Test::MakeTaskOwner(fixture, R"(
local T = {}
function T:OnStart()
	Task.Spawn(function()
		Probe.Evaluate()
		Task.WaitTicks(1)
		error("destroyed owner resumed")
	end)
end
return Script.Define("Tasks", T)
)");
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.empty());
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 2);
			fixture.GetScene().DestroyEntity(fixture.GetScene().FindEntityByID(owner));
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 1);
			fixture.GetEngine()->FinishDestroyFlush();
			fixture.GetScene().FlushPendingDestroys();
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.GetScene().GetEntityCount() == 1);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("TaskScheduler: invalid delays have no effect and completed handles cancel idempotently")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			const auto evaluated = fixture.Evaluate(R"(
for _, seconds in {-1, math.huge, 0/0, 1e300} do
	assert(not pcall(function() Task.Delay(seconds, function() error("invalid delay ran") end) end))
end
local task = Task.Spawn(function() end)
Task.Cancel(task)
Task.Cancel(task)
return true
)");
			REQUIRE(evaluated);
			CHECK(evaluated->Value.Get() == Json(true));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
		}

		TEST_CASE("TaskScheduler: behaviour tasks survive the end of the test case they were spawned in")
		{
			Test::TaskCaseHost host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			REQUIRE(fixture.AddScript(AssetHandle(0x210001), "Assets/Scripts/Game.luau", R"(
local T = {}
function T:OnFixedUpdate()
	self.Calls = (self.Calls or 0) + 1
	if self.Calls == 1 then
		Task.Spawn(function()
			Task.WaitTicks(3)
			self.Spawned = true
		end)
	end
end
return Script.Define("Game", T)
)"));
			const Entity game = fixture.GetScene().CreateEntity("Game");
			ScriptComponent component{};
			component.Script.SetHandle(AssetHandle(0x210001));
			game.AddComponent<ScriptComponent>(component);
			// Suite isolation: both cases share this scene, so gameplay continuations must cross the case boundary.
			const auto suite = Test::CollectTaskCases(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("First", function() Test.WaitTicks(1) end)
	Test.Case("Second", function()
		Test.WaitUntil(function() return (Scene.FindByName("Game") :: Entity):GetScript().Spawned == true end, 10)
	end)
end)
)");
			REQUIRE(suite.Cases.size() == 2);
			const auto first = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(first);
			REQUIRE(fixture.GetEngine()->ResumeCase(*first));
			fixture.GetEngine()->FixedUpdate(); // the behaviour spawns its task while the first case is running
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			const auto passed = fixture.GetEngine()->ResumeCase(*first);
			REQUIRE(passed);
			CHECK(passed->State == ScriptCaseState::Completed);
			fixture.GetEngine()->CancelCase(*first); // the runner ends every case like this, passed or failed
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			const auto second = fixture.GetEngine()->StartCase(suite.Cases[1].Function);
			REQUIRE(second);
			ScriptCaseState state = ScriptCaseState::Yielded;
			for (uint64_t tick = 1; tick <= 4 && state == ScriptCaseState::Yielded; ++tick)
			{
				fixture.Frame.Tick = tick;
				fixture.GetEngine()->ResumeTasks();
				const auto resumed = fixture.GetEngine()->ResumeCase(*second);
				REQUIRE(resumed);
				state = resumed->State;
			}
			CHECK(state == ScriptCaseState::Completed);
			CHECK(Test::TaskValue(fixture, game.GetUUID(), "return self.Spawned") == Json(true));
			fixture.GetEngine()->CancelCase(*second);
			CHECK(host.Reports.empty());
			CHECK(host.Errors.empty());
		}

		TEST_CASE("TaskScheduler: a case ended by its own task never suppresses another entity's gameplay")
		{
			Test::TaskCaseHost host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			REQUIRE(fixture.AddScript(AssetHandle(0x210002), "Assets/Scripts/Shooter.luau", R"(
local T = {}
function T:OnFixedUpdate()
	self.Calls = (self.Calls or 0) + 1
	if self.Calls == 1 then
		Task.Spawn(function()
			Task.WaitTicks(1)
			Scene.CreateEntity("Bullet")
		end)
	elseif self.Calls == 2 then
		Scene.CreateEntity("AfterLatch")
	end
end
return Script.Define("Shooter", T)
)"));
			const Entity shooter = fixture.GetScene().CreateEntity("Shooter");
			ScriptComponent component{};
			component.Script.SetHandle(AssetHandle(0x210002));
			shooter.AddComponent<ScriptComponent>(component);
			const auto suite = Test::CollectTaskCases(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Helper", function()
		Task.Spawn(function()
			Test.WaitTicks(1)
			Test.Fail("helper failed")
		end)
		Test.WaitTicks(5)
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			REQUIRE(fixture.GetEngine()->ResumeCase(*thread));
			fixture.GetEngine()->FixedUpdate(); // gameplay task created after the case's helper: resumed after it
			fixture.Frame.Tick = 1;
			// The helper latches the case terminal; the gameplay task resumed next in the same loop still takes effect.
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.GetScene().FindEntityByPath("/Bullet"));
			// So does a gameplay callback before the runner ends the latched case.
			fixture.GetEngine()->FixedUpdate();
			CHECK(fixture.GetScene().FindEntityByPath("/AfterLatch"));
			const auto failed = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(failed);
			CHECK(failed->State == ScriptCaseState::Failed);
			CHECK(failed->Message == "helper failed");
			fixture.GetEngine()->CancelCase(*thread);
			REQUIRE(host.Reports.size() == 1);
			CHECK(host.Reports[0].Message == "helper failed");
			CHECK(host.Errors.empty());
			CHECK(fixture.Errors.empty());
		}
	}

}
