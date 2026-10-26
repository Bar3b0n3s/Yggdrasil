--!strict
-- Two open scenes (Docs/Decisions/0031-m20-contract.md decisions 13 to 16): MultiSceneSecond.scene was the active scene
-- while the MultiScene scaffold built part of MultiSceneFirst.scene through scene targets, so it holds only what its own
-- requests built; its Beacon instance was rebuilt when the other open scene applied its edit to Beacon.prefab
-- (decision 15), and a request targeting it saved it while it was not the active scene.
local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " exists")
	return entity
end

return Test.Suite("MultiScene.Second", function()
	Test.Case("the scene holds only what was built while it was the active scene", function()
		Test.ExpectEqual(Find("Marker").Transform.Translation, vector.create(-2, 0.5, 0))
		Test.ExpectEqual(Scene.FindByName("Gate"), nil, "the targeted requests built nothing here")
		Test.ExpectEqual(Scene.FindByName("Post"), nil)
	end)

	Test.Case("its prefab instance follows the prefab applied from the other open scene", function()
		Test.ExpectEqual((Find("Beacon").PointLight :: PointLight).Intensity, 6)
	end)
end)
