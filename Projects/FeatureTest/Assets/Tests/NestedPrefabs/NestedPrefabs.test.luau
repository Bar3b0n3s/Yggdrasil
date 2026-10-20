--!strict
-- Live nested prefabs (Docs/Decisions/0031-m20-contract.md decisions 7, 8 and 10): NestedPrefabs.scene holds Room2, an
-- instance of Room.prefab, which nests a live instance of Lamp.prefab whose bulb Room.prefab records as hidden. Room2
-- deletes the Table (a RemoveEntity override) and keeps its nested bulb from casting shadows (an override whose path names
-- the nested root and the bulb). The Room the editor built the prefab from is an instance too. Scripts instantiate the
-- nesting prefab whole: the Runtime never loads the nested prefab.

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

local function Renderer(entity: Entity): MeshRenderer
	return entity.MeshRenderer :: MeshRenderer
end

return Test.Suite("NestedPrefabs", function()
	Test.Case("an instance keeps the record of its nested instance and its own overrides of it", function()
		local room = Find("/Room2")
		Test.ExpectEqual(Names(room), "Lamp", "the deleted Table stays deleted")
		local bulb = Find("/Room2/Lamp/Bulb")
		Test.ExpectEqual(Renderer(bulb).Visible, false, "the nesting prefab's record of its nested instance")
		Test.ExpectEqual(Renderer(bulb).CastShadows, false, "the instance's own override of the nested bulb")
		Test.ExpectEqual(Names(Find("/Room")), "Table,Lamp", "the instance the prefab was created from")
		Test.ExpectEqual(Renderer(Find("/Room/Lamp/Bulb")).CastShadows, true)
	end)

	Test.Case("a script instantiates the nesting prefab with its nested instance whole", function()
		local prefab = Assets.Load("Assets/Prefabs/Nested/Room.prefab") :: AssetRef
		local first = Scene.Instantiate(prefab, vector.create(4, 0, 0))
		local second = Scene.Instantiate(prefab, vector.create(-4, 0, 0))
		Test.ExpectEqual(Names(first), "Table,Lamp")
		local lamp = first:FindChild("Lamp") :: Entity
		local bulb = lamp:FindChild("Bulb") :: Entity
		Test.ExpectEqual(Renderer(bulb).Visible, false, "the nested instance's recorded state")
		Test.ExpectEqual(Renderer(bulb).CastShadows, true, "the prefab's own value")
		local otherLamp = second:FindChild("Lamp") :: Entity
		Test.Expect(lamp.ID ~= otherLamp.ID, "each instance's nested instance has its own ids")
		Test.Expect(bulb.ID ~= (otherLamp:FindChild("Bulb") :: Entity).ID, "its members too")
	end)
end)
