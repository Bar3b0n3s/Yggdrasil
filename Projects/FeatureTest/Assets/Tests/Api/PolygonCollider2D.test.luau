--!strict
-- The PolygonCollider2D proxy: every field read and written, and new points reshape the body.
local Support = require("./Support")

return Test.Suite("Api.PolygonCollider2D", function()
	Test.Case("every field reads and writes through the proxy and reshapes the body", function()
		local wedge = Support.Find("Wedge")
		local polygon = wedge.PolygonCollider2D :: PolygonCollider2D
		local points = polygon.Points
		Test.ExpectEqual(#points, 3)
		Test.ExpectNear(points[3], vector.create(0.6, 0.5, 0), 1e-6, "the authored wedge")
		local square = { vector.create(-1, -0.5, 0), vector.create(1, -0.5, 0), vector.create(1, 1.5, 0), vector.create(-1, 1.5, 0) }
		polygon.Points = square
		Test.ExpectEqual(#polygon.Points, 4)
		Test.ExpectNear(polygon.Points[3], vector.create(1, 1.5, 0), 1e-6, "Points reads back what was written")
		Test.ExpectNear(Support.RoundTrip(polygon, "Offset", vector.create(0.25, 0, 0), 1e-6), vector.create(0.05, 0, 0), 1e-6)
		Test.ExpectNear(Support.RoundTrip(polygon, "Angle", 0, 1e-6), 5, 1e-5)
		Test.ExpectEqual(Support.RoundTrip(polygon, "IsTrigger", true), false)
		polygon.IsTrigger = false
		Test.WaitTicks(1)
		local position = wedge.Transform.WorldPosition
		local found = Physics2D.OverlapCircle(position + vector.create(0.25, 1.2, 0), 0.05, Physics.LayerMask("Default"))
		Test.Expect(table.find(found, wedge) ~= nil, "the taller polygon reaches 1.2 above its entity")
		Test.Expect(not pcall(function()
			polygon.Points = { vector.zero, vector.one }
		end), "fewer than 3 points are refused")
	end)
end)
