--!strict
-- The MeshCollider proxy: every field read and written; the collider uses the engine cube's hull.
local Support = require("./Support")

return Test.Suite("Api.MeshCollider", function()
	Test.Case("every field reads and writes through the proxy", function()
		local pillar = Support.Find("Pillar")
		local collider = pillar.MeshCollider :: MeshCollider
		local cube = Assets.Load("engine://Meshes/Cube") :: AssetRef
		local sphere = Assets.Load("engine://Meshes/Sphere") :: AssetRef
		Test.ExpectEqual(Support.RoundTrip(collider, "Mesh", sphere), cube, "the scene's mesh")
		Test.ExpectEqual(Support.RoundTrip(collider, "Convex", false), true, "the scene's convex hull")
		Test.ExpectEqual(Support.RoundTrip(collider, "IsTrigger", true), false)
		collider.IsTrigger = false
		collider.Convex = true
		Support.RoundTrip(collider, "Mesh", cube)
		Test.WaitTicks(1)
		local minimum, maximum = Physics.GetColliderBounds(pillar)
		Test.Expect(minimum ~= nil and maximum ~= nil, "the hull has bounds")
		Test.ExpectNear((maximum :: vector) - (minimum :: vector), vector.create(1, 2, 1), 0.02, "the scaled unit cube")
	end)
end)
