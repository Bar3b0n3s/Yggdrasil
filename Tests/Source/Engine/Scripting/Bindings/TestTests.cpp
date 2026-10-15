#include "TestsPCH.h"
#include "Engine/Scripting/ScriptTestHost.h"

#include "Engine/Platform/Input/KeyCodes.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Scripting/TaskScheduler.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <string_view>
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
			// Slow host work: a capture or a reload advances the safety clock by HostSeconds, and a reload runs through
			// ReloadEngine->RunHostOperation as the editor host's reimport and type check do.
			double* Clock = nullptr;
			double HostSeconds = 0.0;
			ScriptEngine* ReloadEngine = nullptr;

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
				if (Clock != nullptr)
					*Clock += HostSeconds;
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
				if (ReloadEngine == nullptr)
					return {};
				return ReloadEngine->RunHostOperation([this]() -> Status
				{
					if (Clock != nullptr)
						*Clock += HostSeconds;
					return {};
				});
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
			// §11.10's documented text.
			CheckTrue(fixture, "local ok, message = pcall(Test.Case, 'outside', function() end); return not ok and string.find(message, 'Test.Case outside Test.Suite', 1, true) ~= nil");
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
			CHECK(services.Reports[1].Message == "equality (got 1, expected 2)");
			CHECK(services.Reports[2].Message == "distance (got 1, expected 1.5, epsilon 0.1)");
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
			// Every value of each enum slot with at most 16 values, through each overload, so per-value counters exist and run.
			std::string names[3];
			const auto list = [](std::string& target, std::string_view name)
			{
				if (!name.empty())
					target += std::format("\"{}\",", name);
			};
			for (size_t i = 0; i < MouseButtonCount; ++i)
				list(names[0], MouseButtonToString(static_cast<MouseButton>(i)));
			for (size_t i = 0; i < GamepadButtonCount; ++i)
				list(names[1], GamepadButtonToString(static_cast<GamepadButton>(i)));
			for (size_t i = 0; i < GamepadAxisCount; ++i)
				list(names[2], GamepadAxisToString(static_cast<GamepadAxis>(i)));
			CheckTrue(fixture, std::format(R"(
for _, state in {{"Down", "up", "TAP"}} do
	Test.InjectAction("Jump", state)
	Test.InjectKey("Space", state)
	Test.InjectMouse("Left", state)
	Test.InjectGamepad(0, "South", state)
end
for _, button in {{{0}}} do Test.InjectMouse(button, "Tap") end
for _, button in {{{1}}} do Test.InjectGamepad(1, button, "Down") end
for _, axis in {{{2}}} do Test.InjectGamepad(1, axis, 0.5) end
Test.InjectAction("Move", "Down", 0.75)
return true
)",
								   names[0], names[1], names[2]));
			CHECK(services.Inputs[6] == Json{ { "type", "action" }, { "name", "Jump" }, { "state", "Down" } });
			// Case-insensitive input reaches the host in canonical spelling.
			CHECK(services.Inputs[10] == Json{ { "type", "action" }, { "name", "Jump" }, { "state", "Up" } });
			CHECK(services.Inputs[17] == Json{ { "type", "gamepadButton" }, { "gamepad", 0 }, { "button", "South" }, { "state", "Tap" } });
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

		// ADR 0022 "Integration": Test.Screenshot and Test.ReloadScript block in host code (rendering and encoding a capture; a
		// reimport with its type check), which ran inside the case's callback budget, so suites timed out on a loaded machine.
		// Host work that runs no Luau is now excluded from the budget, while the script's own Luau time still counts.
		TEST_CASE("TestBindings: slow host work inside a case stays out of the callback budget while a Luau spin still times out")
		{
			double now = 0.0;
			bool spinning = false;
			TestServices services;
			services.Clock = &now;
			services.HostSeconds = 10.0;
			Test::ScriptTestFixtureSpecification specification{ .Mode = RunModes::Editor, .TestMode = true, .TestHost = &services };
			specification.Settings.CallbackBudgetMs = 500;
			specification.ClockSeconds = [&now, &spinning]
			{
				if (spinning)
					now += 0.02;
				return now;
			};
			Test::ScriptTestFixture fixture(specification);
			REQUIRE(fixture.AddScript(UUID(733), "Assets/Scripts/Reload.luau", "return {}"));
			auto suite = Collect(fixture, services, R"(return Test.Suite("Host", function()
	Test.Case("screenshot", function()
		Test.Screenshot("slow")
		for _ = 1, 3 do Test.Expect(true) end
	end)
	Test.Case("reload", function()
		Test.ReloadScript(Assets.Load("Assets/Scripts/Reload.luau"))
		for _ = 1, 3 do Test.Expect(true) end
	end)
	Test.Case("spin", function()
		while true do end
	end)
end))");
			services.ReloadEngine = fixture.GetEngine();
			REQUIRE(suite.Cases.size() == 3);
			for (size_t index = 0; index < 2; ++index)
			{
				CAPTURE(suite.Cases[index].Name);
				const auto thread = StartCase(fixture, suite.Cases[index]);
				const ScriptCaseResume result = Resume(fixture, thread, 0);
				CHECK(result.State == ScriptCaseState::Completed);
				CHECK_FALSE(result.Error.has_value());
				fixture.GetEngine()->CancelCase(thread);
			}
			// Each host call took 10 s of the safety clock, twenty times the 500 ms budget.
			CHECK(now == doctest::Approx(20.0));
			CHECK(services.Screenshots == std::vector<std::string>{ "slow" });
			CHECK(services.Reloads == std::vector<AssetHandle>{ UUID(733) });
			CHECK(services.Reports.empty());
			CHECK(fixture.Errors.empty());
			Test::ExpectLog expected(LogLevel::Error, "script exceeded 500 ms in");
			spinning = true;
			const auto thread = StartCase(fixture, suite.Cases[2]);
			const ScriptCaseResume spin = Resume(fixture, thread, 0);
			CHECK(spin.State == ScriptCaseState::Failed);
			REQUIRE(spin.Error.has_value());
			CHECK(spin.Error->Kind == ScriptErrorKind::Timeout);
			CHECK(expected.GetMatchCount() >= 1);
			fixture.GetEngine()->CancelCase(thread);
			fixture.GetEngine()->EndSuite();
		}

		TEST_CASE("TestBindings: input and observation APIs cross only the declared test host boundary")
		{
			std::vector<ScriptApiCoverage> snapshots;
			RunInputScenario(RunModes::Editor, snapshots);
		}

		TEST_CASE("TestBindings: input injection enums are declared typed and validated before the host")
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &services });
			REQUIRE(fixture.Start());
			CheckTrue(fixture, R"(
for _, fn in {
	function() Test.InjectKey("Spcae", "Down") end,
	function() Test.InjectKey("Space", "Hold") end,
	function() Test.InjectMouse("Thumb", "Tap") end,
	function() Test.InjectMouse("Left", "Pressed") end,
	function() Test.InjectAction("Jump", "Held") end,
	function() Test.InjectAction("Jump") end,
	function() Test.InjectAction("Move", "Up", 0.5) end,
	function() Test.InjectGamepad(0, "LeftX", "Down") end,
	function() Test.InjectGamepad(0, "South", 0.5) end,
	function() Test.InjectGamepad(0, "South", "Sideways") end,
	function() Test.InjectGamepad(0, "South") end,
} do
	local ok, message = pcall(fn)
	assert(not ok, "accepted an invalid input")
end
local ok, message = pcall(Test.InjectKey, "Spcae", "Down")
assert(string.find(message, "unknown Key 'Spcae'", 1, true), message)
return true
)");
			CHECK(services.Inputs.empty());
			std::string declarations;
			{
				const auto generated = fixture.GetApi().GenerateDefinitions();
				REQUIRE(generated.has_value());
				declarations = *generated;
			}
			CHECK(declarations.find("export type InputEventState = \"Down\" | \"Up\" | \"Tap\"") != std::string::npos);
			CHECK(declarations.find("InjectKey: (key: Key, state: InputEventState) -> ()") != std::string::npos);
			CHECK(declarations.find("InjectMouse: (button: MouseButton, state: InputEventState, position: vector?) -> ()") != std::string::npos);
			CHECK(declarations.find("InjectAction: (name: string, state: InputEventState?, value: number?) -> ()") != std::string::npos);
			CHECK(declarations.find("InjectGamepad: ((index: number, button: GamepadButton, state: InputEventState) -> ()) & ((index: number, axis: GamepadAxis, value: number) -> ())") != std::string::npos);
			// Each slot with at most 16 values has one counter per value and overload.
			const auto coverage = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(coverage.has_value());
			const auto counter = std::ranges::find_if(coverage->Members, [](const ScriptApiCounter& entry)
			{
				return entry.Owner == "Test" && entry.Member == "InjectGamepad";
			});
			REQUIRE(counter != coverage->Members.end());
			const auto* buttons = fixture.GetApi().FindEnum("GamepadButton");
			const auto* axes = fixture.GetApi().FindEnum("GamepadAxis");
			REQUIRE(buttons != nullptr);
			REQUIRE(axes != nullptr);
			CHECK(counter->EnumValues.size() == buttons->Values.size() + 3 + axes->Values.size());
		}

		TEST_CASE("TestBindings: expected error patterns claim individual occurrences without hiding their counts")
		{
			std::vector<ScriptApiCoverage> snapshots;
			RunClaimsScenario(RunModes::Editor, snapshots);
		}

		TEST_CASE("TestBindings: reported messages show authored locations and the text the runner keeps is bounded")
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &services });
			auto suite = Collect(fixture, services, std::format(R"(return Test.Suite("Bounded", function()
	assert(not pcall(Test.Suite, string.rep("s", {0} + 1), function() end))
	assert(not pcall(Test.Case, string.rep("n", {0} + 1), function() end))
	Test.Case(string.rep("n", {0}), function()
		local ok, caught = pcall(function() local t = nil; return t.x end)
		Test.Expect(ok, caught)
		Test.ExpectEqual(1, 2, caught)
		Test.Expect(false, string.rep("m", 2^20))
		local accepted, message = pcall(Test.ExpectScriptError, string.rep("p", {1} + 1), 1)
		assert(not accepted and string.find(message, "the limit is {1}", 1, true), message)
		Test.Fail(caught)
	end)
end))",
														MaxScriptTestNameBytes, MaxScriptErrorPatternBytes));
			REQUIRE(suite.Cases.size() == 1);
			CHECK(suite.Cases[0].Name == std::string(MaxScriptTestNameBytes, 'n'));
			const auto thread = StartCase(fixture, suite.Cases[0]);
			const auto result = Resume(fixture, thread, 0);
			CHECK(result.State == ScriptCaseState::Failed);
			REQUIRE(services.Reports.size() == 4);
			for (const auto& report : services.Reports)
			{
				CAPTURE(report.Message);
				CHECK(report.Message.find("engine/") == std::string::npos);
				CHECK(report.Message.size() <= MaxScriptLogMessageBytes);
			}
			// A VM error a script caught keeps its authored location in every reported message, never the private chunk name.
			CHECK(services.Reports[0].Message.starts_with("Assets/Tests/Bindings.luau:5: attempt to index nil"));
			CHECK(services.Reports[1].Message == services.Reports[0].Message + " (got 1, expected 2)");
			const std::string marker = std::format(" [truncated: {} bytes]", 1048576);
			CHECK(services.Reports[2].Message == std::string(MaxScriptLogMessageBytes - marker.size(), 'm') + marker);
			CHECK(services.Reports[3].Signal == ScriptTestSignal::Fail);
			CHECK(services.Reports[3].Message == services.Reports[0].Message);
			CHECK(result.Message == services.Reports[0].Message);
			fixture.GetEngine()->CancelCase(thread);
		}

		// M16 friction S1-08: a failed ExpectEqual or ExpectNear reported only its message, so a failing row never said which
		// values it compared.
		TEST_CASE("TestBindings: ExpectEqual and ExpectNear failures report the actual and expected values and the epsilon")
		{
			TestServices services;
			Test::ScriptTestFixture fixture({ .Mode = RunModes::Editor, .TestMode = true, .TestHost = &services });
			const UUID entity = fixture.GetScene().CreateEntity("Visible").GetUUID();
			REQUIRE(fixture.AddScript(UUID(734), "Assets/Scripts/Value.luau", "return {}"));
			auto suite = Collect(fixture, services, R"(return Test.Suite("Values", function()
	Test.Case("values", function()
		Test.ExpectEqual(1, 2)
		Test.ExpectEqual("a\"b\n", "c", "names")
		Test.ExpectEqual(true, nil, "flags")
		Test.ExpectNear(0.5, 0.25, 0.125, "speed")
		Test.ExpectNear(vector.create(1, 2, 3), vector.create(1, 2, 3.5))
		Test.ExpectEqual(Color.New(1, 0, 0), Color.New(0, 0.5, 0, 0.25))
		Test.ExpectEqual(Quat.Identity(), Quat.New(0, 0, 1, 0))
		Test.ExpectEqual(Scene.FindByName("Visible"), Assets.Load("Assets/Scripts/Value.luau"))
		Test.ExpectEqual(Scene.FindByName("Visible").Transform, {}, "proxy")
		Test.ExpectEqual("x" .. string.rep("\u{E9}", 100), print)
		Test.ExpectEqual(1, 2, string.rep("m", 2^20))
		Test.ExpectEqual(2, 2, "equal values report nothing")
		Test.ExpectNear(1, 1.000001, nil, "near values report nothing")
	end)
end))");
			REQUIRE(suite.Cases.size() == 1);
			const auto thread = StartCase(fixture, suite.Cases[0]);
			CHECK(Resume(fixture, thread, 0).State == ScriptCaseState::Completed);
			fixture.GetEngine()->CancelCase(thread);
			// "x" and 100 two-byte characters: the first 128 bytes end inside the 64th, which the row leaves out.
			std::string accented;
			for (int i = 0; i < 63; ++i)
				accented += "\xC3\xA9";
			const std::string marker = std::format(" [truncated: {} bytes]", 1048576);
			const std::array<std::string, 11> expected{
				"expected values to be equal (got 1, expected 2)",
				R"(names (got "a\"b\n", expected "c"))",
				"flags (got true, expected nil)",
				"speed (got 0.5, expected 0.25, epsilon 0.125)",
				"values differ by more than epsilon (got vector(1, 2, 3), expected vector(1, 2, 3.5), epsilon 1e-05)",
				"expected values to be equal (got Color(1, 0, 0, 1), expected Color(0, 0.5, 0, 0.25))",
				"expected values to be equal (got Quat(0, 0, 0, 1), expected Quat(0, 0, 1, 0))",
				std::format("expected values to be equal (got Entity({}), expected AssetRef({}))", entity.ToString(), UUID(734).ToString()),
				"proxy (got Transform, expected table)",
				std::format("expected values to be equal (got \"x{}\"... (201 bytes), expected function)", accented),
				std::string(MaxScriptLogMessageBytes - marker.size() - 20, 'm') + marker + " (got 1, expected 2)",
			};
			REQUIRE(services.Reports.size() == expected.size());
			for (size_t index = 0; index < expected.size(); ++index)
			{
				CAPTURE(index);
				CHECK(services.Reports[index].Signal == ScriptTestSignal::ExpectationFailed);
				CHECK(services.Reports[index].Message == expected[index]);
				CHECK(services.Reports[index].Line == index + 3);
			}
			CHECK(services.Reports.back().Message.size() == MaxScriptLogMessageBytes);
			CHECK(services.Errors.empty());
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
