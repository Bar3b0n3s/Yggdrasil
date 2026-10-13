--!strict
-- The SphereCollider proxy: every field read and written, and the resized sphere's bounds.
local Support = require("./Support")

return Test.Suite("Api.SphereCollider", function()
	Test.Case("every field reads and writes through the proxy and reshapes the body", function()
		local ball = Support.Find("Ball")
		local sphere = ball.SphereCollider :: SphereCollider
		Test.ExpectNear(Support.RoundTrip(sphere, "Radius", 0.75, 1e-6), 0.5, 1e-6, "the scene's radius")
		Support.RoundTrip(sphere, "Offset", vector.create(0.25, 0, 0))
		Support.RoundTrip(sphere, "IsTrigger", true)
		sphere.IsTrigger = false
		Test.WaitTicks(1)
		local minimum, maximum = Physics.GetColliderBounds(ball)
		Test.Expect(minimum ~= nil and maximum ~= nil, "the reshaped sphere has bounds")
		Test.ExpectNear((maximum :: vector) - (minimum :: vector), vector.create(1.5, 1.5, 1.5), 0.01)
		local center = ((maximum :: vector) + (minimum :: vector)) * 0.5
		Test.ExpectNear(center - ball.Transform.WorldPosition, vector.create(0.25, 0, 0), 0.01, "the offset")
		Test.Expect(not pcall(function()
			sphere.Radius = 0
		end), "a zero radius is refused")
	end)
end)
