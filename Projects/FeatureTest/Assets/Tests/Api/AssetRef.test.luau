--!strict
-- AssetRef values (§11.5): references to the same asset compare equal by handle and are one table key.
return Test.Suite("Api.AssetRef", function()
	Test.Case("two references to one asset are equal and others are not", function()
		local a = Assets.Load("Assets/Audio/Beep.sfx") :: AssetRef
		local b = Assets.Load("Assets/Audio/Beep.sfx") :: AssetRef
		local cube = Assets.Load("engine://Meshes/Cube") :: AssetRef
		Test.Expect(a == b, "the same asset")
		Test.Expect(a ~= cube, "different assets")
		Test.Expect(not (cube == a), "equality is symmetric")
		local seen: { [AssetRef]: boolean } = { [a] = true }
		Test.Expect(seen[b] == true, "one asset is one table key")
		Test.ExpectEqual(seen[cube], nil)
		local speaker = Scene.FindByName("Speaker") :: Entity
		Test.Expect((speaker.AudioSource :: AudioSource).Clip == a, "a component's reference equals a loaded one")
	end)
end)
