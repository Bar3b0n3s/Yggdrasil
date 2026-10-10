#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace Engine {

	namespace {

		Result<ScriptEvaluation> EvaluateReload(Test::ScriptTestFixture& fixture, std::string_view source, std::optional<UUID> entity = std::nullopt)
		{
			ENGINE_TRY_ASSIGN(auto path, VfsPath::Create("project", "Assets/HotReloadProbe.luau"));
			return fixture.GetEngine()->Evaluate(source, path, entity);
		}

		class ReloadTestHost final : public IScriptTestHost
		{
		public:
			void Report(const ScriptTestReport& report) override { Reports.push_back(report); }
			void OnScriptError(const ScriptError& error, bool fatal) override
			{
				Errors.push_back(error);
				Fatal = Fatal || fatal;
			}
			void OnExpectedScriptError(const ScriptError&) override {}
			Status InjectInput(const Json&) override { return MakeError(ErrorCode::Unsupported, "input is unused by reload tests"); }
			Status CaptureScreenshot(std::string_view) override { return MakeError(ErrorCode::Unsupported, "screenshots are unused by reload tests"); }
			Status BeginAudioCapture(uint32_t) override { return MakeError(ErrorCode::Unsupported, "audio is unused by reload tests"); }
			bool IsAudioCaptureReady() const override { return false; }
			Result<AudioLevels> EndAudioCapture() override { return MakeError(ErrorCode::Unsupported, "audio is unused by reload tests"); }
			void CancelAudioCapture() override {}
			Status ReloadScript(AssetHandle script) override
			{
				const auto result = Engine->Reload(script, true);
				if (!result)
					return std::unexpected(result.error());
				return {};
			}
			uint64_t ComputeStateHash() const override { return 0; }
			ScriptExtractionState GetLastExtraction() const override { return {}; }
			std::optional<glm::vec3> GetExtractedPosition(UUID) const override { return std::nullopt; }
			void OnDebugBreak() override {}

			ScriptEngine* Engine = nullptr; // borrowed only while the fixture's engine is alive
			std::vector<ScriptTestReport> Reports{};
			std::vector<ScriptError> Errors{};
			bool Fatal = false;
		};

		// Runtime top-level failures can depend on live state even after a successful import. Compile their exact
		// engine bytecode here while retaining the previously validated declaration/schema, as a published reload.
		Status PublishReloadBody(Test::ScriptTestFixture& fixture, AssetHandle handle,
			const ScriptData& declaration, std::string_view body)
		{
			ENGINE_TRY_ASSIGN(auto path, VfsPath::Create("project", declaration.SourceMap.Path));
			ENGINE_TRY_ASSIGN(auto compilation, ScriptCompiler::Compile({ .Path = path, .Source = body }));
			auto script = CreateRef<ScriptData>();
			script->Kind = declaration.Kind;
			script->Name = declaration.Name;
			script->Fields = declaration.Fields;
			script->Requires = declaration.Requires;
			script->Bytecode = std::move(compilation.Bytecode);
			script->SourceMap = std::move(compilation.SourceMap);
			fixture.GetAssetManager().Publish(handle, script, declaration.SourceMap.Path);
			return {};
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("HotReload: instance state survives")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle handle(810);
			REQUIRE(fixture.AddScript(handle, "Assets/Reload.luau", R"(
local C = { Fields = { Count = Field.Integer(3), Removed = Field.String("retained") } }
function C:OnCreate() self.Created = (self.Created or 0) + 1 end
function C:Value() return 10 end
return Script.Define("Reload", C)
)")
					.has_value());
			const Entity entity = fixture.GetScene().CreateEntity("Reload");
			entity.AddComponent<ScriptComponent>().Script.SetHandle(handle);
			REQUIRE(fixture.Start().has_value());
			REQUIRE(EvaluateReload(fixture, "self.Count = 41; self.NilState = nil; self.Saved = self.Value; self.Class = getmetatable(self)", entity.GetUUID()).has_value());
			REQUIRE(fixture.AddScript(handle, "Assets/Reload.luau", R"(
local C = { Fields = { Count = Field.Integer(99), NewField = Field.String("new") } }
function C:Value() return 20 end
function C:OnHotReload() self.Reloaded = (self.Reloaded or 0) + 1 end
return Script.Define("Reload", C)
)")
					.has_value());
			const auto reloaded = fixture.GetEngine()->Reload(handle);
			REQUIRE(reloaded.has_value());
			CHECK_FALSE(reloaded->Deferred);
			CHECK(reloaded->Scripts == std::vector<AssetHandle>{ handle });
			const auto state = EvaluateReload(fixture, R"(
assert(self.Class == getmetatable(self))
return {self.Count, self.Removed, self.NewField, self.Created, self.Reloaded, self:Value(), self:Saved()}
)",
				entity.GetUUID());
			REQUIRE(state.has_value());
			CHECK(state->Value.Get() == Json::array({ 41, "retained", "new", 1, 1, 20, 10 }));
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("HotReload: editing a required module updates dependents")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle leaf(820), middle(821), behaviour(822);
			REQUIRE(fixture.AddScript(leaf, "Assets/Leaf.luau", "return { Value = function() return 2 end }").has_value());
			REQUIRE(fixture.AddScript(middle, "Assets/Middle.luau", R"(
local Leaf = require("./Leaf")
local value = Leaf.Value
return { Value = function() return value() * 3 end }
)")
					.has_value());
			REQUIRE(fixture.AddScript(behaviour, "Assets/Dependent.luau", R"(
local Middle = require("./Middle")
local value = Middle.Value
local C = {}
function C:Value() return value() end
return Script.Define("Dependent", C)
)")
					.has_value());
			const Entity entity = fixture.GetScene().CreateEntity("Dependent");
			entity.AddComponent<ScriptComponent>().Script.SetHandle(behaviour);
			REQUIRE(fixture.Start().has_value());
			REQUIRE(EvaluateReload(fixture, "self.Leaf = require('./Leaf'); self.Middle = require('./Middle'); self.Old = self.Middle.Value", entity.GetUUID()).has_value());
			REQUIRE(fixture.AddScript(leaf, "Assets/Leaf.luau", "return { Value = function() return 5 end }").has_value());
			const auto reloaded = fixture.GetEngine()->Reload(leaf);
			REQUIRE(reloaded.has_value());
			CHECK(reloaded->Scripts == std::vector<AssetHandle>{ leaf, middle, behaviour });
			const auto state = EvaluateReload(fixture, R"(
assert(self.Leaf == require("./Leaf"))
assert(self.Middle == require("./Middle"))
return {self:Value(), self.Middle.Value(), self.Leaf.Value(), self.Old()}
)",
				entity.GetUUID());
			REQUIRE(state.has_value());
			CHECK(state->Value.Get() == Json::array({ 15, 15, 5, 6 }));
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("HotReload: deferred while lockstep owns time")
		{
			for (const RunModes mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				CAPTURE(mode);
				Test::ScriptTestFixture fixture({ .Mode = mode });
				const AssetHandle handle(830);
				REQUIRE(fixture.AddScript(handle, "Assets/Deferred.luau", R"(
local C = {}
function C:Value() return 1 end
return Script.Define("Deferred", C)
)")
						.has_value());
				const Entity entity = fixture.GetScene().CreateEntity("Deferred");
				entity.AddComponent<ScriptComponent>().Script.SetHandle(handle);
				REQUIRE(fixture.Start().has_value());
				REQUIRE(fixture.AddScript(handle, "Assets/Deferred.luau", R"(
local C = {}
function C:Value() return 2 end
return Script.Define("Deferred", C)
)")
						.has_value());
				fixture.ReloadDeferred = true; // the host applies this same policy for lockstep, replay, recording and suites
				const auto reloaded = fixture.GetEngine()->Reload(handle);
				if (mode == RunModes::Editor)
				{
					REQUIRE(reloaded.has_value());
					CHECK(reloaded->Deferred);
					CHECK(reloaded->Scripts.empty());
				}
				else
				{
					REQUIRE_FALSE(reloaded.has_value());
					CHECK(reloaded.error().GetCode() == ErrorCode::Unsupported);
				}
				const auto original = EvaluateReload(fixture, "return self:Value()", entity.GetUUID());
				REQUIRE(original.has_value());
				CHECK(original->Value.Get() == Json(1));
				CHECK(fixture.ExternalMutations.empty());
				CHECK(fixture.Errors.empty());
				CHECK_FALSE(fixture.GetEngine()->Reload(handle, true).has_value());
				if (mode == RunModes::Editor)
				{
					fixture.ReloadDeferred = false;
					REQUIRE(fixture.GetEngine()->Reload(handle).has_value());
					const auto changed = EvaluateReload(fixture, "return self:Value()", entity.GetUUID());
					REQUIRE(changed.has_value());
					CHECK(changed->Value.Get() == Json(2));
				}
			}
		}

		TEST_CASE("HotReload: a failing dependent rolls back the chain")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle leaf(840), behaviour(841);
			const auto originalLeaf = fixture.AddScript(leaf, "Assets/AtomicLeaf.luau", "return { Value = function() return 7 end }");
			REQUIRE(originalLeaf.has_value());
			const auto originalBehaviour = fixture.AddScript(behaviour, "Assets/AtomicBehaviour.luau", R"(
local Leaf = require("./AtomicLeaf")
local C = { Fields = { Count = Field.Integer(3) } }
function C:Value() return Leaf.Value() end
return Script.Define("Atomic", C)
)");
			REQUIRE(originalBehaviour.has_value());
			const Entity entity = fixture.GetScene().CreateEntity("Atomic");
			entity.AddComponent<ScriptComponent>().Script.SetHandle(behaviour);
			REQUIRE(fixture.Start().has_value());
			REQUIRE(EvaluateReload(fixture, "self.Count = 91; self.Leaf = require('./AtomicLeaf'); self.Old = self.Leaf.Value; self.Class = getmetatable(self)", entity.GetUUID()).has_value());
			REQUIRE(fixture.AddScript(leaf, "Assets/AtomicLeaf.luau", "return { Value = function() return 9 end }").has_value());
			REQUIRE(PublishReloadBody(fixture, behaviour, **originalBehaviour,
				"local Leaf = require('./AtomicLeaf')\nassert(Leaf.Value() == 9)\nerror('late dependent fault')")
					.has_value());
			Test::ExpectLog expectedFailure(LogLevel::Error, "late dependent fault");
			CHECK_FALSE(fixture.GetEngine()->Reload(leaf).has_value());
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors.front().Script == "Assets/AtomicBehaviour.luau");
			CHECK(fixture.Errors.front().Line == 3);
			CHECK(fixture.Errors.front().Message.find("late dependent fault") != std::string::npos);
			const auto original = EvaluateReload(fixture, R"(
assert(self.Leaf == require("./AtomicLeaf"))
assert(self.Class == getmetatable(self))
return {self.Count, self:Value(), self.Leaf.Value(), self.Old()}
)",
				entity.GetUUID());
			REQUIRE(original.has_value());
			CHECK(original->Value.Get() == Json::array({ 91, 7, 7, 7 }));
			// The cache transaction must have ended, so a later valid retry adopts the already published new leaf.
			fixture.GetAssetManager().Publish(behaviour, *originalBehaviour);
			REQUIRE(fixture.GetEngine()->Reload(leaf).has_value());
			const auto retried = EvaluateReload(fixture, "return {self.Count, self:Value()}", entity.GetUUID());
			REQUIRE(retried.has_value());
			CHECK(retried->Value.Get() == Json::array({ 91, 9 }));
			CHECK(fixture.Errors.size() == 1);
		}

		TEST_CASE("HotReload: successful replacement recovers a fault-disabled instance")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle handle(850);
			REQUIRE(fixture.AddScript(handle, "Assets/Recover.luau", R"(
local C = { Fields = { Count = Field.Integer(0) } }
function C:OnFixedUpdate() self.Count += 1; error("disabled before reload") end
return Script.Define("Recover", C)
)")
					.has_value());
			const Entity entity = fixture.GetScene().CreateEntity("Recover");
			entity.AddComponent<ScriptComponent>().Script.SetHandle(handle);
			REQUIRE(fixture.Start().has_value());
			Test::ExpectLog expectedFailure(LogLevel::Error, "disabled before reload");
			fixture.GetEngine()->FixedUpdate();
			REQUIRE(fixture.Errors.size() == 1);
			REQUIRE(fixture.GetEngine()->GetInstances().size() == 1);
			CHECK(fixture.GetEngine()->GetInstances().front().Disabled);
			REQUIRE(fixture.AddScript(handle, "Assets/Recover.luau", R"(
local C = { Fields = { Count = Field.Integer(0) } }
function C:OnFixedUpdate() self.Count += 10 end
function C:OnHotReload() self.Recovered = true end
return Script.Define("Recover", C)
)")
					.has_value());
			REQUIRE(fixture.GetEngine()->Reload(handle).has_value());
			CHECK_FALSE(fixture.GetEngine()->GetInstances().front().Disabled);
			fixture.GetEngine()->FixedUpdate();
			const auto state = EvaluateReload(fixture, "return {self.Count, self.Recovered}", entity.GetUUID());
			REQUIRE(state.has_value());
			CHECK(state->Value.Get() == Json::array({ 11, true }));
			CHECK(fixture.Errors.size() == 1);
		}
		TEST_CASE("HotReload: isolated candidates preserve unchanged dependency state and reconnect exported aliases")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle dependency(860), behaviour(861), added(862);
			REQUIRE(fixture.AddScript(dependency, "Assets/Stateful.luau", R"(
local count = 0
local state = { Count = 0 }
return {
	State = state,
	Bump = function() count += 1; state.Count = count; return count end,
	Peek = function() return count end,
}
)")
					.has_value());
			const auto original = fixture.AddScript(behaviour, "Assets/Isolated.luau", R"(
local Dependency = require("./Stateful")
local C = {}
function C:Read() return Dependency.Peek() end
return Script.Define("Isolated", C)
)");
			REQUIRE_MESSAGE(original.has_value(), (original ? "" : original.error().ToString()));
			const Entity entity = fixture.GetScene().CreateEntity("Isolated");
			entity.AddComponent<ScriptComponent>().Script.SetHandle(behaviour);
			REQUIRE(fixture.Start().has_value());
			REQUIRE(EvaluateReload(fixture, R"(
local D = require("./Stateful")
D.Bump(); D.Bump(); D.Bump()
self.Dependency = D; self.State = D.State; self.Old = self.Read
)",
				entity.GetUUID())
					.has_value());
			REQUIRE(PublishReloadBody(fixture, behaviour, **original,
				"local D = require('./Stateful'); assert(D.Bump() == 1); error('isolated failure')")
					.has_value());
			Test::ExpectLog expectedFailure(LogLevel::Error, "isolated failure");
			CHECK_FALSE(fixture.GetEngine()->Reload(behaviour).has_value());
			REQUIRE(fixture.Errors.size() == 1);
			const auto retained = EvaluateReload(fixture, "return {self:Read(), self.Dependency.Peek(), self.State.Count}", entity.GetUUID());
			REQUIRE_MESSAGE(retained.has_value(), (retained ? "" : retained.error().ToString()));
			CHECK(retained->Value.Get() == Json::array({ 3, 3, 3 }));
			REQUIRE(fixture.AddScript(added, "Assets/NewDependency.luau", R"(
local peek = require("./Stateful").Peek
return { Read = function() return peek() end }
)")
					.has_value());
			REQUIRE(fixture.AddScript(behaviour, "Assets/Isolated.luau", R"(
DependencyGlobal = require("./Stateful")
local peek, state = DependencyGlobal.Peek, DependencyGlobal.State
local initial = DependencyGlobal.Bump()
local added = require("./NewDependency")
local C = {}
function C:Read() return {initial, peek(), state.Count, DependencyGlobal.Peek(), added.Read()} end
function C:Increment() return DependencyGlobal.Bump() end
return Script.Define("Isolated", C)
)")
					.has_value());
			const auto reloaded = fixture.GetEngine()->Reload(behaviour);
			REQUIRE_MESSAGE(reloaded.has_value(), (reloaded ? "" : reloaded.error().ToString()));
			const auto state = EvaluateReload(fixture, R"(
assert(self.Dependency == require("./Stateful"))
assert(self.State == require("./Stateful").State)
assert(self:Old() == 3)
assert(self:Increment() == 4)
return self:Read()
)",
				entity.GetUUID());
			REQUIRE_MESSAGE(state.has_value(), (state ? "" : state.error().ToString()));
			CHECK(state->Value.Get() == Json::array({ 1, 4, 4, 4, 4 }));
			CHECK(fixture.Errors.size() == 1);
		}

		TEST_CASE("HotReload: initializer API guards and private random cannot mutate the host")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle handle(870);
			const auto original = fixture.AddScript(handle, "Assets/PureReload.luau", "return { Value = 1 }");
			REQUIRE_MESSAGE(original.has_value(), (original ? "" : original.error().ToString()));
			REQUIRE(fixture.Start().has_value());
			REQUIRE(EvaluateReload(fixture, "return require('./PureReload').Value").has_value());
			const auto before = fixture.GetRandom().GetState();
			REQUIRE(PublishReloadBody(fixture, handle, **original, R"(
math.randomseed(45)
math.random()
local ok, message = pcall(function() Scene.CreateEntity("Forbidden") end)
assert(not ok and string.find(tostring(message), "not available at load time", 1, true))
assert(not pcall(function() return Time.GetTick() end))
print("private initializer output")
return { Value = 2 }
)")
					.has_value());
			const auto reloaded = fixture.GetEngine()->Reload(handle);
			REQUIRE_MESSAGE(reloaded.has_value(), (reloaded ? "" : reloaded.error().ToString()));
			CHECK(fixture.GetRandom().GetState() == before);
			CHECK(fixture.GetScene().GetEntityCount() == 0);
			CHECK(fixture.ExternalMutations.size() == 1); // publication is the sole external mutation
			CHECK(fixture.Errors.empty());
			const auto value = EvaluateReload(fixture, "return require('./PureReload').Value");
			REQUIRE_MESSAGE(value.has_value(), (value ? "" : value.error().ToString()));
			CHECK(value->Value.Get() == Json(2));
		}

		TEST_CASE("HotReload: cached nil and non-table module results update dependents")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle leaf(880), dependent(881);
			REQUIRE(fixture.AddScript(leaf, "Assets/Optional.luau", "return nil").has_value());
			REQUIRE(fixture.AddScript(dependent, "Assets/OptionalUser.luau", R"(
local value = require("./Optional")
return { Read = function() return value == nil and "missing" or value() end }
)")
					.has_value());
			REQUIRE(fixture.Start().has_value());
			const auto initial = EvaluateReload(fixture, "return require('./OptionalUser').Read()");
			REQUIRE_MESSAGE(initial.has_value(), (initial ? "" : initial.error().ToString()));
			CHECK(initial->Value.Get() == Json("missing"));
			REQUIRE(fixture.AddScript(leaf, "Assets/Optional.luau", "return function() return 13 end").has_value());
			const auto reloaded = fixture.GetEngine()->Reload(leaf);
			REQUIRE_MESSAGE(reloaded.has_value(), (reloaded ? "" : reloaded.error().ToString()));
			CHECK(reloaded->Scripts == std::vector<AssetHandle>{ leaf, dependent });
			const auto present = EvaluateReload(fixture, "return require('./OptionalUser').Read()");
			REQUIRE_MESSAGE(present.has_value(), (present ? "" : present.error().ToString()));
			CHECK(present->Value.Get() == Json(13));
			REQUIRE(fixture.AddScript(leaf, "Assets/Optional.luau", "return nil").has_value());
			REQUIRE(fixture.GetEngine()->Reload(leaf).has_value());
			const auto removed = EvaluateReload(fixture, "return require('./OptionalUser').Read()");
			REQUIRE_MESSAGE(removed.has_value(), (removed ? "" : removed.error().ToString()));
			CHECK(removed->Value.Get() == Json("missing"));
			CHECK(fixture.Errors.empty());
		}
		TEST_CASE("HotReload: preserved suite identity receives the new authenticated declaration and body")
		{
			ReloadTestHost host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const AssetHandle handle(890);
			REQUIRE(fixture.AddScript(handle, "Assets/ReloadSuite.luau", R"(
return Test.Suite("Old", function()
	Test.Case("Before", function() end)
end, { CaseTimeoutTicks = 9 })
)")
					.has_value());
			REQUIRE(fixture.Start().has_value());
			host.Engine = fixture.GetEngine();
			const auto original = host.Engine->CollectSuite(handle, host);
			REQUIRE_MESSAGE(original.has_value(), (original ? "" : original.error().ToString()));
			REQUIRE(original->Cases.size() == 1);
			CHECK(original->Name == "Old");
			CHECK(original->Cases.front().Name == "Before");
			host.Engine->EndSuite();
			REQUIRE(fixture.AddScript(handle, "Assets/ReloadSuite.luau", R"(
return Test.Suite("New", function()
	Test.Case("After", function() end)
end, { CaseTimeoutTicks = 17 })
)")
					.has_value());
			const auto reloaded = host.Engine->Reload(handle);
			REQUIRE_MESSAGE(reloaded.has_value(), (reloaded ? "" : reloaded.error().ToString()));
			const auto current = host.Engine->CollectSuite(handle, host);
			REQUIRE_MESSAGE(current.has_value(), (current ? "" : current.error().ToString()));
			REQUIRE(current->Cases.size() == 1);
			CHECK(current->Name == "New");
			CHECK(current->Cases.front().Name == "After");
			CHECK(current->Cases.front().TimeoutTicks == 17);
			host.Engine->EndSuite();
			CHECK(host.Errors.empty());
		}

		TEST_CASE("HotReload: explicit test reload uses the caller coroutine and retains its external origin")
		{
			ReloadTestHost host;
			Test::ScriptTestFixture fixture({ .TestMode = true, .TestHost = &host });
			const AssetHandle module(900), suiteHandle(901);
			REQUIRE(fixture.AddScript(module, "Assets/FromCase.luau", "return { Value = 1 }").has_value());
			REQUIRE(fixture.AddScript(suiteHandle, "Assets/CaseReload.luau", R"(
return Test.Suite("Reload", function()
	Test.Case("Live coroutine", function()
		local before = require("./FromCase")
		assert(before.Value == 1)
		Test.ReloadScript(Assets.Load("Assets/FromCase.luau"))
		assert(before == require("./FromCase"))
		assert(before.Value == 2)
	end)
end)
)")
					.has_value());
			REQUIRE(fixture.Start().has_value());
			host.Engine = fixture.GetEngine();
			REQUIRE(EvaluateReload(fixture, "return require('./FromCase').Value").has_value());
			const auto suite = host.Engine->CollectSuite(suiteHandle, host);
			REQUIRE_MESSAGE(suite.has_value(), (suite ? "" : suite.error().ToString()));
			REQUIRE(suite->Cases.size() == 1);
			REQUIRE(fixture.AddScript(module, "Assets/FromCase.luau", "return { Value = 2 }").has_value());
			fixture.ReloadDeferred = true;
			const auto thread = host.Engine->StartCase(suite->Cases.front().Function);
			REQUIRE_MESSAGE(thread.has_value(), (thread ? "" : thread.error().ToString()));
			const auto result = host.Engine->ResumeCase(*thread);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->State == ScriptCaseState::Completed);
			CHECK_FALSE(result->Error.has_value());
			CHECK(fixture.ExternalMutations.size() == 1);
			CHECK(host.Errors.empty());
			CHECK(host.Reports.empty());
			host.Engine->EndSuite();
		}
		TEST_CASE("HotReload: a candidate memory fault restores live state and releases staging before recovery")
		{
			Test::ScriptTestFixture fixture({ .Settings = { .MemoryLimitMB = 2 } });
			const AssetHandle handle(910);
			const auto original = fixture.AddScript(handle, "Assets/MemoryReload.luau", R"(
local C = { Fields = { Count = Field.Integer(7) } }
function C:Value() return 11 end
return Script.Define("MemoryReload", C)
)");
			REQUIRE_MESSAGE(original.has_value(), (original ? "" : original.error().ToString()));
			const Entity entity = fixture.GetScene().CreateEntity("MemoryReload");
			entity.AddComponent<ScriptComponent>().Script.SetHandle(handle);
			REQUIRE(fixture.Start().has_value());
			REQUIRE(EvaluateReload(fixture, "self.Count = 99; self.Class = getmetatable(self)", entity.GetUUID()).has_value());
			REQUIRE(PublishReloadBody(fixture, handle, **original, R"(
local C = { Payload = buffer.create(3 * 1024 * 1024) }
return C
)")
					.has_value());
			Test::ExpectLog expectedFailure(LogLevel::Error, "memory limit 2 MB");
			CHECK_FALSE(fixture.GetEngine()->Reload(handle).has_value());
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors.front().Kind == ScriptErrorKind::Memory);
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
			CHECK_FALSE(fixture.FatalError);
			const auto memory = fixture.GetEngine()->GetMemoryState();
			CHECK(memory.SoftBreachCount == 1);
			CHECK_FALSE(memory.NeedsRecovery);
			CHECK(memory.UsedBytes <= memory.SoftLimitBytes);
			const auto retained = EvaluateReload(fixture, "assert(self.Class == getmetatable(self)); return {self.Count, self:Value()}", entity.GetUUID());
			REQUIRE_MESSAGE(retained.has_value(), (retained ? "" : retained.error().ToString()));
			CHECK(retained->Value.Get() == Json::array({ 99, 11 }));
			fixture.GetAssetManager().Publish(handle, *original);
			const auto retried = fixture.GetEngine()->Reload(handle);
			REQUIRE_MESSAGE(retried.has_value(), (retried ? "" : retried.error().ToString()));
			CHECK(fixture.Errors.size() == 1);
		}
	}

}
