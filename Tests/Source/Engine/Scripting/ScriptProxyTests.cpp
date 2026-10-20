#include "TestsPCH.h"
#include "Engine/Scripting/ScriptProxy.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/CameraComponent.h"
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

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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
	// Probe.Find: the identity of `id` in scene generation `generation`, whatever the entity's state, so a test reaches an
	// entity pending destruction or already destroyed, which the scene's searches leave out.
	static int PushIdentityOf(ScriptCall& call)
	{
		const auto id = UUID::FromString(Lua::Check<std::string>(call, 1));
		if (!id)
			return Lua::RaiseError(call, "id must contain exactly 16 hexadecimal digits");
		Lua::Push(call, ScriptEntityIdentity{ *id, Lua::Check<uint32_t>(call, 2) });
		return 1;
	}
	static Status RegisterIdentityProbe(ScriptApiRegistry& api)
	{
		api.Module("Probe", "Identity regression probe.").Function("Find", PushIdentityOf, "(id: string, generation: number) -> Entity", "An identity of the generation, live or not.", { .Mutates = false });
		return {};
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
			Test::ScriptTestFixture fixture({ .ConfigureApi = RegisterIdentityProbe });
			const auto entity = fixture.GetScene().CreateEntity("Doomed");
			const UUID entityID = entity.GetUUID();
			CHECK_FALSE(ScriptProxy::IsValid(fixture, {}));
			const auto invalid = ScriptProxy::ValidateEntity(fixture, { entity.GetUUID(), 0 });
			REQUIRE_FALSE(invalid);
			CHECK(invalid.error().GetMessageText() == std::format("entity {} is not a valid entity identity", entity.GetUUID()));
			CHECK_FALSE(ScriptProxy::ValidateEntityForRead(fixture, { entity.GetUUID(), 0 }));
			// §5.7: access to a destroyed entity names it as destroyed (not as possibly belonging to an expired scene). A marked
			// entity is still readable until the flush (M20, W-P02), never writable.
			const auto gone = fixture.GetScene().CreateEntity("Gone");
			const ScriptEntityIdentity goneIdentity{ gone.GetUUID(), fixture.Generation };
			fixture.GetScene().DestroyEntity(gone);
			const auto destroyed = ScriptProxy::ValidateEntity(fixture, goneIdentity);
			REQUIRE_FALSE(destroyed);
			CHECK(destroyed.error().GetMessageText() == std::format("entity {} was destroyed", goneIdentity.ID));
			CHECK(ScriptProxy::ValidateEntityForRead(fixture, goneIdentity));
			CHECK_FALSE(ScriptProxy::IsValid(fixture, goneIdentity));
			fixture.GetScene().FlushPendingDestroys();
			const auto flushed = ScriptProxy::ValidateEntityForRead(fixture, goneIdentity);
			REQUIRE_FALSE(flushed);
			CHECK(flushed.error().GetMessageText() == std::format("entity {} was destroyed", goneIdentity.ID));
			CHECK_FALSE(ScriptProxy::ValidateComponent(fixture, { { entity.GetUUID(), fixture.Generation }, 999999 }));
			CHECK_FALSE(ScriptProxy::ValidateComponentForRead(fixture, { { entity.GetUUID(), fixture.Generation }, 999999 }));
			REQUIRE(fixture.Start());
			CheckProxyScript(fixture, R"(
local e = Scene.FindByName("Doomed")
local proxy = e.Transform
local method = proxy.Forward
assert(not pcall(method, Color.New(1, 0, 0)))
e:Destroy()
assert(not e:IsValid())
assert(e.Name == "Doomed" and proxy.Translation == vector.zero)
assert(not pcall(function() e.Name = "Renamed" end))
assert(not pcall(function() proxy.Translation = vector.one end))
return true
)");
			fixture.GetScene().FlushPendingDestroys();
			CheckProxyScript(fixture, std::format(R"(
local e = Probe.Find("{}", 1)
assert(not e:IsValid())
assert(not pcall(function() return e.Name end))
assert(not pcall(function() return e.Transform end))
assert(not pcall(function() return e:GetChildren() end))
return true
)",
										  entityID.ToString()));
		}
		// Docs/Decisions/0031-m20-contract.md decision 4 and its risk 3 (the audit of every member registered as not mutating
		// against the mutation check): each such member of Entity and of the component proxies, every shortcut and every
		// proxy field read accepts an entity pending destruction, nested ones included, without a host mutation; the reads
		// of a system's state (a body, a character, a voice) keep refusing it; every mutating member and every proxy field
		// write refuses it before any mutation; and once flushed, every read but the identity's own refuses it. A member
		// added later fails here until it is classified.
		TEST_CASE("ScriptProxy: every read member reads an entity pending destruction and every write refuses it")
		{
			struct MemberCheck
			{
				std::string_view Member;
				std::string_view Lua;
			};
			// Reads of the pending entity Doomed (at world (1, 2, 0) under the live Parent, with a Camera, a cube MeshRenderer,
			// a Script instance and a tag) and of its pending Child (world (1, 2, 3)). The identity reads work on any
			// identity, so they keep working after the flush.
			const std::vector<MemberCheck> reads = {
				{ "Entity.ID", "assert(#d.ID == 16)" },
				{ "Entity.IsValid", "assert(not d:IsValid())" },
				{ "Entity.__eq", "assert(d == Probe.Find(d.ID, 1))" },
				{ "Entity.Name", "assert(d.Name == \"Doomed\")" },
				{ "Entity.IsActive", "assert(d:IsActive())" },
				{ "Entity.IsActiveSelf", "assert(d:IsActiveSelf())" },
				{ "Entity.HasTag", "assert(d:HasTag(\"Tagged\"))" },
				{ "Entity.GetTags", "assert(d:GetTags()[1] == \"Tagged\")" },
				{ "Entity.HasComponent", "assert(d:HasComponent(\"Camera\"))" },
				{ "Entity.GetComponent", "assert(d:GetComponent(\"Camera\") ~= nil)" },
				{ "Entity.GetScript", "assert(d:GetScript() ~= nil)" },
				{ "Entity.GetParent", "assert(d:GetParent() == Scene.FindByName(\"Parent\") and child:GetParent() == d)" },
				{ "Entity.GetChildren", "assert(d:GetChildren()[1] == child)" },
				{ "Entity.FindChild", "assert(d:FindChild(\"Child\") == child)" },
				{ "Entity.GetWorldBounds", "local low, high = d:GetWorldBounds(); assert(vector.magnitude(low - vector.create(0.5, 1.5, -0.5)) < 0.0001 and vector.magnitude(high - vector.create(1.5, 2.5, 0.5)) < 0.0001)" },
				{ "Transform.Forward", "assert(d.Transform:Forward() == vector.create(0, 0, -1))" },
				{ "Transform.Right", "assert(d.Transform:Right() == vector.create(1, 0, 0))" },
				{ "Transform.Up", "assert(d.Transform:Up() == vector.create(0, 1, 0))" },
				{ "Transform.TransformPoint", "assert(child.Transform:TransformPoint(vector.zero) == vector.create(1, 2, 3))" },
				{ "Transform.InverseTransformPoint", "assert(child.Transform:InverseTransformPoint(vector.create(1, 2, 3)) == vector.zero)" },
				{ "Transform.TransformDirection", "assert(child.Transform:TransformDirection(vector.create(0, 0, 1)) == vector.create(0, 0, 1))" },
				{ "Camera.ScreenToWorldRay", "local origin = d.Camera:ScreenToWorldRay(49.5, 49.5); assert(origin == vector.create(1, 2, 0))" },
				{ "Camera.WorldToScreen", "assert(d.Camera:WorldToScreen(vector.create(1, 2, -5)).z > 0)" },
				{ "MeshRenderer.GetMaterial", "assert(d.MeshRenderer:GetMaterial(1) ~= nil)" },
			};
			const std::set<std::string_view> identityReads = { "Entity.ID", "Entity.IsValid", "Entity.__eq" };
			// Reads of a system's state of the entity, which is leaving with it (decision 4, "Unchanged").
			const std::vector<MemberCheck> systemReads = {
				{ "RigidBody.GetLinearVelocity", "holders.RigidBody.RigidBody:GetLinearVelocity()" },
				{ "RigidBody.GetAngularVelocity", "holders.RigidBody.RigidBody:GetAngularVelocity()" },
				{ "RigidBody.IsSleeping", "holders.RigidBody.RigidBody:IsSleeping()" },
				{ "CharacterController.IsGrounded", "holders.CharacterController.CharacterController:IsGrounded()" },
				{ "CharacterController.GetGroundNormal", "holders.CharacterController.CharacterController:GetGroundNormal()" },
				{ "CharacterController.GetVelocity", "holders.CharacterController.CharacterController:GetVelocity()" },
				{ "AudioSource.IsPlaying", "holders.AudioSource.AudioSource:IsPlaying()" },
			};
			// Mutating members, and the setters of the properties that have one.
			const std::vector<MemberCheck> writes = {
				{ "Entity.Name", "d.Name = \"Renamed\"" },
				{ "Entity.SetActive", "d:SetActive(false)" },
				{ "Entity.AddTag", "d:AddTag(\"More\")" },
				{ "Entity.RemoveTag", "d:RemoveTag(\"Tagged\")" },
				{ "Entity.AddComponent", "d:AddComponent(\"RigidBody\")" },
				{ "Entity.RemoveComponent", "d:RemoveComponent(\"Camera\")" },
				{ "Entity.SetParent", "child:SetParent(nil)" },
				{ "Entity.Destroy", "child:Destroy()" },
				{ "Transform.Teleport", "d.Transform:Teleport(vector.zero)" },
				{ "Transform.LookAt", "d.Transform:LookAt(vector.create(0, 0, -10))" },
				{ "Transform.Translate", "d.Transform:Translate(vector.one)" },
				{ "Transform.Rotate", "d.Transform:Rotate(vector.one)" },
				{ "RigidBody.AddForce", "holders.RigidBody.RigidBody:AddForce(vector.one)" },
				{ "RigidBody.AddForceAtPosition", "holders.RigidBody.RigidBody:AddForceAtPosition(vector.one, vector.zero)" },
				{ "RigidBody.AddTorque", "holders.RigidBody.RigidBody:AddTorque(vector.one)" },
				{ "RigidBody.AddImpulse", "holders.RigidBody.RigidBody:AddImpulse(vector.one)" },
				{ "RigidBody.AddAngularImpulse", "holders.RigidBody.RigidBody:AddAngularImpulse(vector.one)" },
				{ "RigidBody.SetLinearVelocity", "holders.RigidBody.RigidBody:SetLinearVelocity(vector.one)" },
				{ "RigidBody.SetAngularVelocity", "holders.RigidBody.RigidBody:SetAngularVelocity(vector.one)" },
				{ "RigidBody.MoveKinematic", "holders.RigidBody.RigidBody:MoveKinematic(vector.zero, Quat.Identity())" },
				{ "RigidBody.Teleport", "holders.RigidBody.RigidBody:Teleport(vector.zero)" },
				{ "RigidBody.WakeUp", "holders.RigidBody.RigidBody:WakeUp()" },
				{ "CharacterController.Move", "holders.CharacterController.CharacterController:Move(vector.one)" },
				{ "AudioSource.Play", "holders.AudioSource.AudioSource:Play()" },
				{ "AudioSource.Stop", "holders.AudioSource.AudioSource:Stop()" },
				{ "AudioSource.Pause", "holders.AudioSource.AudioSource:Pause()" },
				{ "AudioSource.Resume", "holders.AudioSource.AudioSource:Resume()" },
				{ "MeshRenderer.SetMaterial", "d.MeshRenderer:SetMaterial(1, nil)" },
			};

			Test::ScriptTestFixture fixture({ .ConfigureApi = RegisterIdentityProbe });
			fixture.Environment.WindowSize = { 100, 100 };
			REQUIRE(fixture.AddScript(AssetHandle(94), "Assets/Kept.luau", "return Script.Define(\"Kept\", {})"));
			Scene& scene = fixture.GetScene();
			const TypeRegistry& types = fixture.GetTypes();

			const Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 0.0f, 0.0f);
			});
			const Entity doomed = scene.CreateEntity("Doomed", parent);
			doomed.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 2.0f, 0.0f);
			});
			doomed.AddTag("Tagged");
			doomed.AddComponent<CameraComponent>();
			MeshRendererComponent renderer;
			renderer.Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			doomed.AddComponent<MeshRendererComponent>(renderer);
			ScriptComponent script;
			script.Script.SetHandle(AssetHandle(94));
			doomed.AddComponent<ScriptComponent>(script);
			const Entity child = scene.CreateEntity("Child", doomed);
			child.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 0.0f, 3.0f);
			});
			// One pending holder of each component exposed to scripts, for its proxy fields, its shortcut and its methods.
			std::string prelude = std::format("local d = Probe.Find(\"{}\", 1)\nlocal child = Probe.Find(\"{}\", 1)\nlocal holders = {{}}\n", doomed.GetUUID().ToString(), child.GetUUID().ToString());
			std::vector<const ComponentInfo*> components;
			for (const auto* component : types.GetComponents())
			{
				if (!component->HasFlag(ComponentFlags::ScriptVisible) || component->HasFlag(ComponentFlags::Hidden) || component->HasFlag(ComponentFlags::EntityLevel))
					continue;
				CAPTURE(component->GetName());
				const Entity holder = scene.CreateEntity(component->GetName(), doomed);
				const auto added = AddCoverageComponent(holder, *component, fixture.GetFieldSchemas());
				REQUIRE_MESSAGE(added, (added ? "" : added.error().ToString()));
				prelude += std::format("holders[\"{}\"] = Probe.Find(\"{}\", 1)\n", component->GetName(), holder.GetUUID().ToString());
				components.push_back(component);
			}
			prelude += R"(
local function reads(name, fn)
	local ok, message = pcall(fn)
	if not ok then error(name .. " refused a pending entity: " .. tostring(message)) end
end
local function refuses(name, fn)
	local ok, message = pcall(fn)
	if ok or not string.find(tostring(message), "was destroyed", 1, true) then error(name .. " was not refused: " .. tostring(message)) end
end
)";
			REQUIRE(fixture.Start());

			// The completeness check: every member of Entity and of the component proxies is classified, the classification
			// agrees with its mutation flag, and no check names a member that does not exist.
			std::map<std::string, const ScriptApiMember*, std::less<>> members;
			for (const auto& group : fixture.GetApi().GetTypes())
			{
				if (group.Name != "Entity" && types.FindComponent(group.Name) == nullptr)
					continue;
				for (const auto& member : group.Members)
					members.emplace(group.Name + "." + member.Name, &member);
			}
			const auto findCheck = [](const std::vector<MemberCheck>& checks, std::string_view name)
			{
				return std::ranges::any_of(checks, [name](const MemberCheck& check)
				{
					return check.Member == name;
				});
			};
			for (const auto& [name, member] : members)
			{
				CAPTURE(name);
				if (member->Kind == ScriptApiMemberKind::ProxyField || (member->Kind == ScriptApiMemberKind::Property && member->ComponentTypeIndex.has_value()))
					continue; // every proxy field and shortcut is checked below, from the type registry
				const bool read = findCheck(reads, name) || findCheck(systemReads, name);
				const bool write = findCheck(writes, name);
				if (member->Kind == ScriptApiMemberKind::Property)
				{
					CHECK(read);
					CHECK(write == member->Writable);
				}
				else
				{
					CHECK(read != write);
					CHECK(write == member->Options.Mutates);
				}
			}
			for (const auto* checks : { &reads, &systemReads, &writes })
				for (const auto& check : *checks)
				{
					CAPTURE(check.Member);
					CHECK(members.contains(check.Member));
				}

			scene.DestroyEntity(doomed);
			REQUIRE_FALSE(doomed.IsValid());
			fixture.ExternalMutations.clear();
			const uint64_t revision = scene.GetRevision();

			std::string readSource = prelude;
			for (const auto& check : reads)
				readSource += std::format("reads(\"{}\", function() {} end)\n", check.Member, check.Lua);
			for (const auto* component : components)
			{
				const std::string& name = component->GetName();
				if (!component->HasFlag(ComponentFlags::NoShortcut))
					readSource += std::format("reads(\"Entity.{0}\", function() assert(holders[\"{0}\"][\"{0}\"] ~= nil) end)\n", name);
				for (const auto& field : component->GetFields())
					if (field->GetMeta().Scriptable && !field->GetMeta().Hidden && HasFlag(field->GetMeta().Modes, RunModes::Editor))
						readSource += std::format("reads(\"{0}.{1}\", function() return holders[\"{0}\"]:GetComponent(\"{0}\")[\"{1}\"] end)\n", name, field->GetName());
			}
			for (const auto& check : systemReads)
				readSource += std::format("refuses(\"{}\", function() return {} end)\n", check.Member, check.Lua);
			readSource += "return true\n";
			CheckProxyScript(fixture, readSource);
			CHECK(fixture.ExternalMutations.empty());
			CHECK(scene.GetRevision() == revision);

			std::string writeSource = prelude;
			for (const auto& check : writes)
				writeSource += std::format("refuses(\"{}\", function() {} end)\n", check.Member, check.Lua);
			for (const auto* component : components)
				for (const auto& field : component->GetFields())
					if (field->GetMeta().Scriptable && !field->GetMeta().Hidden && !field->IsReadOnly() && HasFlag(field->GetMeta().Modes, RunModes::Editor))
						writeSource += std::format("refuses(\"{0}.{1}\", function() local c = holders[\"{0}\"]:GetComponent(\"{0}\"); c[\"{1}\"] = c[\"{1}\"] end)\n", component->GetName(), field->GetName());
			writeSource += "return true\n";
			CheckProxyScript(fixture, writeSource);
			CHECK(fixture.ExternalMutations.empty());
			CHECK(scene.GetRevision() == revision);

			// Once flushed, nothing but the identity itself can be read.
			scene.FlushPendingDestroys();
			std::string flushedSource = prelude;
			for (const auto& check : reads)
			{
				if (identityReads.contains(check.Member))
					flushedSource += std::format("reads(\"{}\", function() {} end)\n", check.Member, check.Lua);
				else
					flushedSource += std::format("refuses(\"{}\", function() {} end)\n", check.Member, check.Lua);
			}
			flushedSource += "return true\n";
			CheckProxyScript(fixture, flushedSource);
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
