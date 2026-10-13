--!strict
-- The MeshRenderer proxy (§11.5): material slots resolve to the mesh default, and every field reads and writes.
local Support = require("./Support")

return Test.Suite("Api.MeshRenderer", function()
	Test.Case("material slots resolve to the mesh default and take overrides", function()
		local mesh = Support.Find("Crate").MeshRenderer :: MeshRenderer
		local default = Assets.Load("engine://Materials/Default") :: AssetRef
		local replacement = Assets.Load("engine://Materials/Error") :: AssetRef
		Test.ExpectEqual(mesh:GetMaterial(1), default, "an empty slot list uses the mesh default")
		mesh:SetMaterial(1, replacement)
		Test.ExpectEqual(mesh:GetMaterial(1), replacement)
		Test.ExpectEqual(#mesh.Materials, 1, "the override expands the slot list")
		mesh:SetMaterial(1, nil)
		Test.ExpectEqual(mesh:GetMaterial(1), default, "nil restores the default")
		Test.Expect(not pcall(function()
			mesh:GetMaterial(2)
		end), "the cube has one slot")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local crate = Support.Find("Crate")
		local mesh = crate.MeshRenderer :: MeshRenderer
		local cube = Assets.Load("engine://Meshes/Cube") :: AssetRef
		local sphere = Assets.Load("engine://Meshes/Sphere") :: AssetRef
		Test.ExpectEqual(Support.RoundTrip(mesh, "Mesh", sphere), cube, "the scene's mesh")
		local replacement = Assets.Load("engine://Materials/Error") :: AssetRef
		mesh.Materials = { replacement }
		Test.ExpectEqual(#mesh.Materials, 1)
		Test.ExpectEqual(mesh.Materials[1], replacement)
		mesh.Materials = {}
		Test.ExpectEqual(#mesh.Materials, 0)
		Test.ExpectEqual(Support.RoundTrip(mesh, "CastShadows", false), true)
		Test.ExpectEqual(Support.RoundTrip(mesh, "ReceiveShadows", false), true)
		Test.ExpectEqual(Support.RoundTrip(mesh, "Visible", false), true)
		mesh.Visible = true
		mesh.Mesh = cube
		local minimum, maximum = crate:GetWorldBounds()
		Test.Expect(minimum ~= nil and maximum ~= nil, "a visible mesh has render bounds")
	end)
end)
