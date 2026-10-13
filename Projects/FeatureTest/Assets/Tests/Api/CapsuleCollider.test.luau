--!strict
-- The CapsuleCollider proxy: every field read and written, and the resized capsule's bounds.
local Support = require("./Support")

return Test.Suite("Api.CapsuleCollider", function()
	Test.Case("every field reads and writes through the proxy and reshapes the body", function()
		local pole = Support.Find("Pole")
		local capsule = pole.CapsuleCollider :: CapsuleCollider
		Test.ExpectNear(Support.RoundTrip(capsule, "Radius", 0.3, 1e-6), 0.2, 1e-6, "the scene's radius")
		Test.ExpectNear(Support.RoundTrip(capsule, "HalfHeight", 1, 1e-6), 0.8, 1e-6, "the scene's half height")
		Support.RoundTrip(capsule, "Offset", vector.create(0, 0.5, 0))
		local tilt = Quat.FromEuler(0, 0, 90)
		capsule.Rotation = tilt
		Support.ExpectQuatNear(capsule.Rotation, tilt, 1e-6, "Rotation reads back what was written")
		capsule.Rotation = Quat.Identity()
		Support.RoundTrip(capsule, "IsTrigger", true)
		capsule.IsTrigger = false
		Test.WaitTicks(1)
		local minimum, maximum = Physics.GetColliderBounds(pole)
		Test.Expect(minimum ~= nil and maximum ~= nil, "the reshaped capsule has bounds")
		Test.ExpectNear((maximum :: vector) - (minimum :: vector), vector.create(0.6, 2.6, 0.6), 0.01, "radius and half height")
		Test.ExpectNear(((maximum :: vector) + (minimum :: vector)) * 0.5, vector.create(6, 1.5, -2), 0.01, "the offset")
	end)
end)
