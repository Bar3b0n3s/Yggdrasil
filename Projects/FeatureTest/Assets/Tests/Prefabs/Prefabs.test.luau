--!strict
-- Prefabs (§5.5, §15.5): instances the editor placed in Prefabs.scene, one carrying an override of every
-- PrefabOverrideKind (a field, an added and a removed component, and entity keys); a nested prefab flattened into one
-- instance; references into instances from outside; and instances a script creates with Scene.Instantiate.
local LinkedScript = require("../../Scripts/Prefabs/Linked")
local TaggerScript = require("../../Scripts/Prefabs/Tagger")

local function Find(path: string): Entity
	local entity = Scene.FindByPath(path)
	assert(entity ~= nil, path .. " exists")
	return entity
end

local function Names(entity: Entity): string
	local names: { string } = {}
	for _, child in entity:GetChildren() do
		table.insert(names, child.Name)
	end
	return table.concat(names, ",")
end

-- The unmodified structure of a Crate instance rooted at `root`.
local function ExpectPlainCrate(root: Entity, what: string)
	Test.ExpectEqual(Names(root), "Lid,Label", what .. ": children")
	local lid = root:FindChild("Lid") :: Entity
	local label = root:FindChild("Label") :: Entity
	Test.ExpectEqual((lid.MeshRenderer :: MeshRenderer).Visible, true, what .. ": the lid is visible")
	Test.Expect(not lid:HasComponent("PointLight"), what .. ": the lid has no light")
	Test.ExpectEqual((label.Text :: Text).Text, "Crate", what .. ": the label")
	local tagger = root:GetScript() :: TaggerScript.Tagger
	Test.ExpectEqual(tagger.Lid, lid, what .. ": the prefab's internal reference names this instance's lid")
	Test.Expect(root:HasTag("Crate") and lid:HasTag("Lid"), what .. ": OnCreate ran for root and lid")
	Test.ExpectEqual(root.RigidBody ~= nil, true)
end

return Test.Suite("Prefabs", function()
	Test.Case("an editor-placed instance keeps an override of every kind", function()
		local crate = Find("/Crate")
		Test.ExpectEqual(Names(crate), "Cover,Label", "the Lid was renamed (an entity key override)")
		local cover = crate:FindChild("Cover") :: Entity
		Test.ExpectEqual((cover.MeshRenderer :: MeshRenderer).Visible, false, "a field override")
		local light = cover.PointLight
		Test.Expect(light ~= nil, "an added component")
		Test.ExpectEqual((light :: PointLight).Intensity, 4)
		Test.ExpectEqual((light :: PointLight).Range, 3)
		local label = crate:FindChild("Label") :: Entity
		Test.Expect(not label:HasComponent("Text"), "a removed component")
		Test.Expect(crate:HasTag("Editor") and crate:HasTag("Crate"), "the root's tag override and OnCreate's tag")
		local tagger = crate:GetScript() :: TaggerScript.Tagger
		Test.ExpectEqual(tagger.Lid, cover, "the internal reference follows the renamed member")
		Test.ExpectEqual(tagger.Tag, "Crate")
		Test.ExpectEqual((cover:GetScript() :: TaggerScript.Tagger).Tag, "Lid")
	end)

	Test.Case("an instance without overrides matches its prefab", function()
		local crateB = Find("/CrateB")
		ExpectPlainCrate(crateB, "CrateB")
		Test.ExpectNear(crateB.Transform.WorldPosition, vector.create(0, 0.5, -3), 0.01, "placed where the editor put it")
	end)

	Test.Case("a nested prefab is flattened into one instance of the outer prefab", function()
		local cart = Find("/Cart")
		Test.ExpectEqual(Names(cart), "Body,FrontWheel,BackWheel")
		for _, name in { "FrontWheel", "BackWheel" } do
			local wheel = cart:FindChild(name) :: Entity
			Test.ExpectEqual(Names(wheel), "Hub", name .. " keeps the inner prefab's subtree")
			Test.Expect(wheel.MeshRenderer ~= nil, name .. " keeps the inner prefab's components")
		end
		Test.ExpectNear((cart:FindChild("FrontWheel") :: Entity).Transform.Translation, vector.create(0.6, -0.3, 0), 0.0001)
		Test.ExpectNear((cart:FindChild("BackWheel") :: Entity).Transform.Translation, vector.create(-0.6, -0.3, 0), 0.0001)
	end)

	Test.Case("references from outside resolve to instance members and the prefab asset", function()
		local linked = Find("/Inspector"):GetScript() :: LinkedScript.Linked
		local crateB = Find("/CrateB")
		Test.ExpectEqual(linked.Crate, crateB)
		Test.ExpectEqual(linked.Target, crateB:FindChild("Lid"))
		Test.ExpectEqual(linked.Target, Find("/CrateB/Lid"))
		Test.ExpectEqual(linked.Spawnable, Assets.Load("Assets/Prefabs/Crate.prefab"))
		Test.ExpectEqual(Assets.GetType(linked.Spawnable :: AssetRef), "Prefab")
	end)

	Test.Case("a script instantiates prefabs with their subtree, remapped references and fresh ids", function()
		local linked = Find("/Inspector"):GetScript() :: LinkedScript.Linked
		local prefab = linked.Spawnable :: AssetRef
		local before = Scene.GetEntityCount()
		local first = Scene.Instantiate(prefab, vector.create(-1, 2, 2), Quat.FromEuler(0, 90, 0))
		local second = Scene.Instantiate(prefab, vector.create(1, 2, 2))
		Test.ExpectEqual(Scene.GetEntityCount(), before + 6, "each instance has three entities")
		ExpectPlainCrate(first, "first")
		ExpectPlainCrate(second, "second")
		Test.Expect(first.ID ~= second.ID and first.ID ~= Find("/CrateB").ID, "every instance has its own ids")
		Test.Expect((first:FindChild("Lid") :: Entity).ID ~= (second:FindChild("Lid") :: Entity).ID, "members too")
		Test.ExpectNear(first.Transform.WorldPosition, vector.create(-1, 2, 2), 0.0001)
		Test.ExpectNear(first.Transform:Forward(), vector.create(-1, 0, 0), 0.0001, "the instance takes the rotation")
		local parent = Find("/Cart")
		local child = Scene.Instantiate(Assets.Load("Assets/Prefabs/Wheel.prefab") :: AssetRef, nil, nil, parent)
		Test.ExpectEqual(child:GetParent(), parent)
		Test.ExpectEqual(Names(child), "Hub")
		Test.ExpectEqual(#parent:GetChildren(), 4)
		local cart = Scene.Instantiate(Assets.Load("Assets/Prefabs/Cart.prefab") :: AssetRef, vector.create(5, 3, 0))
		Test.ExpectEqual(Names(cart), "Body,FrontWheel,BackWheel")
		Test.WaitTicks(2)
		Test.Expect(first:IsValid() and (first.RigidBody :: RigidBody):GetLinearVelocity().y < 0, "the instance's body simulates")
	end)
end)
