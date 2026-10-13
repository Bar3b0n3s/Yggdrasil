#include "TestsPCH.h"
#include "Engine/Scripting/ScriptProxy.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	static void CheckEntityScript(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
		CHECK(result->Value.Get() == Json(true));
	}

	namespace Test {

		void RunEntityBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			// Script.Define and every Field constructor are covered by Bindings/ScriptTests.cpp; this behaviour serves GetScript.
			REQUIRE(fixture.AddScript(AssetHandle(200), "Assets/Coverage.luau", R"(
local T = {}
function T:OnCreate() self.Ready = true end
return Script.Define("Coverage", T)
)"));
			const Entity bound = fixture.GetScene().CreateEntity("Bound");
			ScriptComponent script;
			script.Script.SetHandle(AssetHandle(200));
			bound.AddComponent<ScriptComponent>(script);
			REQUIRE(fixture.Start());
			CheckEntityScript(fixture, R"(
local bound = Scene.FindByName("Bound")
assert(bound:GetScript().Ready)
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
child:Destroy()
assert(not child:IsValid() and not grandchild:IsValid())
return true
)");
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("EntityBindings: hierarchy tags components and fields agree with scene state")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunEntityBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("EntityBindings: names and tags kept by the host are bounded before any mutation")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start());
			CheckEntityScript(fixture, std::format(R"(
local e = Scene.CreateEntity(string.rep("n", {0}))
assert(#e.Name == {0})
local long = string.rep("n", 2^26)
local ok, message = pcall(function() e.Name = long end)
assert(not ok and string.find(message, "argument #2 is 67108864 bytes; the limit is {0}", 1, true), message)
assert(#e.Name == {0})
ok = pcall(Scene.CreateEntity, string.rep("n", {0} + 1))
assert(not ok)
assert(#Scene.FindAllByName(string.rep("n", {0} + 1)) == 0)
e:AddTag(string.rep("t", {1}))
assert(not pcall(e.AddTag, e, string.rep("t", {1} + 1)))
assert(#e:GetTags() == 1)
return true
)",
										   MaxScriptEntityNameBytes, MaxScriptTagBytes));
			CHECK(fixture.GetScene().GetEntityCount() == 1);
			// An entity carries a bounded number of tags; adding a present tag again is still a no-op.
			CheckEntityScript(fixture, std::format(R"(
local e = Scene.CreateEntity("Tagged")
for i = 1, {0} do e:AddTag("tag" .. i) end
e:AddTag("tag1")
local ok, message = pcall(e.AddTag, e, "one more")
assert(not ok and string.find(message, "entity already has the maximum of {0} tags", 1, true), message)
assert(#e:GetTags() == {0} and not e:HasTag("one more"))
return true
)",
										   MaxScriptEntityTags));
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("EntityBindings: component text a script writes is bounded before any mutation")
		{
			// One 64 MiB VM string stored on every entity would otherwise keep a host copy per component (M13 review F17).
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.AddScript(AssetHandle(403), "Assets/Scripts/Holder.luau", "return Script.Define('Holder', {Fields = {Names = Field.Array(Field.String())}})"));
			REQUIRE(fixture.Start());
			CheckEntityScript(fixture, std::format(R"(
local limit, long = {0}, string.rep("x", 2^26)
local function refused(write)
	local ok, message = pcall(write)
	assert(not ok and string.find(message, "holds more than the {0} bytes of text a script may write into a component", 1, true), message)
end
local label = Scene.CreateEntity("Label")
refused(function() label:AddComponent("Text", {{Text = long}}) end)
assert(not label:HasComponent("Text"))
local text = label:AddComponent("Text", {{Text = string.rep("t", limit)}})
assert(#text.Text == limit)
refused(function() text.Text = string.rep("y", limit + 1) end)
refused(function() text.Text = long end)
assert(text.Text == string.rep("t", limit))
-- Strings and map keys of one value count together.
local holder = Scene.CreateEntity("Holder")
local script = holder:AddComponent("Script", {{Script = Assets.Load("Assets/Scripts/Holder.luau")}})
refused(function() script.Fields = {{Names = {{string.rep("a", limit / 2), string.rep("b", limit / 2)}}}} end)
assert(next(script.Fields) == nil)
script.Fields = {{Names = {{string.rep("a", limit / 4), "b"}}}}
assert(script.Fields.Names[2] == "b")
return true
)",
										   MaxScriptComponentStringBytes));
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("EntityBindings: AddComponent returns nil when the new behaviour's OnCreate destroys its entity")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.AddScript(AssetHandle(401), "Assets/Scripts/OneShot.luau", "local T = {}\nfunction T:OnCreate() self.Entity:Destroy() end\nreturn Script.Define('OneShot', T)"));
			REQUIRE(fixture.AddScript(AssetHandle(402), "Assets/Scripts/Keeper.luau", "local T = {}\nfunction T:OnCreate() self.Created = true end\nreturn Script.Define('Keeper', T)"));
			REQUIRE(fixture.Start());
			CheckEntityScript(fixture, R"(
local doomed = Scene.CreateEntity("Doomed")
local proxy = doomed:AddComponent("Script", {Script = Assets.Load("Assets/Scripts/OneShot.luau")})
assert(proxy == nil and not doomed:IsValid())
local kept = Scene.CreateEntity("Kept")
local script = kept:AddComponent("Script", {Script = Assets.Load("Assets/Scripts/Keeper.luau")})
assert(script ~= nil and kept:GetScript().Created == true)
return true
)");
			// The caller is not faulted (no error, no pause) although the add happened.
			CHECK(fixture.Errors.empty());
			CHECK_FALSE(fixture.PauseRequested);
		}

		TEST_CASE("EntityBindings: one identity is one userdata while referenced, so entity-keyed sets work")
		{
			Test::ScriptTestFixture fixture;
			static_cast<void>(fixture.GetScene().CreateEntity("A"));
			static_cast<void>(fixture.GetScene().CreateEntity("B"));
			REQUIRE(fixture.Start());
			CheckEntityScript(fixture, R"(
local a = Scene.FindByName("A")
assert(rawequal(a, Scene.FindByName("A")) and rawequal(a, Scene.FindByID(a.ID)))
assert(not rawequal(a, Scene.FindByName("B")))
-- OnTriggerEnter/Exit style bookkeeping: separate lookups of one entity address the same key.
local inside = {}
inside[Scene.FindByName("A")] = true
inside[Scene.FindByName("B")] = true
inside[Scene.FindAllByName("A")[1]] = nil
local count = 0
for entity in pairs(inside) do count += 1; assert(entity.Name == "B") end
assert(count == 1)
-- Component proxies and asset references are identities too.
assert(rawequal(a.Transform, Scene.FindByName("A"):GetComponent("Transform")))
return true
)");
			CHECK(fixture.Errors.empty());
		}
	}

}
