#include "TestsPCH.h"
#include "Engine/Scripting/ScriptTestHost.h"

#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/TaskScheduler.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		class TestServices final : public IScriptTestHost
		{
		public:
			std::vector<ScriptTestReport> Reports{};
			std::vector<ScriptError> Errors{}, Claims{};
			std::vector<Json> Inputs{};
			std::vector<std::string> Screenshots{};
			std::vector<AssetHandle> Reloads{};
			uint32_t AudioBegins = 0, AudioEnds = 0, AudioCancels = 0, AudioTicks = 0, Breaks = 0;
			bool AudioReady = false, Capturing = false;
			UUID PositionedEntity{};

			void Report(const ScriptTestReport& report) override { Reports.push_back(report); }
			void OnScriptError(const ScriptError& error, bool) override { Errors.push_back(error); }
			void OnExpectedScriptError(const ScriptError& error) override { Claims.push_back(error); }
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
				++AudioBegins;
				AudioTicks = ticks;
				Capturing = true;
				return {};
			}
			bool IsAudioCaptureReady() const override { return Capturing && AudioReady; }
			Result<AudioLevels> EndAudioCapture() override
			{
				++AudioEnds;
				Capturing = false;
				return AudioLevels{ 0.25f, 0.5f, 0.75f };
			}
			void CancelAudioCapture() override
			{
				++AudioCancels;
				Capturing = false;
			}
			Status ReloadScript(AssetHandle script) override
			{
				Reloads.push_back(script);
				return {};
			}
			uint64_t ComputeStateHash() const override { return 0x1234; }
			ScriptExtractionState GetLastExtraction() const override { return { 0.25f, 19 }; }
			std::optional<glm::vec3> GetExtractedPosition(UUID entity) const override
			{
				return entity == PositionedEntity ? std::optional(glm::vec3(1, 2, 3)) : std::nullopt;
			}
			void OnDebugBreak() override { ++Breaks; }
		};

		ScriptCollectedSuite Collect(Test::ScriptTestFixture& fixture, TestServices& services, std::string source)
		{
			auto asset = fixture.AddScript(UUID(731), "Assets/Tests/Bindings.luau", std::move(source));
			REQUIRE(asset.has_value());
			REQUIRE(fixture.Start().has_value());
			auto suite = fixture.GetEngine()->CollectSuite(UUID(731), services);
			if (!suite)
				INFO(suite.error().ToString());
			REQUIRE(suite.has_value());
			return *suite;
		}

		ScriptReference StartCase(Test::ScriptTestFixture& fixture, const ScriptTestCase& test)
		{
			auto thread = fixture.GetEngine()->StartCase(test.Function);
			REQUIRE(thread.has_value());
			return *thread;
		}

		ScriptCaseResume Resume(Test::ScriptTestFixture& fixture, ScriptReference thread, uint64_t tick)
		{
			fixture.Frame.Tick = tick;
			auto result = fixture.GetEngine()->ResumeCase(thread);
			if (!result)
				INFO(result.error().ToString());
			REQUIRE(result.has_value());
			return *result;
		}

		void CheckTrue(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			auto result = fixture.Evaluate(source);
			if (!result)
				INFO(result.error().ToString());
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
		}

		void CaptureCoverage(Test::ScriptTestFixture& fixture, RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			fixture.Stop();
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot.has_value());
			snapshots.push_back(*snapshot);
		}

		void RunSuiteScenario(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .TestHost = &services });
			auto suite = Collect(fixture, services, R"(return Test.Suite("Options", function()
	Test.Case("inherited", function() Test.Expect(false, "executed") end)
	Test.Case("override", function() end, {TimeoutTicks=9})
	Test.Case("empty", function() end, {})
	for _, options in {{TimeoutTicks=0}, {TimeoutTicks=-1}, {TimeoutTicks=1.5}, {Unknown=2}} do
		assert(not pcall(Test.Case, "invalid", function() end, options))
	end
	assert(not pcall(Test.Case, "inherited", function() end))
	assert(not pcall(Test.Case, "", function() end))
	assert(not pcall(Test.Case, "not function", {}))
end, {CaseTimeoutTicks=6}))");
			REQUIRE(suite.Cases.size() == 3);
			CHECK(suite.Name == "Options");
			CHECK(suite.Cases[0].Name == "inherited");
			CHECK(suite.Cases[0].TimeoutTicks == 6);
			CHECK(suite.Cases[1].TimeoutTicks == 9);
			CHECK(suite.Cases[2].TimeoutTicks == 6);
			CHECK(suite.Cases[0].File == "Assets/Tests/Bindings.luau");
			CHECK(suite.Cases[0].Line == 2);
			CHECK(services.Reports.empty());
			const auto thread = StartCase(fixture, suite.Cases[0]);
			CHECK(services.Reports.empty());
			CHECK(Resume(fixture, thread, 0).State == ScriptCaseState::Completed);
			REQUIRE(services.Reports.size() == 1);
			CHECK(services.Reports[0].Message == "executed");
			CHECK(services.Reports[0].Line == 2);
			fixture.GetEngine()->CancelCase(thread);
			fixture.GetEngine()->EndSuite();
			CheckTrue(fixture, "return not pcall(Test.Case, 'outside', function() end)");
			CaptureCoverage(fixture, mode, snapshots);
		}

		void RunAssertionsScenario(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .TestHost = &services });
			auto suite = Collect(fixture, services, R"(return Test.Suite("Assertions", function()
	Test.Case("expectations", function()
		Test.Expect(false, "condition")
		Test.ExpectEqual(1, 2, "equality")
		Test.ExpectNear(1, 1.5, 0.1, "distance")
		Test.ExpectNear(vector.create(1,2,3), vector.create(1,2,3.001), 0.01)
		Test.ExpectNear(1, 1.000001)
		assert(not pcall(Test.Expect, 1))
		assert(not pcall(Test.ExpectEqual, 1))
		for _, value in {-1, math.huge, 0/0} do assert(not pcall(Test.ExpectNear, 1, 1, value)) end
	end)
	Test.Case("fail", function() pcall(Test.Fail, "terminal failure") end)
	Test.Case("skip", function() pcall(Test.Skip, "terminal skip") end)
end))");
			const std::array expected{ ScriptCaseState::Completed, ScriptCaseState::Failed, ScriptCaseState::Skipped };
			for (size_t i = 0; i < suite.Cases.size(); ++i)
			{
				const auto thread = StartCase(fixture, suite.Cases[i]);
				auto result = Resume(fixture, thread, 0);
				CHECK(result.State == expected[i]);
				CHECK_FALSE(result.Error.has_value());
				fixture.GetEngine()->CancelCase(thread);
			}
			REQUIRE(services.Reports.size() == 5);
			CHECK(services.Reports[0].Message == "condition");
			CHECK(services.Reports[1].Message == "equality");
			CHECK(services.Reports[2].Message == "distance");
			CHECK(services.Reports[3].Signal == ScriptTestSignal::Fail);
			CHECK(services.Reports[4].Signal == ScriptTestSignal::Skip);
			CHECK(services.Errors.empty());
			CaptureCoverage(fixture, mode, snapshots);
		}

		void RunWaitsScenario(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .TestHost = &services });
			auto suite = Collect(fixture, services, R"(return Test.Suite("Waits", function()
	Test.Case("wait", function()
		Test.WaitTicks(0)
		assert(Time.GetTick()==1)
		Test.WaitUntil(function() return Time.GetTick()>=3 end, 4)
		local levels=Test.CaptureAudio(2)
		assert(Time.GetTick()==6)
		Test.ExpectNear(levels.RmsLeft, 0.25)
		Test.ExpectNear(levels.RmsRight, 0.5)
		Test.ExpectNear(levels.Peak, 0.75)
	end)
end))");
			const auto thread = StartCase(fixture, suite.Cases[0]);
			for (uint64_t tick = 0; tick < 6; ++tick)
			{
				auto result = Resume(fixture, thread, tick);
				CHECK(result.State == ScriptCaseState::Yielded);
				CHECK(result.File == "Assets/Tests/Bindings.luau");
				CHECK(result.Line > 0);
			}
			CHECK(services.AudioBegins == 1);
			CHECK(services.AudioTicks == 2);
			CHECK(services.AudioEnds == 0);
			services.AudioReady = true;
			CHECK(Resume(fixture, thread, 6).State == ScriptCaseState::Completed);
			CHECK(services.AudioEnds == 1);
			CHECK(services.Reports.empty());
			CaptureCoverage(fixture, mode, snapshots);
		}

		void RunInputScenario(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .TestHost = &services });
			const auto entity = fixture.GetScene().CreateEntity("Visible");
			services.PositionedEntity = entity.GetUUID();
			static_cast<void>(fixture.GetScene().CreateEntity("Hidden"));
			REQUIRE(fixture.AddScript(UUID(732), "Assets/Scripts/Reload.luau", "return {}"));
			REQUIRE(fixture.Start());
			ScriptError error;
			error.Script = "Assets/Replays/Embedded.replay";
			error.JsonPointer = "/Expect/1/Luau";
			error.Message = "retained diagnostic";
			error.Line = 7;
			error.Traceback.push_back({ error.Script, 7, "predicate" });
			static_cast<void>(fixture.GetEngine()->GetErrors().Add(error));
			CheckTrue(fixture, R"(
Test.InjectAction("Jump", "Tap")
Test.InjectAction("Move", nil, 0.25)
Test.InjectKey("Space", "Down")
Test.InjectMouse("Left", "Up", vector.create(4,5,0))
Test.InjectGamepad(3, "South", "Tap")
Test.InjectGamepad(2, "LeftX", -0.5)
Test.Screenshot("frame")
assert(Test.GetStateHash()=="0000000000001234")
local extraction=Test.GetLastExtraction()
assert(extraction.Alpha==0.25 and extraction.Frame==19)
assert(Test.GetExtractedPosition(Scene.FindByName("Visible"))==vector.create(1,2,3))
assert(Test.GetExtractedPosition(Scene.FindByName("Hidden"))==nil)
local errors=Test.GetScriptErrors()
assert(#errors==1 and errors[1].line==7 and errors[1].jsonPointer=="/Expect/1/Luau")
assert(errors[1].traceback[1]["function"]=="predicate")
assert(#Test.GetScriptErrors(errors[1].id)==0)
for _, value in {-1, 0.5, 9007199254740992, math.huge} do assert(not pcall(Test.GetScriptErrors,value)) end
return true
)");
			REQUIRE(services.Inputs.size() == 6);
			CHECK(services.Inputs[0] == Json{ { "type", "action" }, { "name", "Jump" }, { "state", "Tap" } });
			CHECK(services.Inputs[1] == Json{ { "type", "action" }, { "name", "Move" }, { "value", 0.25f } });
			CHECK(services.Inputs[2] == Json{ { "type", "key" }, { "key", "Space" }, { "state", "Down" } });
			CHECK(services.Inputs[3] == Json{ { "type", "mouseButton" }, { "button", "Left" }, { "state", "Up" }, { "position", { 4, 5 } } });
			CHECK(services.Inputs[4] == Json{ { "type", "gamepadButton" }, { "gamepad", 3 }, { "button", "South" }, { "state", "Tap" } });
			CHECK(services.Inputs[5] == Json{ { "type", "gamepadAxis" }, { "gamepad", 2 }, { "axis", "LeftX" }, { "value", -0.5f } });
			CHECK(services.Screenshots == std::vector<std::string>{ "frame" });
			if (mode == RunModes::Editor)
			{
				CheckTrue(fixture, "Test.ReloadScript(Assets.Load(\"Assets/Scripts/Reload.luau\")); return true");
				CHECK(services.Reloads == std::vector<AssetHandle>{ UUID(732) });
			}
			else
				CHECK(services.Reloads.empty());
			CaptureCoverage(fixture, mode, snapshots);
		}

		void RunClaimsScenario(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .TestHost = &services });
			auto suite = Collect(fixture, services, R"(return Test.Suite("Claims",function()
	Test.Case("claim",function()
		local fault=function() error("occurrence 42") end
		Task.Spawn(fault); Task.Spawn(fault)
		assert(not pcall(Test.ExpectScriptError,"%",1))
		Test.ExpectScriptError("occurrence %d+",0)
		Test.ExpectScriptError("occurrence %d+",0)
		local errors=Test.GetScriptErrors()
		assert(#errors==1 and errors[1].count==2)
		Test.ExpectScriptError("occurrence %d+",1)
	end)
end))");
			Test::ExpectLog expected(LogLevel::Error, "occurrence 42");
			const auto thread = StartCase(fixture, suite.Cases[0]);
			CHECK(Resume(fixture, thread, 0).State == ScriptCaseState::Yielded);
			REQUIRE(services.Errors.size() == 2);
			REQUIRE(services.Claims.size() == 2);
			REQUIRE(fixture.Errors.size() == 2);
			CHECK(fixture.Errors[0].Count == 1);
			CHECK(fixture.Errors[1].Count == 2);
			CHECK(expected.GetMatchCount() == 2);
			CHECK(services.Claims[0].ID == services.Errors[0].ID);
			CHECK(services.Claims[1].ID == services.Errors[1].ID);
			CHECK(services.Claims[0].ID != services.Claims[1].ID);
			auto timedOut = Resume(fixture, thread, 1);
			CHECK(timedOut.State == ScriptCaseState::Failed);
			CHECK_FALSE(timedOut.Error.has_value());
			REQUIRE(services.Reports.size() == 1);
			CHECK(services.Reports[0].Line == 10);
			CaptureCoverage(fixture, mode, snapshots);
		}

	}

	namespace Test {

		void RunTestBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			RunSuiteScenario(mode, snapshots);
			RunAssertionsScenario(mode, snapshots);
			RunWaitsScenario(mode, snapshots);
			RunInputScenario(mode, snapshots);
			RunClaimsScenario(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("TestBindings: suite collection retains ordered cases and effective timeout options without executing them")
		{
			std::vector<ScriptApiCoverage> snapshots;
			RunSuiteScenario(RunModes::Editor, snapshots);
		}

		TEST_CASE("TestBindings: expectations accumulate while fail and skip remain terminal through pcall")
		{
			std::vector<ScriptApiCoverage> snapshots;
			RunAssertionsScenario(RunModes::Editor, snapshots);
		}

		TEST_CASE("TestBindings: waits use simulation deadlines and audio resumes only after host readiness")
		{
			std::vector<ScriptApiCoverage> snapshots;
			RunWaitsScenario(RunModes::Editor, snapshots);
		}

		TEST_CASE("TestBindings: nonyieldable and malformed waits fail before capture or predicate effects")
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &services });
			REQUIRE(fixture.Start());
			CheckTrue(fixture, R"(
local called=false
assert(not pcall(Test.WaitTicks, 1))
assert(not pcall(Test.WaitUntil, function() called=true; return true end, 1))
assert(not pcall(Test.ExpectScriptError, "boom", 1))
assert(not pcall(Test.CaptureAudio, 2))
for _, n in {-1, 0.5, 4294967296, math.huge, 0/0, "1"} do
	assert(not pcall(Test.WaitTicks, n))
	assert(not pcall(Test.CaptureAudio, n))
end
return not called
)");
			CHECK(services.AudioBegins == 0);
			CHECK(services.Claims.empty());
			CHECK(services.Reports.empty());
		}

		TEST_CASE("TestBindings: input and observation APIs cross only the declared test host boundary")
		{
			std::vector<ScriptApiCoverage> snapshots;
			RunInputScenario(RunModes::Editor, snapshots);
		}

		TEST_CASE("TestBindings: expected error patterns claim individual occurrences without hiding their counts")
		{
			std::vector<ScriptApiCoverage> snapshots;
			RunClaimsScenario(RunModes::Editor, snapshots);
		}

		TEST_CASE("TestBindings: cancellation stops a pending capture and its case owned tasks")
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &services });
			auto suite = Collect(fixture, services, R"(return Test.Suite("Cancel",function()
	Test.Case("capture",function()
		Task.Delay(1,function() Test.Fail("orphaned") end)
		Test.CaptureAudio(10)
	end)
end))");
			const auto thread = StartCase(fixture, suite.Cases[0]);
			CHECK(Resume(fixture, thread, 0).State == ScriptCaseState::Yielded);
			REQUIRE(services.Capturing);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			fixture.GetEngine()->CancelCase(thread);
			fixture.GetEngine()->CancelCase(thread);
			CHECK_FALSE(services.Capturing);
			CHECK(services.AudioCancels == 1);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			fixture.Frame.Tick = 10000;
			fixture.GetEngine()->ResumeTasks();
			CHECK(services.Reports.empty());
			CHECK_FALSE(fixture.GetEngine()->ResumeCase(thread));
		}

		TEST_CASE("TestBindings: registry rejects ordinary play and read only mutations before host effects")
		{
			SUBCASE("runtime")
			{
				Test::ScriptTestFixture fixture;
				REQUIRE(fixture.Start());
				CheckTrue(fixture, R"(
local ran=false
assert(Test.Suite("Pure",function() ran=true end)~=nil)
assert(not pcall(Test.Expect,true))
assert(not pcall(Test.InjectKey,"Space","Down"))
assert(not pcall(Test.CaptureAudio,1))
assert(not pcall(Test.GetStateHash))
return not ran
)");
			}
			SUBCASE("read only")
			{
				TestServices services;
				Test::ScriptTestFixture fixture({ .TestMode = true, .ReadOnly = true, .TestHost = &services });
				REQUIRE(fixture.Start());
				CheckTrue(fixture, R"(
assert(not pcall(Test.InjectKey,"Space","Down"))
assert(not pcall(Test.Screenshot,"blocked"))
assert(not pcall(Test.CaptureAudio,1))
return Test.GetStateHash()=="0000000000001234"
)");
				CHECK(services.Inputs.empty());
				CHECK(services.Screenshots.empty());
				CHECK(services.AudioBegins == 0);
			}
		}

		TEST_CASE("TestBindings: every registered member has its frozen availability and mutation metadata")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			const std::array<std::string_view, 21> names{ "Suite", "Case", "Expect", "ExpectEqual", "ExpectNear", "Fail", "Skip", "WaitTicks", "WaitUntil", "ExpectScriptError", "CaptureAudio", "InjectAction", "InjectKey", "InjectMouse", "InjectGamepad", "Screenshot", "ReloadScript", "GetStateHash", "GetLastExtraction", "GetExtractedPosition", "GetScriptErrors" };
			const auto modules = fixture.GetApi().GetModules();
			const auto found = std::find_if(modules.begin(), modules.end(), [](const auto& group)
			{
				return group.Name == "Test";
			});
			REQUIRE(found != modules.end());
			REQUIRE(found->Members.size() == names.size());
			for (const auto name : names)
			{
				CAPTURE(name);
				const auto member = std::find_if(found->Members.begin(), found->Members.end(), [name](const auto& entry)
				{
					return entry.Name == name;
				});
				REQUIRE(member != found->Members.end());
				CHECK(member->Options.Environments == (name == "Suite" ? ScriptApiEnvironment::All : ScriptApiEnvironment::Test));
				CHECK(member->Options.Modes == (name == "ReloadScript" ? RunModes::EditorOnly : RunModes::All));
				const bool mutation = name.starts_with("Inject") || name == "CaptureAudio" || name == "Screenshot" || name == "ReloadScript";
				CHECK(member->Options.Mutates == mutation);
				CHECK_FALSE(member->Signature.empty());
				CHECK_FALSE(member->Description.empty());
			}
		}
	}

}
