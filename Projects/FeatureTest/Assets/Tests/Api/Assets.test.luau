--!strict
-- The Assets module (§11.5): references by path and by handle, their paths and types, and missing references.
return Test.Suite("Api.Assets", function()
	Test.Case("assets load by path and by handle and report their path and type", function()
		local clip = Assets.Load("Assets/Audio/Beep.sfx")
		Test.Expect(clip ~= nil and Assets.IsValid(clip), "the sound effect loads")
		local path = Assets.GetPath(clip :: AssetRef)
		Test.ExpectEqual(path, "Assets/Audio/Beep.sfx")
		Test.ExpectEqual(Assets.GetType(clip :: AssetRef), "AudioClip")
		local script = Assets.Load("Assets/Scripts/Api/Probe.luau") :: AssetRef
		Test.ExpectEqual(Assets.GetType(script), "Script")
		local cube = Assets.Load("engine://Meshes/Cube") :: AssetRef
		Test.ExpectEqual(Assets.GetType(cube), "Mesh")
		Test.ExpectEqual(Assets.GetPath(cube), "engine://Meshes/Cube")
		Test.ExpectEqual(Assets.GetType(Assets.Load("Assets/Scenes/Scripting.scene") :: AssetRef), "Scene")
		Test.ExpectEqual(Assets.GetType(Assets.Load("engine://Materials/Default") :: AssetRef), "Material")
	end)

	Test.Case("a handle names the same asset as its path", function()
		local speaker = Scene.FindByName("Speaker") :: Entity
		local clip = (speaker.AudioSource :: AudioSource).Clip :: AssetRef
		Test.ExpectEqual(Assets.GetPath(clip), "Assets/Audio/Beep.sfx")
		local probe = (Scene.FindByName("Probe") :: Entity):GetComponent("Script") :: Script
		local script = probe.Script :: AssetRef
		local byPath = Assets.Load(Assets.GetPath(script))
		Test.ExpectEqual(byPath, script)
	end)

	Test.Case("missing references are nil and not valid", function()
		Test.ExpectEqual(Assets.Load("Assets/Tests/Api/Missing.sfx"), nil)
		Test.ExpectEqual(Assets.Load("fedcba9876543210"), nil)
		Test.ExpectEqual(Assets.IsValid(nil), false)
	end)
end)
