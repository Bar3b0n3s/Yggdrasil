--!strict
-- Navigation.scene (M30; Docs/Decisions/0045-m30-navigation.md decisions 4, 5 and 13): the arena navigation.bake baked
-- (Navigation.jsonl, IdSeed 2701), a floor with a pillar, a ramp up to a platform, a lava channel NavModifier that is not
-- walkable and a costly mud NavModifier, under a Navigation component of two agent types. The committed bake loads as a
-- NavMesh in every run mode (gate 9's .navmesh), and the scene holds every Navigation and NavModifier field at a
-- non-default value (gates 1 and 1b).
return Test.Suite("Navigation", function()
	Test.Case("the committed bake loads as a NavMesh", function()
		local mesh = Assets.Load("Assets/Navigation/Scenes/Navigation.navmesh")
		assert(mesh, "the scene's bake is committed")
		Test.ExpectEqual(Assets.GetType(mesh), "NavMesh")
		Test.ExpectEqual(Assets.GetPath(mesh), "Assets/Navigation/Scenes/Navigation.navmesh")
	end)
end)
