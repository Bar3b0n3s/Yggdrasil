--!strict
-- The Quat type (§11.5): constructors, methods, the product operator, components and exact equality.
return Test.Suite("Api.Quat", function()
	Test.Case("constructors and components", function()
		local q = Quat.New(1, 2, 3, 4)
		Test.Expect(q.x == 1 and q.y == 2 and q.z == 3 and q.w == 4, "components read back")
		q.x = 2
		q.y = 3
		q.z = 4
		q.w = 5
		Test.Expect(q.x == 2 and q.y == 3 and q.z == 4 and q.w == 5, "components are writable")
		local identity = Quat.Identity()
		Test.Expect(identity.x == 0 and identity.y == 0 and identity.z == 0 and identity.w == 1, "identity")
		Test.Expect(not pcall(Quat.New, 0, 0, 0, 0), "a zero quaternion is refused")
	end)

	Test.Case("rotations compose, invert, interpolate and convert to Euler degrees", function()
		local quarter = Quat.AngleAxis(90, vector.create(0, 5, 0))
		local forward = vector.create(0, 0, -1)
		Test.ExpectNear(quarter:Rotate(forward), vector.create(-1, 0, 0), 1e-5)
		Test.ExpectNear(quarter * forward, vector.create(-1, 0, 0), 1e-5, "the vector product rotates")
		local half = quarter * quarter
		Test.ExpectNear(half * forward, vector.create(0, 0, 1), 1e-5, "the quaternion product composes")
		local product = quarter * quarter:Inverse()
		Test.ExpectNear(vector.create(product.x, product.y, product.z), vector.zero, 1e-6, "q times its inverse")
		Test.ExpectNear(product.w, 1, 1e-6)
		local scaled = Quat.New(0, 0, 0, 2)
		local unit = scaled:Normalized()
		Test.ExpectEqual(unit.w, 1)
		local euler = Quat.FromEuler(30, 40, 50)
		Test.ExpectNear(euler:ToEuler(), vector.create(30, 40, 50), 1e-4)
		local halfway = Quat.Slerp(Quat.Identity(), quarter, 0.5)
		Test.ExpectNear(halfway * forward, vector.create(-0.7071067811865475, 0, -0.7071067811865475), 1e-5)
		local look = Quat.LookRotation(vector.create(1, 0, 0))
		Test.ExpectNear(look * forward, vector.create(1, 0, 0), 1e-5)
		Test.ExpectNear(Quat.LookRotation(vector.create(0, 0, 1), vector.create(0, 1, 0)) * forward, vector.create(0, 0, 1), 1e-5)
	end)

	Test.Case("equality compares the four components exactly", function()
		Test.Expect(Quat.New(0, 0, 0, 1) == Quat.Identity(), "equal components")
		Test.Expect(Quat.New(0, 0, 0, -1) ~= Quat.Identity(), "q and -q differ")
	end)
end)
