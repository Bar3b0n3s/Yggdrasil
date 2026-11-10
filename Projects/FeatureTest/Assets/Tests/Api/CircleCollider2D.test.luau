--!strict
-- The CircleCollider2D proxy: every field read and written, and a larger circle reaches further.
local Support = require("./Support")

return Test.Suite("Api.CircleCollider2D", function()
	Test.Case("every field reads and writes through the proxy and reshapes the sensor", function()
		local sensor = Support.Find("CircleSensor")
		local circle = sensor.CircleCollider2D :: CircleCollider2D
		Test.ExpectEqual(Support.RoundTrip(circle, "Radius", 1.5, 1e-6), 0.75)
		Test.ExpectNear(Support.RoundTrip(circle, "Offset", vector.create(0, 0.5, 0), 1e-6), vector.create(0.25, 0, 0), 1e-6)
		Test.ExpectEqual(Support.RoundTrip(circle, "IsTrigger", false), true)
		circle.IsTrigger = true
		Test.WaitTicks(1)
		local position = sensor.Transform.WorldPosition
		local found = Physics2D.OverlapCircle(position + vector.create(0, 1.8, 0), 0.1, Physics.LayerMask("Trigger"))
		Test.Expect(table.find(found, sensor) ~= nil, "the larger, raised circle reaches 1.8 above its entity")
		Test.Expect(not pcall(function()
			circle.Radius = 0
		end), "a zero radius is refused")
	end)
end)
