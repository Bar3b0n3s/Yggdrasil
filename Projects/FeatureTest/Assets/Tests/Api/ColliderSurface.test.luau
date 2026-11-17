--!strict
-- The ColliderSurface proxy (Docs/Decisions/0053-m36-contact-surfaces.md; §5.3, §11.4): every field reads and writes, and
-- Physics.GetSurface follows a write at once.
local Support = require("./Support")

return Test.Suite("Api.ColliderSurface", function()
	Test.Case("every field reads and writes through the proxy", function()
		local ice = Support.Find("Ice")
		local surface = ice.ColliderSurface :: ColliderSurface
		Test.ExpectNear(Support.RoundTrip(surface, "Friction", 0.3, 1e-6), 0.02, 1e-6, "the ice's friction")
		Test.ExpectEqual(Support.RoundTrip(surface, "Restitution", 0.5, 1e-6), 0, "no restitution of its own")
		Test.ExpectEqual(Support.RoundTrip(surface, "SurfaceVelocity", vector.create(1, 0, -1), 1e-6), vector.zero, "no velocity of its own")
		local read = Physics.GetSurface(ice)
		assert(read, "the ice has a surface")
		Test.ExpectNear(read.Friction, 0.3, 1e-6)
		Test.ExpectNear(read.Restitution, 0.5, 1e-6)
		Test.ExpectNear(read.SurfaceVelocity, vector.create(1, 0, -1), 1e-6)
	end)
end)
