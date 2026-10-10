#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/TaskScheduler.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

namespace Engine {

	namespace Test {

		class ScriptCaseObserver final : public IScriptTestHost
		{
		public:
			void Report(const ScriptTestReport& report) override { Reports.push_back(report); }
			void OnScriptError(const ScriptError& error, bool fatal) override
			{
				Errors.push_back(error);
				Fatal = Fatal || fatal;
			}
			void OnExpectedScriptError(const ScriptError& error) override { Claimed.push_back(error); }
			Status InjectInput(const Json& event) override
			{
				Inputs.push_back(event);
				return {};
			}
			Status CaptureScreenshot(std::string_view name) override
			{
				Screenshots.emplace_back(name);
				return {};
			}
			Status BeginAudioCapture(uint32_t ticks) override
			{
				CaptureTicks = ticks;
				Capturing = true;
				return {};
			}
			bool IsAudioCaptureReady() const override { return Capturing && AudioReady; }
			Result<AudioLevels> EndAudioCapture() override
			{
				Capturing = false;
				return AudioLevels{ 0.25f, 0.5f, 0.75f };
			}
			void CancelAudioCapture() override { Capturing = false; }
			Status ReloadScript(AssetHandle /*script*/) override { return MakeError(ErrorCode::Unsupported, "No editor in this case fixture"); }
			uint64_t ComputeStateHash() const override { return 0; }
			ScriptExtractionState GetLastExtraction() const override { return {}; }
			std::optional<glm::vec3> GetExtractedPosition(UUID /*entity*/) const override { return std::nullopt; }
			void OnDebugBreak() override { ++Breaks; }

			std::vector<ScriptTestReport> Reports{};
			std::vector<ScriptError> Errors{};
			std::vector<ScriptError> Claimed{};
			std::vector<Json> Inputs{};
			std::vector<std::string> Screenshots{};
			uint32_t CaptureTicks = 0;
			uint32_t Breaks = 0;
			bool Capturing = false;
			bool AudioReady = false;
			bool Fatal = false;
		};

		static ScriptCollectedSuite CollectCaseFixture(ScriptTestFixture& fixture, ScriptCaseObserver& host, std::string source)
		{
			const AssetHandle handle(0x220000);
			const auto asset = fixture.AddScript(handle, "Assets/Tests/Cases.luau", std::move(source));
			if (!asset)
				INFO(asset.error().ToString());
			REQUIRE(asset);
			REQUIRE(fixture.Start());
			const auto suite = fixture.GetEngine()->CollectSuite(handle, host);
			if (!suite)
				INFO(suite.error().ToString());
			REQUIRE(suite);
			return *suite;
		}

		void RunDebugBreakCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptCaseObserver host;
			ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .TestHost = &host });
			REQUIRE(fixture.Start());
			const auto result = fixture.Evaluate("Debug.Break(); return true");
			REQUIRE(result);
			CHECK(result->Value.Get() == Json(true));
			CHECK(host.Breaks == 1);
			CHECK_FALSE(fixture.PauseRequested);
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptCases: case waits use simulation ticks and case cancellation releases owned tasks")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Wait", function()
		Task.Delay(10, function() error("cancelled case task ran") end)
		Test.WaitTicks(2)
		Test.Expect(Time.GetTick() == 2)
	end)
	Test.Case("Next", function() Test.Expect(true) end)
end, {CaseTimeoutTicks = 30})
)");
			REQUIRE(suite.Cases.size() == 2);
			CHECK(suite.Cases[0].TimeoutTicks == 30);
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			CHECK_FALSE(fixture.GetEngine()->StartCase(suite.Cases[1].Function));
			const auto first = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(first);
			CHECK(first->State == ScriptCaseState::Yielded);
			fixture.Frame.Tick = 1;
			const auto waiting = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(waiting);
			CHECK(waiting->State == ScriptCaseState::Yielded);
			fixture.Frame.Tick = 2;
			const auto done = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(done);
			CHECK(done->State == ScriptCaseState::Completed);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			fixture.GetEngine()->CancelCase(*thread);
			fixture.GetEngine()->CancelCase(*thread);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.GetEngine()->StartCase(suite.Cases[1].Function).has_value());
			fixture.Frame.Tick = 1000;
			fixture.GetEngine()->ResumeTasks();
			CHECK(host.Errors.empty());
			CHECK(host.Reports.empty());
		}

		TEST_CASE("ScriptCases: terminal failures cannot be swallowed by pcall and waits retain their authored location")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(return Test.Suite("Cases", function()
	Test.Case("Fail", function() pcall(function() Test.Fail("terminal") end) end)
	Test.Case("Wait", function()
		Test.WaitUntil(function() return false end, 3)
	end)
end))");
			const auto failedThread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(failedThread);
			const auto failed = fixture.GetEngine()->ResumeCase(*failedThread);
			REQUIRE(failed);
			CHECK(failed->State == ScriptCaseState::Failed);
			CHECK(failed->Message == "terminal");
			CHECK_FALSE(failed->Error.has_value());
			CHECK(host.Errors.empty());
			REQUIRE(host.Reports.size() == 1);
			fixture.GetEngine()->CancelCase(*failedThread);
			const auto waitingThread = fixture.GetEngine()->StartCase(suite.Cases[1].Function);
			REQUIRE(waitingThread);
			const auto pending = fixture.GetEngine()->ResumeCase(*waitingThread);
			REQUIRE(pending);
			CHECK(pending->State == ScriptCaseState::Yielded);
			CHECK(pending->File == "Assets/Tests/Cases.luau");
			CHECK(pending->Line == 4);
			fixture.Frame.Tick = 3;
			const auto expired = fixture.GetEngine()->ResumeCase(*waitingThread);
			REQUIRE(expired);
			CHECK(expired->State == ScriptCaseState::Failed);
			CHECK(expired->Line == 4);
		}

		TEST_CASE("ScriptCases: caught Fail and Skip prevent later effects and cancel queued case tasks")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
local function after(signal)
	Task.Delay(0, function() Scene.CreateEntity("Queued") end)
	pcall(signal)
	pcall(function() Scene.CreateEntity("After") end)
	pcall(function() Task.Spawn(function() Scene.CreateEntity("Spawned") end) end)
end
return Test.Suite("Cases", function()
	Test.Case("Fail", function() after(function() Test.Fail("failed") end) end)
	Test.Case("Skip", function() after(function() Test.Skip("skipped") end) end)
	Test.Case("Task fail", function()
		Task.Spawn(function() after(function() Test.Fail("failed") end) end)
		pcall(function() Scene.CreateEntity("After") end)
	end)
	Test.Case("Task skip", function()
		Task.Spawn(function() after(function() Test.Skip("skipped") end) end)
		pcall(function() Scene.CreateEntity("After") end)
	end)
end)
)");
			REQUIRE(suite.Cases.size() == 4);
			for (size_t index = 0; index < suite.Cases.size(); ++index)
			{
				const auto thread = fixture.GetEngine()->StartCase(suite.Cases[index].Function);
				REQUIRE(thread);
				const auto result = fixture.GetEngine()->ResumeCase(*thread);
				REQUIRE(result);
				CHECK(result->State == (index % 2 == 0 ? ScriptCaseState::Failed : ScriptCaseState::Skipped));
				CHECK_FALSE(result->Error);
				CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
				++fixture.Frame.Tick;
				fixture.GetEngine()->ResumeTasks();
				CHECK(fixture.GetScene().GetEntityCount() == 0);
				fixture.GetEngine()->CancelCase(*thread);
			}
			CHECK(host.Errors.empty());
			CHECK(host.Reports.size() == 4);
		}

		TEST_CASE("ScriptCases: catching a terminal signal never runs the remaining case body")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
local continued = 0
return Test.Suite("Cases", function()
	Test.Case("Fail", function()
		pcall(function() Test.Fail("failed") end)
		continued += 1
	end)
	Test.Case("Skip", function()
		pcall(function() Test.Skip("skipped") end)
		continued += 1
	end)
	Test.Case("Task fail", function()
		pcall(function()
			Task.Spawn(function()
				pcall(function() Test.Fail("failed") end)
				continued += 1
			end)
		end)
		continued += 1
	end)
	Test.Case("Task skip", function()
		pcall(function()
			Task.Spawn(function()
				pcall(function() Test.Skip("skipped") end)
				continued += 1
			end)
		end)
		continued += 1
	end)
	Test.Case("Observe", function() Test.ExpectEqual(continued, 0) end)
end)
)");
			REQUIRE(suite.Cases.size() == 5);
			const ScriptCaseState expected[] = { ScriptCaseState::Failed, ScriptCaseState::Skipped, ScriptCaseState::Failed, ScriptCaseState::Skipped, ScriptCaseState::Completed };
			for (size_t index = 0; index < suite.Cases.size(); ++index)
			{
				const auto thread = fixture.GetEngine()->StartCase(suite.Cases[index].Function);
				REQUIRE(thread);
				const auto result = fixture.GetEngine()->ResumeCase(*thread);
				REQUIRE(result);
				CHECK(result->State == expected[index]);
				fixture.GetEngine()->CancelCase(*thread);
			}
			CHECK(host.Reports.size() == 4);
			CHECK(host.Errors.empty());
		}

		TEST_CASE("ScriptCases: Fail in a case-owned behaviour callback ends the case without a runtime error")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const AssetHandle behaviour(0x220001);
			REQUIRE(fixture.AddScript(behaviour, "Assets/Scripts/CaseFailure.luau", R"(
local T = {}
function T:OnFixedUpdate() Test.Fail("callback ended the case") end
return Script.Define("CaseFailure", T)
)"));
			const Entity entity = fixture.GetScene().CreateEntity("Driver");
			ScriptComponent component{};
			component.Script.SetHandle(behaviour);
			entity.AddComponent<ScriptComponent>(component);
			const auto suite = Test::CollectCaseFixture(fixture, host,
				"return Test.Suite('Cases', function() Test.Case('Callback', function() Test.WaitTicks(2) end) end)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			const auto pending = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(pending);
			REQUIRE(pending->State == ScriptCaseState::Yielded);
			fixture.GetEngine()->FixedUpdate();
			const auto result = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(result);
			CHECK(result->State == ScriptCaseState::Failed);
			CHECK_FALSE(result->Error);
			CHECK(host.Errors.empty());
			REQUIRE(host.Reports.size() == 1);
			CHECK(host.Reports[0].Message == "callback ended the case");
		}

		TEST_CASE("ScriptCases: expected errors claim individual occurrences without removing them from diagnostics")
		{
			Test::ExpectLog expected(LogLevel::Error, "expected fault");
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Errors", function()
		local function bad() error("expected fault") end
		Task.Spawn(bad)
		Task.Spawn(bad)
		Test.ExpectScriptError("expected.*fault", 0)
		Test.ExpectScriptError("expected.*fault", 0)
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			const auto result = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(result);
			CHECK(result->State == ScriptCaseState::Completed);
			REQUIRE(host.Errors.size() == 2);
			REQUIRE(host.Claimed.size() == 2);
			CHECK(host.Claimed[0].ID != host.Claimed[1].ID);
			const auto errors = fixture.GetEngine()->GetErrors().Read();
			REQUIRE(errors.size() == 1);
			CHECK(errors[0].Count == 2);
			CHECK(host.Reports.empty());
		}

		TEST_CASE("ScriptCases: audio readiness follows the host pull and cancellation releases an unfinished capture")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Audio", function()
		local levels = Test.CaptureAudio(2)
		Test.ExpectEqual(levels.Peak, 0.75)
		Test.ExpectEqual(levels.RmsLeft, 0.25)
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			REQUIRE(fixture.GetEngine()->ResumeCase(*thread));
			CHECK(host.Capturing);
			CHECK(host.CaptureTicks == 2);
			fixture.Frame.Tick = 5;
			const auto waiting = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(waiting);
			CHECK(waiting->State == ScriptCaseState::Yielded);
			host.AudioReady = true;
			const auto done = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(done);
			CHECK(done->State == ScriptCaseState::Completed);
			CHECK_FALSE(host.Capturing);
			CHECK(host.Reports.empty());
			fixture.GetEngine()->CancelCase(*thread);
			host.AudioReady = false;
			const auto next = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(next);
			REQUIRE(fixture.GetEngine()->ResumeCase(*next));
			CHECK(host.Capturing);
			fixture.GetEngine()->CancelCase(*next);
			CHECK_FALSE(host.Capturing);
		}

		TEST_CASE("ScriptCases: task predicates poll before their timeout and resume on the first ready tick")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Polling", function()
		Task.Spawn(function()
			Test.WaitUntil(function() return Time.GetTick() == 1 end, 10)
			Scene.CreateEntity("Ready")
		end)
		Test.WaitTicks(2)
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			REQUIRE(fixture.GetEngine()->ResumeCase(*thread));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			CHECK(fixture.GetScene().GetEntityCount() == 0);
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.GetScene().GetEntityCount() == 1);
			fixture.Frame.Tick = 2;
			const auto result = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(result);
			CHECK(result->State == ScriptCaseState::Completed);
			CHECK(host.Errors.empty());
			CHECK(host.Reports.empty());
		}

		TEST_CASE("ScriptCases: a predicate can cancel its own task without releasing its executing roots")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Cancellation", function()
		local task
		task = Task.Spawn(function()
			Test.WaitUntil(function()
				if Time.GetTick() == 0 then return false end
				pcall(function() Task.Cancel(task) end)
				pcall(function() Scene.CreateEntity("Forbidden") end)
				return true
			end, 10)
			Scene.CreateEntity("Continued")
		end)
		Test.WaitTicks(2)
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			REQUIRE(fixture.GetEngine()->ResumeCase(*thread));
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.GetScene().GetEntityCount() == 0);
			fixture.Frame.Tick = 2;
			const auto result = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(result);
			CHECK(result->State == ScriptCaseState::Completed);
			CHECK(host.Errors.empty());
			CHECK(host.Reports.empty());
		}

		TEST_CASE("ScriptCases: Fail and Skip inside polled predicates end the owning case without a runtime error")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
local function waitForSignal(signal)
	local start = Time.GetTick()
	Test.WaitUntil(function()
		if Time.GetTick() == start then return false end
		pcall(signal)
		pcall(function() Scene.CreateEntity("Forbidden") end)
		return true
	end, 10)
	Scene.CreateEntity("Continued")
end
return Test.Suite("Cases", function()
	Test.Case("Fail", function() waitForSignal(function() Test.Fail("failed") end) end)
	Test.Case("Skip", function() waitForSignal(function() Test.Skip("skipped") end) end)
	Test.Case("Task fail", function()
		Task.Spawn(function() waitForSignal(function() Test.Fail("failed") end) end)
		Test.WaitTicks(5)
	end)
	Test.Case("Task skip", function()
		Task.Spawn(function() waitForSignal(function() Test.Skip("skipped") end) end)
		Test.WaitTicks(5)
	end)
end)
)");
			REQUIRE(suite.Cases.size() == 4);
			for (size_t index = 0; index < suite.Cases.size(); ++index)
			{
				const auto thread = fixture.GetEngine()->StartCase(suite.Cases[index].Function);
				REQUIRE(thread);
				const auto pending = fixture.GetEngine()->ResumeCase(*thread);
				REQUIRE(pending);
				REQUIRE(pending->State == ScriptCaseState::Yielded);
				++fixture.Frame.Tick;
				fixture.GetEngine()->ResumeTasks();
				const auto result = fixture.GetEngine()->ResumeCase(*thread);
				REQUIRE(result);
				CHECK(result->State == (index % 2 == 0 ? ScriptCaseState::Failed : ScriptCaseState::Skipped));
				CHECK_FALSE(result->Error);
				CHECK(fixture.GetScene().GetEntityCount() == 0);
				CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
				fixture.GetEngine()->CancelCase(*thread);
			}
			CHECK(host.Errors.empty());
			CHECK(host.Reports.size() == 4);
		}

		TEST_CASE("ScriptCases: a task wait timeout fails its owning case and cancels its siblings")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Task timeout", function()
		Task.Spawn(function() Test.WaitUntil(function() return false end, 1) end)
		Task.Delay(2, function() Scene.CreateEntity("Forbidden") end)
		Test.WaitTicks(5)
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			REQUIRE(fixture.GetEngine()->ResumeCase(*thread));
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 2);
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			const auto result = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(result);
			CHECK(result->State == ScriptCaseState::Failed);
			CHECK(result->Message == "Test.WaitUntil exceeded its tick limit");
			CHECK_FALSE(result->Error);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.GetScene().GetEntityCount() == 0);
			CHECK(host.Errors.empty());
			CHECK(host.Reports.size() == 1);
		}

		TEST_CASE("ScriptCases: malformed immediate wait results release their captured payloads")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixtureSpecification specification{};
			specification.Settings.MemoryLimitMB = 32;
			specification.TestMode = true;
			specification.TestHost = &host;
			Test::ScriptTestFixture fixture(specification);
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Invalid predicates", function()
		for i = 1, 80 do
			local payload = table.create(65536, i)
			local ok, message = pcall(function()
				Test.WaitUntil(function() return payload end, 2)
			end)
			assert(not ok and string.find(message, "boolean"))
		end
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			const auto result = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(result);
			CHECK(result->State == ScriptCaseState::Completed);
			CHECK(fixture.GetEngine()->GetMemoryState().SoftBreachCount == 0);
			CHECK(host.Errors.empty());
			CHECK(host.Reports.empty());
		}

		TEST_CASE("ScriptCases: a waiting predicate fault releases its coroutine before memory recovery")
		{
			Test::ExpectLog expected(LogLevel::Error, "script exceeded memory limit");
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixtureSpecification specification{};
			specification.Settings.MemoryLimitMB = 16;
			specification.TestMode = true;
			specification.TestHost = &host;
			Test::ScriptTestFixture fixture(specification);
			const auto suite = Test::CollectCaseFixture(fixture, host, R"(
return Test.Suite("Cases", function()
	Test.Case("Faulting wait", function()
		local payload = {}
		Test.WaitUntil(function()
			if Time.GetTick() == 0 then return false end
			for i = 1, 100 do payload[i] = table.create(65536, i) end
			return false
		end, 10)
	end)
end)
)");
			const auto thread = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(thread);
			const auto first = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(first);
			REQUIRE(first->State == ScriptCaseState::Yielded);
			fixture.Frame.Tick = 1;
			const auto failed = fixture.GetEngine()->ResumeCase(*thread);
			REQUIRE(failed);
			CHECK(failed->State == ScriptCaseState::Failed);
			REQUIRE(failed->Error);
			CHECK(failed->Error->Kind == ScriptErrorKind::Memory);
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
			CHECK_FALSE(fixture.GetEngine()->GetMemoryState().NeedsRecovery);
			CHECK(fixture.GetEngine()->GetMemoryState().SoftBreachCount == 1);
			CHECK_FALSE(host.Fatal);
			REQUIRE(host.Errors.size() == 1);
			const auto healthy = fixture.Evaluate("return 17");
			REQUIRE(healthy);
			CHECK(healthy->Value.Get() == Json(17));
		}

		TEST_CASE("ScriptEngine: retained references never survive their VM or alias released work")
		{
			Test::ScriptCaseObserver host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const std::string source = "return Test.Suite('Cases', function() Test.Case('Empty', function() end) end)";
			const auto suite = Test::CollectCaseFixture(fixture, host, source);
			const auto first = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(first);
			fixture.GetEngine()->CancelCase(*first);
			const auto second = fixture.GetEngine()->StartCase(suite.Cases[0].Function);
			REQUIRE(second);
			CHECK(first->Index != second->Index);
			CHECK_FALSE(fixture.GetEngine()->ResumeCase(*first));
			CHECK_FALSE(fixture.GetEngine()->GetTaskScheduler()->Cancel(suite.Cases[0].Function));
			fixture.GetEngine()->CancelCase(*second);
			fixture.GetEngine()->ReleaseReference(suite.Cases[0].Function);
			CHECK_FALSE(fixture.GetEngine()->StartCase(suite.Cases[0].Function));
			fixture.Stop();
			REQUIRE(fixture.Start());
			CHECK(fixture.GetEngine()->GetGeneration() != first->VMGeneration);
			CHECK_FALSE(fixture.GetEngine()->GetTaskScheduler()->Cancel(*first));
			CHECK_FALSE(fixture.GetEngine()->GetTaskScheduler()->Spawn(suite.Cases[0].Function));
		}
	}

}
