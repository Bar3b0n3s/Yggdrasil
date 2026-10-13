--!strict
-- The BoxCollider proxy: every field read and written, and a resized box changes the body's bounds.
local Support = require("./Support")

return Test.Suite("Api.BoxCollider", function()
	Test.Case("every field reads and writes through the proxy and reshapes the body", function()
		local crate = Support.Find("Crate")
		local box = crate.BoxCollider :: BoxCollider
		Test.ExpectEqual(Support.RoundTrip(box, "HalfExtents", vector.create(0.5, 0.25, 0.75)), vector.create(0.5, 0.5, 0.5))
		Test.ExpectEqual(Support.RoundTrip(box, "Offset", vector.create(0, 0.25, 0)), vector.zero)
		local rotation = Quat.AngleAxis(30, vector.create(0, 1, 0))
		local previous = box.Rotation
		box.Rotation = rotation
		Support.ExpectQuatNear(box.Rotation, rotation, 1e-6, "Rotation reads back what was written")
		Support.ExpectQuatNear(previous, Quat.Identity(), 0, "the default rotation")
		box.Rotation = Quat.Identity()
		Test.ExpectEqual(Support.RoundTrip(box, "IsTrigger", true), false)
		box.IsTrigger = false
		Test.WaitTicks(1)
		local minimum, maximum = Physics.GetColliderBounds(crate)
		Test.Expect(minimum ~= nil and maximum ~= nil, "the reshaped collider has bounds")
		local size = (maximum :: vector) - (minimum :: vector)
		Test.ExpectNear(size, vector.create(1, 0.5, 1.5), 0.01, "the body uses the new half extents")
		Test.Expect(not pcall(function()
			box.HalfExtents = vector.create(0, 1, 1)
		end), "half extents below 1 mm are refused")
	end)
end)
