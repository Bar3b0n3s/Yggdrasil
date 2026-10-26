--!strict
-- Two open scenes (Docs/Decisions/0031-m20-contract.md decisions 13 to 16): the MultiScene scaffold built part of
-- MultiSceneFirst.scene while MultiSceneSecond.scene was the active scene, through requests whose scene target named this
-- one (an entity.create, and an edit.batch whose entity paths and Gate's id resolved here), then applied its Beacon
-- instance's edit to Beacon.prefab, which also rebuilt the Beacon of the other open scene (Second.test.luau).
local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " exists")
	return entity
end

return Test.Suite("MultiScene.First", function()
	Test.Case("the requests that targeted this scene built it while another scene was active", function()
		local gate = Find("Gate")
		Test.ExpectEqual(gate.Transform.Translation, vector.create(2, 0.5, 0), "the targeted batch moved Gate by its id")
		local post = Find("Post")
		Test.ExpectEqual(post:GetParent(), gate, "the targeted batch created Post under Gate")
		Test.ExpectEqual((post.PointLight :: PointLight).Intensity, 4, "and updated it by its path")
		Test.ExpectEqual(Scene.FindByName("Marker"), nil, "what the active scene's requests built stayed in that scene")
	end)

	Test.Case("the instance the prefab was applied from holds the applied value", function()
		Test.ExpectEqual((Find("Beacon").PointLight :: PointLight).Intensity, 6)
	end)
end)
