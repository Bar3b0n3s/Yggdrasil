#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Support/DeathTest.h"
#include "Support/SceneTestFixture.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <string>
#include <vector>

namespace Engine {

	namespace Test {

		void RunEntityBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunReflectedFieldCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunPhysicsBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunAudioBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunMathBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunQuatBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunColorBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunSceneBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunInputBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunTimeBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunAssetsBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunTaskBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunRandomBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunDebugBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunLogBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunApplicationBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunTestBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);
		void RunBehaviourCallbacksCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots);

	}

	static std::vector<std::string> MissingApiCoverage(const ScriptApiCoverage& required, const std::vector<ScriptApiCoverage>& snapshots)
	{
		std::vector<std::string> missing;
		for (const auto& target : required.Members)
		{
			uint64_t calls = 0, reads = 0, writes = 0;
			std::vector<uint64_t> enums(target.EnumValues.size(), 0);
			for (const auto& snapshot : snapshots)
			{
				if (snapshot.Mode != required.Mode)
					continue;
				for (const auto& observed : snapshot.Members)
				{
					if (observed.Owner != target.Owner || observed.Member != target.Member || observed.Kind != target.Kind)
						continue;
					calls += observed.Calls;
					reads += observed.Reads;
					writes += observed.Writes;
					for (size_t i = 0; i < target.EnumValues.size(); ++i)
						for (const auto& value : observed.EnumValues)
							if (value.ArgumentIndex == target.EnumValues[i].ArgumentIndex && value.EnumName == target.EnumValues[i].EnumName && value.ValueName == target.EnumValues[i].ValueName)
								enums[i] += value.Count;
				}
			}
			const std::string name = target.Owner + "." + target.Member;
			if (!target.RequiresRead && !target.RequiresWrite && calls == 0)
				missing.push_back(name + " call");
			if (target.RequiresRead && reads == 0)
				missing.push_back(name + " read");
			if (target.RequiresWrite && writes == 0)
				missing.push_back(name + " write");
			for (size_t i = 0; i < target.EnumValues.size(); ++i)
				if (enums[i] == 0)
					missing.push_back(std::format("{} argument {} {}.{}", name, target.EnumValues[i].ArgumentIndex, target.EnumValues[i].EnumName, target.EnumValues[i].ValueName));
		}
		return missing;
	}

	static int RegistryProbe(ScriptCall& call)
	{
		Lua::Push(call, int32_t(7));
		return 1;
	}
	static int RegistryRejectedProbe(ScriptCall& /*call*/)
	{
		FAIL("dispatch entered a rejected native callback");
		return 0;
	}
	static int RegistryEnumProbe(ScriptCall& call)
	{
		Lua::Push(call, static_cast<int32_t>(Lua::CheckEnum(call, 1, "ProbeChoice")));
		return 1;
	}
	static void CheckRegistryScript(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		if (!result)
			INFO(result.error().ToString());
		REQUIRE(result);
		CHECK(result->Value.Get() == true);
	}

	ENGINE_DEATH_TEST("Scripting/DuplicateApiMember")
	{
		ScriptApiRegistry api;
		api.Module("Probe", "Probe module.").Function("Read", RegistryProbe, "() -> number", "Read.", { .Mutates = false }).Function("Read", RegistryProbe, "() -> number", "Read.", { .Mutates = false });
	}
	ENGINE_DEATH_TEST("Scripting/ShortcutCollision")
	{
		Test::SceneTestFixture fixture;
		ScriptApiRegistry api;
		api.Type("Entity", "Entity.").Property("Transform", RegistryProbe, nullptr, "number", "Colliding shortcut.");
		static_cast<void>(api.Freeze(fixture.GetRegistry()));
	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptApi: every registered function, method, property, operator and constructor has a test")
		{
			// These are assertion-bearing test programs, independent of the production registration graph. They create
			// fresh fixtures and return ONLY real dispatcher observations. No declared member-name list grants coverage,
			// no earlier doctest case needs to run, and adding an API immediately adds an unobserved requirement.
			constexpr std::array Scenarios{
				&Test::RunEntityBindingsCoverage,
				&Test::RunReflectedFieldCoverage,
				&Test::RunPhysicsBindingsCoverage,
				&Test::RunAudioBindingsCoverage,
				&Test::RunMathBindingsCoverage,
				&Test::RunQuatBindingsCoverage,
				&Test::RunColorBindingsCoverage,
				&Test::RunSceneBindingsCoverage,
				&Test::RunInputBindingsCoverage,
				&Test::RunTimeBindingsCoverage,
				&Test::RunAssetsBindingsCoverage,
				&Test::RunTaskBindingsCoverage,
				&Test::RunRandomBindingsCoverage,
				&Test::RunDebugBindingsCoverage,
				&Test::RunLogBindingsCoverage,
				&Test::RunApplicationBindingsCoverage,
				&Test::RunTestBindingsCoverage,
				&Test::RunBehaviourCallbacksCoverage
			};
			Test::ScriptTestFixture catalog;
			REQUIRE(catalog.Start());
			for (const auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				const std::string modeName = mode == RunModes::Editor ? "Editor" : mode == RunModes::Release ? "Release"
																											 : "Dist";
				CAPTURE(modeName);
				std::vector<ScriptApiCoverage> snapshots;
				for (const auto scenario : Scenarios)
					scenario(mode, snapshots);
				REQUIRE_FALSE(snapshots.empty());
				for (const auto& snapshot : snapshots)
					REQUIRE(snapshot.Mode == mode);
				const auto required = catalog.GetApi().GetCoverage(mode);
				REQUIRE(required);
				const auto missing = MissingApiCoverage(*required, snapshots);
				std::string report;
				for (const auto& member : missing)
					report += "\n" + modeName + " " + member;
				CHECK_MESSAGE(missing.empty(), "APIs have no executed test evidence:", report);
			}
		}

		TEST_CASE("ScriptApiRegistry: coverage acceptance rejects an unexecuted newly registered API")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true, .ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Acceptance coverage regression.")
					.Function("Executed", RegistryProbe, "() -> number", "An executed probe.", { .Mutates = false })
					.Function("Forgotten", RegistryProbe, "() -> number", "A newly added untested probe.", { .Mutates = false });
				return {};
			} });
			REQUIRE(fixture.Start());
			auto required = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(required);
			std::erase_if(required->Members, [](const auto& member)
			{
				return member.Owner != "Probe";
			});
			const auto empty = MissingApiCoverage(*required, {});
			CHECK(empty == std::vector<std::string>{ "Probe.Executed call", "Probe.Forgotten call" });
			CheckRegistryScript(fixture, "assert(Probe.Executed() == 7); return true");
			const auto observed = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(observed);
			CHECK(MissingApiCoverage(*required, { *observed }) == std::vector<std::string>{ "Probe.Forgotten call" });
			// A snapshot from a different mode cannot certify this mode.
			required->Mode = RunModes::Release;
			CHECK(MissingApiCoverage(*required, { *observed }) == empty);
		}

		TEST_CASE("ScriptApiRegistry: coverage targets match metadata and start with zero observations")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			for (auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				const auto coverage = fixture.GetApi().GetCoverage(mode);
				REQUIRE(coverage);
				std::set<std::pair<std::string, std::string>> expected;
				for (auto groups : { fixture.GetApi().GetModules(), fixture.GetApi().GetTypes() })
					for (const auto& group : groups)
						for (const auto& member : group.Members)
							if (member.Kind != ScriptApiMemberKind::Constant && HasFlag(member.Options.Modes, mode))
								expected.emplace(group.Name, member.Name);
				CHECK(expected.size() == coverage->Members.size());
				for (const auto& counter : coverage->Members)
				{
					CHECK(expected.erase({ counter.Owner, counter.Member }) == 1);
					CHECK(counter.Calls == 0);
					CHECK(counter.Reads == 0);
					CHECK(counter.Writes == 0);
				}
				CHECK(expected.empty());
			}
		}
		TEST_CASE("ScriptApiRegistry: one fluent registration supplies binding metadata and generated text")
		{
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			api.Module("Probe", "Probe module.").Function("Read", RegistryProbe, "() -> number", "Reads the probe.", { .Mutates = false });
			api.Type("ProbeValue", "Probe value.").Method("Read", RegistryProbe, "(self: ProbeValue) -> number", "Reads a value.", { .Mutates = false }).Property("Value", RegistryProbe, RegistryProbe, "number", "The value.").Operator("__eq", RegistryProbe, "(self: ProbeValue, other: ProbeValue) -> boolean", "Compares values.").Constructor("New", RegistryProbe, "() -> ProbeValue", "Creates a value.");
			REQUIRE(api.Freeze(types));
			CHECK(api.IsFrozen());
			REQUIRE(api.GetModules().size() == 1);
			REQUIRE(api.GetTypes().size() == 1);
			CHECK(api.GetTypes()[0].Members.size() == 4);
			const auto definitions = api.GenerateDefinitions();
			const auto documentation = api.GenerateDocumentation();
			REQUIRE(definitions);
			REQUIRE(documentation);
			CHECK(definitions->find("declare extern type ProbeValue") != std::string::npos);
			CHECK(documentation->find("Reads the probe.") != std::string::npos);
			CHECK(api.Freeze(types));
			TypeRegistry other;
			other.Freeze();
			CHECK_FALSE(api.Freeze(other));
			CHECK_FALSE(api.RegisterAlias("Late", "number", "Too late."));
		}
		TEST_CASE("ScriptApiRegistry: enum parameters share the authoritative table and retain zero counters")
		{
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			EnumInfo choice("ProbeChoice", "Probe choices.");
			choice.AddEntry({ "First", 3, "First choice." });
			choice.AddEntry({ "Second", 9, "Second choice." });
			REQUIRE(api.RegisterEnum(choice));
			api.Module("Probe", "Enum probe.").Function("Choose", RegistryEnumProbe, "(choice: ProbeChoice?) -> number", "Choose a value.", { .Mutates = false, .EnumParameters = { { 1, "ProbeChoice", true, "First" } } });
			REQUIRE(api.Freeze(types));
			const auto coverage = api.GetCoverage(RunModes::Dist);
			REQUIRE(coverage);
			REQUIRE(coverage->Members.size() == 1);
			REQUIRE(coverage->Members[0].EnumValues.size() == 2);
			CHECK(coverage->Members[0].EnumValues[0].ValueName == "First");
			CHECK(coverage->Members[0].EnumValues[1].ValueName == "Second");
			CHECK(coverage->Members[0].EnumValues[0].Count == 0);
			CHECK_FALSE(api.GetCoverage(RunModes::All));
		}
		TEST_CASE("ScriptApiRegistry: freeze rejects incomplete metadata and shortcut collisions")
		{
			for (int failure = 0; failure < 5; ++failure)
			{
				CAPTURE(failure);
				TypeRegistry types;
				types.Freeze();
				ScriptApiRegistry api;
				ScriptMemberOptions options{ .Mutates = false };
				if (failure == 4)
					options.EnumParameters.push_back({ 1, "Missing", false, {} });
				api.Module("Probe", "Probe.").Function("Read", failure == 3 ? nullptr : RegistryProbe, failure == 0 ? "number" : failure == 1 ? "() -> Missing"
																																			  : "() -> number",
					failure == 2 ? "" : "Reads a value.", options);
				CHECK_FALSE(api.Freeze(types));
				CHECK_FALSE(api.IsFrozen());
				CHECK_FALSE(api.GenerateDefinitions());
				CHECK_FALSE(api.GetCoverage(RunModes::Editor));
			}
			ENGINE_CHECK_DEATH("Scripting/DuplicateApiMember", "Duplicate script member");
			ENGINE_CHECK_DEATH("Scripting/ShortcutCollision", "Duplicate script member");
		}
		TEST_CASE("ScriptApiRegistry: signature validation traverses return packs and scopes generic names")
		{
			for (const char* signature : { "() -> (number, Missing)", "() -> {Value: Missing}", "(callback: () -> Missing) -> ()", "((<T>(value: T) -> T)) & ((value: T) -> T)", "() -> Missing..." })
			{
				CAPTURE(signature);
				TypeRegistry types;
				types.Freeze();
				ScriptApiRegistry api;
				api.Module("Probe", "Type validation probes.").Function("Read", RegistryProbe, signature, "Invalid nested or unbound type.", { .Mutates = false });
				const auto frozen = api.Freeze(types);
				CHECK_FALSE(frozen);
				CHECK_FALSE(api.IsFrozen());
			}
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			api.Module("Probe", "Type validation probes.")
				.Function("Generic", RegistryProbe, "<T>(value: T) -> {T}", "A scoped type parameter.", { .Mutates = false })
				.Function("Pack", RegistryProbe, "<T...>(callback: (T...) -> T...) -> T...", "A scoped variadic type parameter.", { .Mutates = false });
			REQUIRE(api.Freeze(types));
		}
		TEST_CASE("ScriptApiRegistry: registration hook sees builtins before freeze and propagates failure")
		{
			Test::SceneTestFixture fixture;
			for (const bool fail : { false, true })
			{
				CAPTURE(fail);
				ScriptApiRegistry api;
				bool invoked = false;
				const auto registered = RegisterBindings(api, fixture.GetRegistry(), [&invoked, fail](ScriptApiRegistry& configuring) -> Status
				{
					invoked = true;
					CHECK_FALSE(configuring.IsFrozen());
					CHECK(std::ranges::any_of(configuring.GetTypes(), [](const auto& type)
					{
						return type.Name == "Entity";
					}));
					CHECK(std::ranges::any_of(configuring.GetModules(), [](const auto& module)
					{
						return module.Name == "Field";
					}));
					configuring.Module("Probe", "A public extension.").Function("Read", RegistryProbe, "() -> number", "Reads a custom value.", { .Mutates = false });
					if (fail)
						return MakeError(ErrorCode::Validation, "configuration rejected");
					return {};
				});
				CHECK(invoked);
				CHECK(registered.has_value() == !fail);
				CHECK(api.IsFrozen() == !fail);
				if (fail)
				{
					REQUIRE_FALSE(registered);
					CHECK(registered.error().GetMessageText() == "configuration rejected");
				}
				else
				{
					const auto definitions = api.GenerateDefinitions();
					REQUIRE(definitions);
					CHECK(definitions->find("declare Probe:") != std::string::npos);
				}
			}
		}
		TEST_CASE("ScriptApiRegistry: generated declarations preserve generics overloads constants and reflected fields")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			const auto definitions = fixture.GetApi().GenerateDefinitions();
			REQUIRE(definitions);
			for (const char* text : { "<T>(name: string, class: T) -> T", "name: \"Transform\"", "GetScript: (self: Entity) -> any", "WorldPosition: vector", "Pi: number", "declare extern type Quat" })
				CHECK(definitions->find(text) != std::string::npos);
			for (const char* text : { "export type ScriptInstance =", "export type Key =", "export type FieldOptions =" })
				CHECK(definitions->find(text) != std::string::npos);
			auto checker = ScriptTypeChecker::Create(fixture.GetApi());
			if (!checker)
				INFO(checker.error().ToString());
			REQUIRE(checker);
			auto path = VfsPath::Create("project", "Assets/Types.luau");
			REQUIRE(path);
			const auto diagnostics = (*checker)->CheckScript({ .Path = *path, .Source = R"(
local key: Key = "Space"
local options: FieldOptions = {Min = 0, Max = 5}
local instance: ScriptInstance? = nil
local T = {Fields = {Count = Field.Integer(1, options)}, Value = 3}
local class = Script.Define("Typed", T)
local entity: Entity? = Scene.FindByName("Typed")
if entity then
	local transform: Transform? = entity:GetComponent("Transform")
	if transform then local p: vector = transform.WorldPosition end
	local body: RigidBody? = entity:GetComponent("RigidBody")
	if body then local velocity: vector = body:GetLinearVelocity() end
	local value: any = entity:GetScript()
end
local random = Random.New(19)
local numberChoice: number = random:Choice({1, 2, 3})
local stringChoice: string = random:Choice({"one", "two"})
local shuffledNumbers: {number} = random:Shuffle({1, 2, 3})
local shuffledStrings: {string} = random:Shuffle({"one", "two"})
local q: Quat = Quat.Identity()
local v: vector = q * vector.create(Math.Pi, 0, 0)
return class
)",
				.Modules = &fixture });
			for (const auto& diagnostic : diagnostics)
			{
				INFO(diagnostic.Message);
				CHECK(diagnostic.Severity != DiagnosticSeverity::Error);
			}
		}
		TEST_CASE("ScriptApiRegistry: binding availability is independent of run mode")
		{
			for (auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				Test::ScriptTestFixture fixture({ .Mode = mode });
				REQUIRE(fixture.Start());
				CheckRegistryScript(fixture, R"(local suite = Test.Suite("Pure", function() error("not executed") end); assert(suite.Name == "Pure"); local ok, message = pcall(Test.Expect, true); assert(not ok and string.find(message, "only available in test runs", 1, true)); return true)");
			}
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			api.Module("Probe", "Availability probe.").Function("Pure", RegistryProbe, "() -> number", "Pure probe.", { .Environments = ScriptApiEnvironment::All, .Mutates = false }).Function("Host", RegistryRejectedProbe, "() -> ()", "Host probe.", { .Mutates = false });
			REQUIRE(api.Freeze(types));
			Random random(17);
			for (auto mode : { SandboxMode::LoadTime, SandboxMode::Runtime })
			{
				auto sandbox = Sandbox::Create({ .Mode = mode, .Api = &api, .RandomStream = &random });
				REQUIRE(sandbox);
				auto result = (*sandbox)->Evaluate("assert(Probe.Pure() == 7); local ok, message = pcall(Probe.Host); assert(not ok); return message", {});
				REQUIRE(result);
				CHECK(result->Value.Get().dump().find(mode == SandboxMode::LoadTime ? "not available at load time" : "requires an active script engine") != std::string::npos);
			}
			const auto coverage = api.GetCoverage(RunModes::Editor);
			REQUIRE(coverage);
			for (const auto& member : coverage->Members)
				CHECK(member.Calls == 0);
		}
		TEST_CASE("ScriptApiRegistry: read-only dispatch rejects host mutations before invoking callbacks")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true, .ReadOnly = true, .ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Mutation sentinels.").Function("Write", RegistryRejectedProbe, "() -> ()", "Must be rejected before native entry.");
				return {};
			} });
			REQUIRE(fixture.Start());
			CheckRegistryScript(fixture, R"(
for _, fn in {Probe.Write, function() Scene.CreateEntity() end, function() Time.SetTimeScale(2) end, function() Random.Number() end, function() Task.Spawn(function() end) end, function() Input.SetCursorMode("Hidden") end} do assert(not pcall(fn)) end
local r = Random.New(12); local c = Color.New(0, 0, 0); c.r = r:Number(); assert(c.r >= 0 and c.r <= 1)
return true
)");
			CHECK(fixture.GetScene().GetEntityCount() == 0);
			CHECK(fixture.ExternalMutations.empty());
			const auto counters = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(counters);
			for (const auto& c : counters->Members)
				if (c.Owner == "Probe")
					CHECK(c.Calls == 0);
		}
		TEST_CASE("ScriptApiRegistry: property getters and setters have independent mutation policies")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true, .ReadOnly = true });
			static_cast<void>(fixture.GetScene().CreateEntity("Original"));
			REQUIRE(fixture.Start());
			CheckRegistryScript(fixture, R"(local e = Scene.FindByName("Original"); assert(e.Name == "Original"); assert(not pcall(function() e.Name = "Changed" end)); local c = Color.New(0,0,0); c.r = 2; assert(c.r == 2); local q = Quat.Identity(); q.x = 0.25; assert(q.x == 0.25); return true)");
			const auto coverage = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(coverage);
			for (const auto& c : coverage->Members)
			{
				if (c.Owner == "Entity" && c.Member == "Name")
				{
					CHECK(c.Reads == 1);
					CHECK(c.Writes == 0);
				}
				if (c.Owner == "Color" && c.Member == "r")
				{
					CHECK(c.Reads == 1);
					CHECK(c.Writes == 1);
				}
			}
		}
		TEST_CASE("ScriptApiRegistry: coverage keeps separate mode read write and enum counters")
		{
			for (auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
				REQUIRE(fixture.AddScript(AssetHandle(77), "Assets/Counter.luau", "local T = {}; function T:OnStart() self.Started = true end; return Script.Define('Counter', T)"));
				const auto owner = fixture.GetScene().CreateEntity("Counter");
				ScriptComponent script;
				script.Script.SetHandle(AssetHandle(77));
				owner.AddComponent<ScriptComponent>(script);
				REQUIRE(fixture.Start());
				CheckRegistryScript(fixture, R"(local e = Scene.FindByName("Counter"); assert(e:GetScript().Started); local t = e.Transform; t:Translate(vector.one); local p = t.Translation; t.Translation = p; return true)");
				const auto coverage = fixture.GetApi().GetCoverage(mode);
				REQUIRE(coverage);
				for (const auto& c : coverage->Members)
				{
					if (c.Owner == "Behaviour" && c.Member == "OnStart")
						CHECK(c.Calls == 1);
					if (c.Owner == "Transform" && c.Member == "Translation")
					{
						CHECK(c.Reads == 1);
						CHECK(c.Writes == 1);
					}
					if (c.Owner == "Transform" && c.Member == "Translate")
					{
						REQUIRE(c.EnumValues.size() == 2);
						CHECK(c.EnumValues[0].Count == 1);
						CHECK(c.EnumValues[1].Count == 0);
					}
				}
				for (auto other : { RunModes::Editor, RunModes::Release, RunModes::Dist })
					if (other != mode)
					{
						const auto empty = fixture.GetApi().GetCoverage(other);
						REQUIRE(empty);
						for (const auto& c : empty->Members)
						{
							CHECK(c.Calls == 0);
							CHECK(c.Reads == 0);
							CHECK(c.Writes == 0);
						}
					}
				fixture.GetApi().ResetCoverage();
				const auto reset = fixture.GetApi().GetCoverage(mode);
				REQUIRE(reset);
				for (const auto& c : reset->Members)
				{
					CHECK(c.Calls == 0);
					CHECK(c.Reads == 0);
					CHECK(c.Writes == 0);
					for (const auto& e : c.EnumValues)
						CHECK(e.Count == 0);
				}
			}
		}
		TEST_CASE("ScriptApiRegistry: enum coverage distinguishes parameters defaults and large tables")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true, .ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				EnumInfo small("ProbeChoice", "Choices.");
				small.AddEntry({ "First", 3, "First." });
				small.AddEntry({ "Second", 9, "Second." });
				ENGINE_TRY(api.RegisterEnum(small));
				EnumInfo large("LargeChoice", "Many choices.");
				for (int i = 0; i < 17; ++i)
					large.AddEntry({ "Value" + std::to_string(i), i, "A numbered value." });
				ENGINE_TRY(api.RegisterEnum(large));
				api.Module("Probe", "Enum probes.").Function("Choose", RegistryEnumProbe, "(first: ProbeChoice?, second: ProbeChoice?) -> number", "Validates two slots.", { .Mutates = false, .EnumParameters = { { 1, "ProbeChoice", true, "First" }, { 2, "ProbeChoice", true, "Second" } } }).Function("Large", RegistryProbe, "(value: LargeChoice) -> number", "Large enum probe.", { .Mutates = false, .EnumParameters = { { 1, "LargeChoice", false, {} } } });
				return {};
			} });
			REQUIRE(fixture.Start());
			CheckRegistryScript(fixture, R"(assert(Probe.Choose() == 3); assert(Probe.Choose("second", "FIRST") == 9); assert(not pcall(Probe.Choose, "First", "typo")); for i = 0,16 do assert(Probe.Large("Value" .. i) == 7) end; return true)");
			const auto coverage = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(coverage);
			for (const auto& c : coverage->Members)
				if (c.Owner == "Probe")
				{
					if (c.Member == "Choose")
					{
						CHECK(c.Calls == 2);
						REQUIRE(c.EnumValues.size() == 4);
						for (const auto& value : c.EnumValues)
							CHECK(value.Count == 1);
					}
					else
					{
						CHECK(c.Calls == 17);
						CHECK(c.EnumValues.empty());
					}
				}
		}
		TEST_CASE("ScriptApiRegistry: generated text is deterministic and uses mandatory descriptions")
		{
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry a, b;
			for (auto* api : { &a, &b })
			{
				for (int i = 0; i < 2; ++i)
				{
					const std::string name = (api == &a ? i : 1 - i) == 0 ? "Alpha" : "Zulu";
					api->Module(name, name + " module.").Function("Read", RegistryProbe, "() -> number", "A mandatory description.", { .Mutates = false });
				}
				REQUIRE(api->Freeze(types));
			}
			const auto aDefinitions = a.GenerateDefinitions();
			const auto bDefinitions = b.GenerateDefinitions();
			const auto aDocs = a.GenerateDocumentation();
			const auto bDocs = b.GenerateDocumentation();
			REQUIRE(aDefinitions);
			REQUIRE(bDefinitions);
			REQUIRE(aDocs);
			REQUIRE(bDocs);
			CHECK(*aDefinitions == *bDefinitions);
			CHECK(*aDocs == *bDocs);
			const auto docs = a.GenerateDocumentation();
			REQUIRE(docs);
			CHECK(docs->find("pure/read") != std::string::npos);
			CHECK(docs->find("Runtime|Test") != std::string::npos);
		}
		TEST_CASE("RegisterBindings: the built-in registry covers the complete scripting surface")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			for (const char* name : { "Script", "Field", "Scene", "Input", "Time", "Physics", "Audio", "Assets", "Task", "Random", "Math", "Debug", "Log", "Application", "Test" })
				CHECK(std::ranges::any_of(fixture.GetApi().GetModules(), [name](const auto& group)
				{
					return group.Name == name;
				}));
			for (const char* name : { "Entity", "Transform", "RigidBody", "CharacterController", "AudioSource", "Camera", "MeshRenderer", "Quat", "Color", "RandomGenerator", "TaskHandle", "AssetRef", "Behaviour" })
				CHECK(std::ranges::any_of(fixture.GetApi().GetTypes(), [name](const auto& group)
				{
					return group.Name == name;
				}));
			for (const auto& group : fixture.GetApi().GetTypes())
				if (group.Name == "Behaviour")
				{
					REQUIRE(group.Members.size() == 13);
					for (const auto& member : group.Members)
					{
						CHECK(member.Kind == ScriptApiMemberKind::Callback);
						CHECK(member.Options.Modes == (member.Name == "OnHotReload" ? RunModes::EditorOnly : RunModes::All));
					}
				}
		}
	}

}
