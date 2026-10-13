--!strict
-- The Entity type (§11.4, §11.5): identity, names, tags, activity, hierarchy, components, scripts, bounds, destruction
-- and the component shortcut properties of every script-visible component.
local ProbeScript = require("../../Scripts/Api/Probe")
local Support = require("./Support")

-- An entity of Scripting.scene carrying each component that has a shortcut property.
type Carrier = { Component: string, Entity: string }

local CARRIERS: { Carrier } = {
	{ Component = "AudioListener", Entity = "Camera" },
	{ Component = "AudioSource", Entity = "Speaker" },
	{ Component = "BoxCollider", Entity = "Crate" },
	{ Component = "Camera", Entity = "Camera" },
	{ Component = "CapsuleCollider", Entity = "Pole" },
	{ Component = "CharacterController", Entity = "Hero" },
	{ Component = "DirectionalLight", Entity = "Sun" },
	{ Component = "Environment", Entity = "Atmosphere" },
	{ Component = "MeshCollider", Entity = "Pillar" },
	{ Component = "MeshRenderer", Entity = "Crate" },
	{ Component = "PointLight", Entity = "Lamp" },
	{ Component = "PostProcess", Entity = "Atmosphere" },
	{ Component = "RigidBody", Entity = "Crate" },
	{ Component = "SphereCollider", Entity = "Ball" },
	{ Component = "SpotLight", Entity = "Spot" },
	{ Component = "Text", Entity = "Sign" },
	{ Component = "Transform", Entity = "Probe" },
}

return Test.Suite("Api.Entity", function()
	Test.Case("identity, name and equality", function()
		local probe = Support.Find("Probe")
		Test.ExpectEqual(#probe.ID, 16, "the ID is 16 hexadecimal digits")
		Test.Expect(string.match(probe.ID, "^%x+$") ~= nil, "hexadecimal")
		Test.ExpectEqual(Scene.FindByID(probe.ID), probe)
		Test.ExpectEqual(probe.Name, "Probe")
		probe.Name = "Renamed"
		Test.ExpectEqual(probe.Name, "Renamed")
		Test.ExpectEqual(Scene.FindByName("Renamed"), probe)
		probe.Name = "Probe"
		Test.Expect(probe == Support.Find("Probe"), "two identities of one entity are equal")
		Test.Expect(probe ~= Support.Find("Crate"), "different entities are not")
		Test.Expect(probe:IsValid(), "a live entity is valid")
	end)

	Test.Case("every script-visible component has a shortcut that is nil when absent", function()
		local bare = Scene.CreateEntity("Bare")
		for _, carrier in CARRIERS do
			local component: string = carrier.Component
			local label = carrier.Entity .. "." .. component
			local entity: any = Support.Find(carrier.Entity)
			Test.Expect(entity[component] ~= nil, label .. " is present")
			Test.Expect(entity:HasComponent(component), label .. " is reported by HasComponent")
			local shortcut: any = (bare :: any)[component]
			Test.Expect(component == "Transform" or shortcut == nil, "Bare has no " .. component)
		end
		Test.Expect(bare.Transform ~= nil, "every entity has a Transform")
		Test.ExpectEqual(bare:GetComponent("RigidBody"), nil)
		Test.Expect(Support.Find("Crate"):GetComponent("RigidBody") ~= nil, "GetComponent finds a present component")
	end)

	Test.Case("tags are kept in insertion order without duplicates", function()
		local item = Support.Find("Item")
		Test.Expect(item:HasTag("Pickup"), "the scene's tag")
		item:AddTag("Shiny")
		item:AddTag("Pickup")
		Test.ExpectEqual(table.concat(item:GetTags(), ","), "Pickup,Shiny")
		item:RemoveTag("Pickup")
		Test.ExpectEqual(table.concat(item:GetTags(), ","), "Shiny")
		Test.Expect(not item:HasTag("Pickup"), "a removed tag is gone")
	end)

	Test.Case("activity follows the hierarchy", function()
		local holder = Support.Find("Holder")
		local item = Support.Find("Item")
		Test.Expect(item:IsActive() and item:IsActiveSelf(), "both active")
		holder:SetActive(false)
		Test.Expect(not item:IsActive() and item:IsActiveSelf(), "a child of an inactive parent is inactive")
		Test.Expect(not holder:IsActiveSelf(), "the parent itself is inactive")
		holder:SetActive(true)
		Test.Expect(item:IsActive(), "active again")
	end)

	Test.Case("the hierarchy can be read and changed", function()
		local holder = Support.Find("Holder")
		local item = Support.Find("Item")
		Test.ExpectEqual(item:GetParent(), holder)
		Test.ExpectEqual(holder:GetParent(), nil)
		Test.ExpectEqual(#holder:GetChildren(), 1)
		Test.ExpectEqual(holder:GetChildren()[1], item)
		Test.ExpectEqual(holder:FindChild("Item"), item)
		local grandchild = Scene.CreateEntity("Grandchild", item)
		Test.ExpectEqual(holder:FindChild("Grandchild"), nil, "FindChild is not recursive by default")
		Test.ExpectEqual(holder:FindChild("Grandchild", true), grandchild)
		local world = item.Transform.WorldPosition
		item:SetParent(nil)
		Test.ExpectEqual(item:GetParent(), nil)
		Test.ExpectNear(item.Transform.WorldPosition, world, 0.0001, "keepWorld defaults to true")
		item:SetParent(holder, false)
		Test.ExpectEqual(item:GetParent(), holder)
		Test.ExpectNear(item.Transform.Translation, world, 0.0001, "keepWorld false keeps the local pose")
	end)

	Test.Case("components are added and removed, and scripts are reached through GetScript", function()
		local entity = Scene.CreateEntity("Built")
		Test.Expect(not entity:HasComponent("MeshRenderer"), "no mesh yet")
		local minimum, maximum = entity:GetWorldBounds()
		Test.ExpectEqual(minimum, nil)
		Test.ExpectEqual(maximum, nil)
		local mesh = entity:AddComponent("MeshRenderer", { Mesh = Assets.Load("engine://Meshes/Cube"), Visible = false }) :: MeshRenderer
		Test.Expect(entity:HasComponent("MeshRenderer") and mesh.Visible == false, "added with its values")
		minimum, maximum = entity:GetWorldBounds()
		Test.ExpectNear(minimum :: vector, vector.create(-0.5, -0.5, -0.5), 0.0001)
		Test.ExpectNear(maximum :: vector, vector.create(0.5, 0.5, 0.5), 0.0001)
		entity:RemoveComponent("MeshRenderer")
		Test.Expect(not entity:HasComponent("MeshRenderer"), "removed")
		local probe = Support.Find("Probe"):GetScript() :: ProbeScript.Probe
		Test.Expect(probe ~= nil and probe.Created, "GetScript returns the behaviour instance")
		Test.ExpectEqual(entity:GetScript(), nil, "an entity without a script has none")
	end)

	Test.Case("a destroyed entity and its children are invalid at once", function()
		local parent = Scene.CreateEntity("Doomed")
		local child = Scene.CreateEntity("DoomedChild", parent)
		parent:Destroy()
		Test.Expect(not parent:IsValid() and not child:IsValid(), "destroyed with its subtree")
		Test.ExpectEqual(Scene.FindByName("Doomed"), nil)
		Test.Expect(not pcall(function()
			return parent.Name
		end), "a destroyed entity cannot be read")
	end)
end)
