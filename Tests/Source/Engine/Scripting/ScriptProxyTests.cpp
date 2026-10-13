#include "TestsPCH.h"
#include "Engine/Scripting/ScriptProxy.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <format>
#include <type_traits>

namespace Engine {

	static_assert(std::is_trivially_copyable_v<ScriptEntityIdentity>);
	static_assert(std::is_trivially_copyable_v<ScriptProxyIdentity>);
	static_assert(std::is_same_v<decltype(ScriptEntityIdentity::ID), UUID>);
	static_assert(std::is_same_v<decltype(ScriptEntityIdentity::SceneGeneration), uint64_t>);
	static_assert(std::is_same_v<decltype(ScriptProxyIdentity::Entity), ScriptEntityIdentity>);
	static int PushPreviousGeneration(ScriptCall& call)
	{
		auto value = Lua::Check<ScriptEntityIdentity>(call, 1);
		--value.SceneGeneration;
		Lua::Push(call, value);
		return 1;
	}

	static void CheckProxyScript(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		if (!result)
			INFO(result.error().ToString());
		REQUIRE(result.has_value());
		CHECK(result->Value.Get() == true);
	}

	static Status AddCoverageComponent(Entity entity, const ComponentInfo& component, const IFieldSchemaSource* schemas)
	{
		if (component.GetHostOps()->Has(entity))
			return {};
		for (const auto* dependency : component.GetRequires())
			ENGINE_TRY(AddCoverageComponent(entity, *dependency, schemas));
		return ComponentAccess::AddComponent(entity, component.GetName(), nullptr, schemas);
	}

	namespace Test {

		// Walk reflection, not script-API metadata. Each operation crosses the real VM/proxy boundary and is checked
		// against ComponentAccess; a missing binding, mismatched field, conversion error or read-only leak fails here.
		void RunReflectedFieldCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start());
			for (const auto* component : fixture.GetTypes().GetComponents())
			{
				if (!component->HasFlag(ComponentFlags::ScriptVisible) || component->HasFlag(ComponentFlags::Hidden) || component->HasFlag(ComponentFlags::EntityLevel))
					continue;
				CAPTURE(component->GetName());
				const Entity entity = fixture.GetScene().CreateEntity(component->GetName());
				const auto added = AddCoverageComponent(entity, *component, fixture.GetFieldSchemas());
				if (!added)
					INFO(added.error().ToString());
				REQUIRE(added);
				const std::string prelude = std::format("local e = Scene.FindByID(\"{}\"); local c = e:GetComponent(\"{}\"); assert(c ~= nil); ", entity.GetUUID().ToString(), component->GetName());
				if (!component->HasFlag(ComponentFlags::NoShortcut))
					CheckProxyScript(fixture, prelude + std::format("assert(e[\"{}\"] ~= nil); return true", component->GetName()));
				for (const auto& descriptor : component->GetFields())
				{
					const auto& field = *descriptor;
					if (!field.GetMeta().Scriptable || field.GetMeta().Hidden || !HasFlag(field.GetMeta().Modes, mode))
						continue;
					CAPTURE(field.GetName());
					const auto before = ComponentAccess::GetFieldValue(entity, component->GetName(), field.GetName());
					REQUIRE(before);
					const std::string read = std::format("local value = c[\"{}\"]; ", field.GetName());
					const std::string write = std::format("c[\"{}\"] = value", field.GetName());
					CheckProxyScript(fixture, prelude + read + (field.IsReadOnly() ? "assert(not pcall(function() " + write + " end)); " : write + "; ") + "return true");
					const auto after = ComponentAccess::GetFieldValue(entity, component->GetName(), field.GetName());
					REQUIRE(after);
					CHECK(*after == *before);
				}
			}
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptProxy: every reflected script field round-trips through its actual component")
		{
			std::vector<ScriptApiCoverage> snapshots;
			for (auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
				Test::RunReflectedFieldCoverage(mode, snapshots);
		}
		TEST_CASE("ScriptProxy: stale generations never resolve in a replacement scene")
		{
			Test::ScriptTestFixture fixture;
			const auto entity = fixture.GetScene().CreateEntity("Saved");
			const ScriptEntityIdentity old{ entity.GetUUID(), fixture.Generation };
			REQUIRE(ScriptProxy::IsValid(fixture, old));
			const auto proxy = ScriptProxy::GetComponent(fixture, old, "Transform");
			REQUIRE(proxy);
			REQUIRE(proxy->has_value());
			++fixture.Generation;
			CHECK_FALSE(ScriptProxy::IsValid(fixture, old));
			const auto expired = ScriptProxy::ValidateEntity(fixture, old);
			REQUIRE_FALSE(expired);
			CHECK(expired.error().GetMessageText() == std::format("entity {} belongs to an expired scene", entity.GetUUID()));
			CHECK_FALSE(ScriptProxy::ValidateComponent(fixture, **proxy));
			CHECK(entity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			CHECK(ScriptProxy::IsValid(fixture, { entity.GetUUID(), fixture.Generation }));
		}
		TEST_CASE("ScriptProxy: dead and marked entities fail without assertions")
		{
			Test::ScriptTestFixture fixture;
			const auto entity = fixture.GetScene().CreateEntity("Doomed");
			CHECK_FALSE(ScriptProxy::IsValid(fixture, {}));
			const auto invalid = ScriptProxy::ValidateEntity(fixture, { entity.GetUUID(), 0 });
			REQUIRE_FALSE(invalid);
			CHECK(invalid.error().GetMessageText() == std::format("entity {} is not a valid entity identity", entity.GetUUID()));
			// §5.7: access to a destroyed entity names it as destroyed (not as possibly belonging to an expired scene).
			const auto gone = fixture.GetScene().CreateEntity("Gone");
			const ScriptEntityIdentity goneIdentity{ gone.GetUUID(), fixture.Generation };
			fixture.GetScene().DestroyEntity(gone);
			const auto destroyed = ScriptProxy::ValidateEntity(fixture, goneIdentity);
			REQUIRE_FALSE(destroyed);
			CHECK(destroyed.error().GetMessageText() == std::format("entity {} was destroyed", goneIdentity.ID));
			CHECK_FALSE(ScriptProxy::ValidateComponent(fixture, { { entity.GetUUID(), fixture.Generation }, 999999 }));
			REQUIRE(fixture.Start());
			CheckProxyScript(fixture, R"(
local e = Scene.FindByName("Doomed")
local proxy = e.Transform
local method = proxy.Forward
assert(not pcall(method, Color.New(1, 0, 0)))
e:Destroy()
assert(not e:IsValid())
assert(not pcall(function() return e.Name end))
assert(not pcall(function() return proxy.Translation end))
return true
)");
		}
		TEST_CASE("ScriptProxy: each access re-resolves the component after structural changes")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			CheckProxyScript(fixture, R"(
local e = Scene.CreateEntity("Owner")
e:AddComponent("MeshRenderer")
local old = e.MeshRenderer
old.Visible = false
for i = 1, 128 do Scene.CreateEntity():AddComponent("MeshRenderer") end
assert(old.Visible == false)
e:RemoveComponent("MeshRenderer")
assert(not pcall(function() return old.Visible end))
e:AddComponent("MeshRenderer")
assert(old.Visible == true)
old.Visible = false
assert(e.MeshRenderer.Visible == false)
return true
)");
		}
		TEST_CASE("ScriptProxy: shortcut eligibility follows reflected flags and entity precedence")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			CheckProxyScript(fixture, R"(
local e = Scene.CreateEntity("Named")
assert(e.Name == "Named" and type(e.ID) == "string")
assert(e.Transform ~= nil and e.RigidBody == nil)
assert(e:GetComponent("Script") == nil and e:GetScript() == nil)
assert(not pcall(function() return e.Script end))
assert(not pcall(function() return e.Relationship end))
assert(not pcall(function() return e:GetComponent("PrefabLink") end))
assert(not pcall(function() return e:GetComponent("Transfrm") end))
return true
)");
		}
		TEST_CASE("ScriptProxy: reflected writes validate atomically and notify systems")
		{
			Test::ScriptTestFixture fixture;
			const auto entity = fixture.GetScene().CreateEntity("Root");
			REQUIRE(fixture.Start());
			const auto revision = fixture.GetScene().GetRevision();
			CheckProxyScript(fixture, R"(
local e = Scene.FindByName("Root")
assert(not pcall(function() e.Transform.Scale = vector.create(1, 0, 1) end))
assert(e.Transform.Scale == vector.create(1, 1, 1))
return true
)");
			CHECK(fixture.GetScene().GetRevision() == revision);
			CHECK(fixture.ExternalMutations.empty());
			CheckProxyScript(fixture, R"(
local e = Scene.FindByName("Root")
e.Transform.Translation = vector.create(2, 3, 4)
e:AddComponent("Camera")
local c = e.Camera
local old = c.NearClip
assert(not pcall(function() c.NearClip = c.FarClip + 1 end))
assert(c.NearClip == old)
return true
)");
			CHECK(entity.GetComponent<TransformComponent>().Translation == glm::vec3(2, 3, 4));
			CHECK(fixture.GetScene().GetRevision() > revision);
			CHECK(fixture.ExternalMutations.size() == 1);
		}
		TEST_CASE("ScriptProxy: field visibility and run modes govern reads writes and coverage")
		{
			for (const auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
				REQUIRE(fixture.Start());
				CheckProxyScript(fixture, R"(
local e = Scene.CreateEntity()
local t = e.Transform
local old = t.Translation
t.Translation = old + vector.create(1, 0, 0)
assert(not pcall(function() t.WorldScale = vector.create(2, 2, 2) end))
return true
)");
				const auto coverage = fixture.GetApi().GetCoverage(mode);
				REQUIRE(coverage);
				bool found = false;
				for (const auto& counter : coverage->Members)
					if (counter.Owner == "Transform" && counter.Member == "Translation")
					{
						found = true;
						CHECK(counter.Reads == 1);
						CHECK(counter.Writes == 1);
						CHECK(counter.RequiresRead);
						CHECK(counter.RequiresWrite);
					}
				CHECK(found);
				for (const auto& group : fixture.GetApi().GetTypes())
					for (const auto& member : group.Members)
						if (member.Kind == ScriptApiMemberKind::ProxyField)
						{
							const auto* info = fixture.GetTypes().GetComponents()[*member.ComponentTypeIndex];
							const auto* field = info->FindField(member.Name);
							REQUIRE(field);
							CHECK(field->GetMeta().Scriptable);
							CHECK_FALSE(field->GetMeta().Hidden);
							CHECK(member.Writable == !field->IsReadOnly());
						}
			}
		}
		TEST_CASE("ScriptProxy: read-only evaluation rejects proxy writes before mutation")
		{
			Test::ScriptTestFixture fixture({ .TestMode = true, .ReadOnly = true });
			const auto entity = fixture.GetScene().CreateEntity("Root");
			REQUIRE(fixture.Start());
			const auto revision = fixture.GetScene().GetRevision();
			CheckProxyScript(fixture, R"(
local t = Scene.FindByName("Root").Transform
assert(t.Translation == vector.zero)
local alias = t
assert(not pcall(function() alias.Translation = vector.one end))
assert(not pcall(function() alias:Translate(vector.one) end))
return true
)");
			CHECK(entity.GetComponent<TransformComponent>().Translation == glm::vec3(0));
			CHECK(fixture.GetScene().GetRevision() == revision);
			CHECK(fixture.ExternalMutations.empty());
			const auto coverage = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(coverage);
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Transform")
					CHECK(counter.Writes == 0);
		}
		TEST_CASE("ScriptProxy: entity equality compares UUID without granting access")
		{
			Test::ScriptTestFixture fixture({ .ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Identity regression probe.").Function("PreviousGeneration", PushPreviousGeneration, "(entity: Entity) -> Entity", "Copy the identity with the previous generation.", { .Mutates = false });
				return {};
			} });
			fixture.Generation = 2;
			REQUIRE(fixture.AddScript(AssetHandle(86), "Assets/Target.luau", "return Script.Define(\"Target\", {Fields = {Target = Field.Entity()}})"));
			const auto entity = fixture.GetScene().CreateEntity("Same");
			ScriptComponent script;
			script.Script.SetHandle(AssetHandle(86));
			entity.AddComponent<ScriptComponent>(script);
			REQUIRE(fixture.Start());
			CheckProxyScript(fixture, R"(
local current = Scene.FindByName("Same")
local old = Probe.PreviousGeneration(current)
assert(old == current and not old:IsValid() and current:IsValid())
assert(not pcall(function() old.Name = "changed" end))
assert(not pcall(function() current:GetComponent("Script").Fields = {Target = old} end))
assert(current:GetComponent("Script").Fields.Target == nil)
assert(current.Name == "Same")
return true
)");
		}
		TEST_CASE("ScriptProxy: unrepresentable world poses fail before mutation or invalidation")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			CheckProxyScript(fixture, R"(
local parent = Scene.CreateEntity("Parent")
parent.Transform.Scale = vector.create(0.0001,0.0001,0.0001)
Scene.CreateEntity("Child", parent)
return true
)");
			fixture.ExternalMutations.clear();
			const auto revision = fixture.GetScene().GetRevision();
			CheckProxyScript(fixture, R"(
local transform = Scene.FindByName("Child").Transform
local huge = vector.create(3e38,0,0)
assert(not pcall(function() transform.WorldPosition = huge end))
assert(not pcall(function() transform:Teleport(huge, Quat.Identity()) end))
assert(transform.Translation == vector.zero)
return true
)");
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Teleported.empty());
			CHECK(fixture.GetScene().GetRevision() == revision);
		}
		TEST_CASE("ScriptProxy: Script assignments reject Module and TestSuite before structural changes")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.AddScript(AssetHandle(91), "Assets/Module.luau", "return {}"));
			REQUIRE(fixture.AddScript(AssetHandle(92), "Assets/Suite.luau", "return Test.Suite(\"Suite\", function() end)"));
			REQUIRE(fixture.AddScript(AssetHandle(93), "Assets/Behaviour.luau", "return Script.Define(\"Behaviour\", {})"));
			const Entity root = fixture.GetScene().CreateEntity("Root");
			REQUIRE(fixture.Start());
			const auto revision = fixture.GetScene().GetRevision();
			CheckProxyScript(fixture, R"(
local entity = Scene.FindByName("Root")
for _, path in {"Assets/Module.luau", "Assets/Suite.luau"} do
	assert(not pcall(function() entity:AddComponent("Script", {Script = Assets.Load(path)}) end))
	assert(not entity:HasComponent("Script"))
end
return true
)");
			CHECK(fixture.GetScene().GetRevision() == revision);
			CHECK(fixture.ExternalMutations.empty());
			for (const char* path : { "Assets/Module.luau", "Assets/Suite.luau" })
			{
				Test::ExpectLog expected(LogLevel::Error, "a Script component requires a Behaviour asset");
				const auto rejected = fixture.Evaluate(std::format("local entity = Scene.FindByName(\"Root\")\nentity:AddComponent(\"Script\", {{Script = Assets.Load(\"{}\")}})", path));
				CHECK_FALSE(rejected);
				CHECK(expected.GetMatchCount() == 1);
				REQUIRE_FALSE(fixture.Errors.empty());
				CHECK(fixture.Errors.back().Line == 2);
				CHECK_FALSE(fixture.Errors.back().Script.empty());
				CHECK(fixture.Errors.back().Message.find("Behaviour") != std::string::npos);
				CHECK(fixture.GetScene().GetRevision() == revision);
				CHECK(fixture.ExternalMutations.empty());
			}
			CheckProxyScript(fixture, R"(
local entity = Scene.FindByName("Root")
local behaviour = Assets.Load("Assets/Behaviour.luau")
local script = entity:AddComponent("Script", {Script = behaviour})
assert(entity:GetScript() ~= nil)
assert(Assets.GetPath(script.Script) == Assets.GetPath(behaviour))
return true
)");
			fixture.ExternalMutations.clear();
			const auto attachedRevision = fixture.GetScene().GetRevision();
			for (const char* path : { "Assets/Module.luau", "Assets/Suite.luau" })
			{
				Test::ExpectLog expected(LogLevel::Error, "a Script component requires a Behaviour asset");
				const auto rejected = fixture.Evaluate(std::format("local script = Scene.FindByName(\"Root\"):GetComponent(\"Script\")\nscript.Script = Assets.Load(\"{}\")", path));
				CHECK_FALSE(rejected);
				CHECK(expected.GetMatchCount() == 1);
				REQUIRE_FALSE(fixture.Errors.empty());
				CHECK(fixture.Errors.back().Line == 2);
				CHECK_FALSE(fixture.Errors.back().Script.empty());
				CHECK(fixture.Errors.back().Message.find("Behaviour") != std::string::npos);
				CHECK(fixture.GetScene().GetRevision() == attachedRevision);
				CHECK(fixture.ExternalMutations.empty());
				CHECK(root.GetComponent<ScriptComponent>().Script.GetHandle() == AssetHandle(93));
			}
		}
	}

}
