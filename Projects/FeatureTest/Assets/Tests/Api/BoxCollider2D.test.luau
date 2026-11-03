--!strict
-- The BoxCollider2D proxy: every field read and written, and a resized box reshapes the body.
local Support = require("./Support")

return Test.Suite("Api.BoxCollider2D", function()
	Test.Case("every field reads and writes through the proxy and reshapes the body", function()
		local crate = Support.Find("Crate")
		local box = crate.BoxCollider2D :: BoxCollider2D
		Test.ExpectEqual(Support.RoundTrip(box, "HalfExtents", vector.create(0.6, 1, 0)), vector.create(0.6, 0.4, 0))
		Test.ExpectEqual(Support.RoundTrip(box, "Offset", vector.create(0, 0.25, 0)), vector.zero)
		Test.ExpectEqual(Support.RoundTrip(box, "Angle", 30, 1e-5), 0)
		box.Angle = 0
		Test.ExpectEqual(Support.RoundTrip(box, "IsTrigger", true), false)
		box.IsTrigger = false
		box.Offset = vector.zero
		Test.WaitTicks(1)
		local hit = Physics2D.Raycast(vector.create(-2, 5, 0), vector.create(0, -1, 0), 20, Physics.LayerMask("Default"))
		Test.Expect(hit ~= nil, "the reshaped crate is hit")
		Test.Expect((hit :: RaycastHit).Point.y > 1, "the taller box reaches higher")
		Test.Expect(not pcall(function()
			box.HalfExtents = vector.create(0, 1, 0)
		end), "half extents below 1 mm are refused")
	end)
end)
