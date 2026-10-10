#include "TestsPCH.h"
#include "Engine/Scripting/ScriptProxy.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
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

	static void TransformCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
		REQUIRE(fixture.Start());
		CheckProxyScript(fixture, R"(
local parent = Scene.CreateEntity("Parent")
parent.Transform.Translation = vector.create(10, 0, 0)
local child = Scene.CreateEntity("Child", parent)
local t = child.Transform
t.Translation = vector.create(2, 0, 0)
assert(t.WorldPosition == vector.create(12, 0, 0))
t.WorldPosition = vector.create(15, 0, 0)
assert(t.Translation == vector.create(5, 0, 0))
assert(t:TransformPoint(vector.zero) == t.WorldPosition)
assert(t:InverseTransformPoint(t.WorldPosition) == vector.zero)
assert(t:Forward() == vector.create(0, 0, -1))
assert(t:Right() == vector.create(1, 0, 0))
assert(t:Up() == vector.create(0, 1, 0))
t:Rotate(vector.create(0, 90, 0), "World")
assert(vector.magnitude(t:Forward() - vector.create(-1, 0, 0)) < 0.00001)
t:Translate(vector.create(0, 0, -2), "local")
t:Translate(vector.create(1, 0, 0), "World")
local world = t.WorldPosition
t:Translate(vector.zero)
assert(t.WorldPosition == world)
t:Rotate(vector.zero, "Local")
t:Rotate(vector.zero)
t:LookAt(t.WorldPosition + vector.create(0, 0, -1))
assert(vector.magnitude(t:TransformDirection(vector.create(0, 0, -1)) - t:Forward()) < 0.00001)
assert(not pcall(function() t:LookAt(t.WorldPosition) end))
t:Teleport(vector.create(30, 1, 2), Quat.Identity())
assert(t.WorldPosition == vector.create(30, 1, 2))
assert(t.RenderPosition == t.WorldPosition)
assert(not pcall(function() t.WorldScale = vector.one end))
return true
)");
		REQUIRE(fixture.Teleported.size() == 1);
		CHECK(fixture.Teleported[0] == fixture.GetScene().FindEntityByID(fixture.Teleported[0]).GetUUID());
		const auto coverage = fixture.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		snapshots.push_back(*coverage);
	}

	static void MaterialCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
		const auto entity = fixture.GetScene().CreateEntity("Mesh");
		MeshRendererComponent renderer;
		renderer.Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
		entity.AddComponent<MeshRendererComponent>(renderer);
		REQUIRE(fixture.Start());
		CheckProxyScript(fixture, R"(
local mesh = Scene.FindByName("Mesh").MeshRenderer
local default = Assets.Load("engine://Materials/Default")
local replacement = Assets.Load("engine://Materials/Error")
assert(Assets.GetPath(mesh:GetMaterial(1)) == Assets.GetPath(default))
mesh:SetMaterial(1, replacement)
assert(Assets.GetPath(mesh:GetMaterial(1)) == Assets.GetPath(replacement))
mesh:SetMaterial(1, nil)
assert(Assets.GetPath(mesh:GetMaterial(1)) == Assets.GetPath(default))
for _, index in {0, -1, 1.5, 2, math.huge} do
assert(not pcall(function() mesh:GetMaterial(index) end))
assert(not pcall(function() mesh:SetMaterial(index, replacement) end))
end
return true
)");
		REQUIRE(entity.GetComponent<MeshRendererComponent>().Materials.size() == 1);
		CHECK_FALSE(entity.GetComponent<MeshRendererComponent>().Materials[0].GetHandle().IsValid());
		const auto coverage = fixture.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		snapshots.push_back(*coverage);
	}

	namespace Test {

		void RunEntityBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			fixture.Environment.WindowSize = { 100, 100 };
			REQUIRE(fixture.AddScript(AssetHandle(200), "Assets/Coverage.luau", R"(
local T = {Fields = {
	Number = Field.Number(1), Integer = Field.Integer(2), Bool = Field.Bool(true),
	String = Field.String("value"), Vector = Field.Vector(vector.create(1,2,3)),
	Tint = Field.Color(Color.New(1,0,0)), Rotation = Field.Quat(Quat.Identity()),
	Target = Field.Entity(), Clip = Field.Asset("AudioClip"), Mode = Field.Enum({"First","Second"}),
	Array = Field.Array(Field.Integer())
}}
function T:OnCreate()
	assert(self.Number == 1 and self.Integer == 2 and self.Bool and self.String == "value")
	assert(self.Vector == vector.create(1,2,3) and self.Tint.r == 1 and self.Rotation.w == 1)
	assert(self.Target == nil and self.Clip == nil and self.Mode == "First" and #self.Array == 0)
	self.Ready = true
end
return Script.Define("Coverage", T)
)"));
			const Entity bound = fixture.GetScene().CreateEntity("Bound");
			ScriptComponent script;
			script.Script.SetHandle(AssetHandle(200));
			bound.AddComponent<ScriptComponent>(script);
			REQUIRE(fixture.Start());
			CheckProxyScript(fixture, R"(
local bound = Scene.FindByName("Bound")
assert(bound:GetScript().Ready)
local suite = Test.Suite("Ordinary", function() error("must not execute") end, {CaseTimeoutTicks=5})
assert(suite.Name == "Ordinary" and suite.Options.CaseTimeoutTicks == 5)
local parent = Scene.CreateEntity("Parent")
local child = Scene.CreateEntity("Child", parent)
assert(parent:IsValid() and child:IsActive() and child:IsActiveSelf())
assert(#parent.ID == 16 and child:GetParent() == parent)
child.Name = "Renamed"
assert(child.Name == "Renamed")
child:AddTag("First"); child:AddTag("Second"); child:AddTag("First")
assert(child:HasTag("First") and #child:GetTags() == 2)
assert(child:GetTags()[1] == "First" and child:GetTags()[2] == "Second")
child:RemoveTag("First")
assert(not child:HasTag("First") and child:GetTags()[1] == "Second")
parent:SetActive(false)
assert(not child:IsActive() and child:IsActiveSelf())
parent:SetActive(true)
assert(child:IsActive())
assert(#parent:GetChildren() == 1 and parent:GetChildren()[1] == child)
local grandchild = Scene.CreateEntity("Grandchild", child)
assert(parent:FindChild("Grandchild") == nil and parent:FindChild("Grandchild", true) == grandchild)
assert(parent:FindChild("Renamed") == child)
child.Transform.Translation = vector.create(2,0,0)
child:SetParent(nil)
assert(child:GetParent() == nil and child.Transform.WorldPosition == vector.create(2,0,0))
child:SetParent(parent, false)
assert(child:GetParent() == parent and child.Transform.Translation == vector.create(2,0,0))
assert(not child:HasComponent("MeshRenderer") and child:GetComponent("MeshRenderer") == nil)
local mesh = child:AddComponent("MeshRenderer", {Visible=false})
assert(child:HasComponent("MeshRenderer") and not mesh.Visible)
local minimum, maximum = child:GetWorldBounds()
assert(minimum == nil and maximum == nil)
mesh.Mesh = Assets.Load("engine://Meshes/Cube")
minimum, maximum = child:GetWorldBounds()
assert(minimum ~= nil and maximum ~= nil and maximum.x > minimum.x)
child:RemoveComponent("MeshRenderer")
assert(not child:HasComponent("MeshRenderer"))
local camera = parent:AddComponent("Camera")
local origin, direction = camera:ScreenToWorldRay(49.5,49.5)
assert(origin == vector.zero and vector.magnitude(direction - vector.create(0,0,-1)) < 0.00001)
local screen = camera:WorldToScreen(vector.create(0,0,-5))
assert(math.abs(screen.x-49.5) < 0.00001 and math.abs(screen.y-49.5) < 0.00001)
assert(math.abs(screen.z-camera.NearClip/5) < 0.00001)
camera.Projection = "Orthographic"
origin, direction = camera:ScreenToWorldRay(49.5,49.5)
assert(origin == vector.create(0,0,-camera.NearClip) and direction == vector.create(0,0,-1))
screen = camera:WorldToScreen(vector.create(0,0,-5))
assert(math.abs(screen.z - (camera.FarClip-5)/(camera.FarClip-camera.NearClip)) < 0.00001)
child:Destroy()
assert(not child:IsValid() and not grandchild:IsValid())
return true
)");
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
			TransformCoverage(mode, snapshots);
			MaterialCoverage(mode, snapshots);
		}

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
		TEST_CASE("EntityBindings: hierarchy tags components fields and camera results agree with scene state")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunEntityBindingsCoverage(RunModes::Editor, snapshots);
		}
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
			CHECK_FALSE(ScriptProxy::ValidateEntity(fixture, old));
			CHECK_FALSE(ScriptProxy::ValidateComponent(fixture, **proxy));
			CHECK(entity.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			CHECK(ScriptProxy::IsValid(fixture, { entity.GetUUID(), fixture.Generation }));
		}
		TEST_CASE("ScriptProxy: dead and marked entities fail without assertions")
		{
			Test::ScriptTestFixture fixture;
			const auto entity = fixture.GetScene().CreateEntity("Doomed");
			CHECK_FALSE(ScriptProxy::IsValid(fixture, {}));
			CHECK_FALSE(ScriptProxy::ValidateEntity(fixture, { entity.GetUUID(), 0 }));
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
		TEST_CASE("ScriptProxy: Transform fast paths preserve reflection and interpolation semantics")
		{
			std::vector<ScriptApiCoverage> snapshots;
			TransformCoverage(RunModes::Editor, snapshots);
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
		TEST_CASE("ScriptProxy: material slots resolve mesh defaults and expand overrides")
		{
			std::vector<ScriptApiCoverage> snapshots;
			MaterialCoverage(RunModes::Editor, snapshots);
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
