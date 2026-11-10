--!strict
-- The SkinnedMesh proxy (M23; Docs/Decisions/0034-m23-contract.md decision 31) in Animation.scene: MorphWeights read
-- and written, and SetMorphWeight by the morph target's name (Skinned.gltf's one target, Bulge).
local Animation = require("../Animation/Animation")

return Test.Suite("Api.SkinnedMesh", function()
	Test.Case("SetMorphWeight sets a target's weight by name and refuses what the mesh lacks", function()
		local skinned = Animation.Find("/Waver/Armature/Body").SkinnedMesh :: SkinnedMesh
		skinned:SetMorphWeight("Bulge", 0.5)
		Test.ExpectEqual(skinned.MorphWeights[1], 0.5)
		skinned:SetMorphWeight("Bulge", -1)
		Test.ExpectEqual(skinned.MorphWeights[1], -1)
		Test.Expect(not pcall(function()
			skinned:SetMorphWeight("NoSuchTarget", 0.5)
		end), "a target the mesh lacks raises")
		Test.Expect(not pcall(function()
			skinned:SetMorphWeight("Bulge", 1.5)
		end), "a weight above 1 raises")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local skinned = Animation.Find("/Statue/Armature/Body").SkinnedMesh :: SkinnedMesh
		local before = skinned.MorphWeights
		Test.ExpectEqual(#before, 1)
		Test.ExpectEqual(before[1], 0.75)
		skinned.MorphWeights = { -0.25 }
		Test.ExpectEqual(#skinned.MorphWeights, 1)
		Test.ExpectEqual(skinned.MorphWeights[1], -0.25)
		skinned.MorphWeights = {}
		Test.ExpectEqual(#skinned.MorphWeights, 0)
	end)
end)
