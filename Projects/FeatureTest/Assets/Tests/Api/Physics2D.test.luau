--!strict
-- The Physics2D module (Docs/Decisions/0032-m21-contract.md decision 13): rays and overlaps in the plane with hit
-- identities, layer masks and the session's 2D gravity, which starts at the project's [0, -12].
local Support = require("./Support")

return Test.Suite("Api.Physics2D", function()
	Test.Case("rays find the crate on the floor with its identity and distance", function()
		Test.WaitTicks(1) -- bodies appear at the first step's pre-step
		local crate = Support.Find("Crate")
		local floor = Support.Find("Floor")
		local origin, down = vector.create(-2, 5, 3), vector.create(0, -1, 0)
		local hit = Physics2D.Raycast(origin, down, 20)
		Test.Expect(hit ~= nil, "the ray hits")
		local first = hit :: RaycastHit
		Test.ExpectEqual(first.Entity, crate)
		Test.ExpectEqual(first.Body, crate)
		Test.ExpectNear(first.Distance, 4.2, 0.02, "the crate's top is at 0.8")
		Test.ExpectNear(first.Point, vector.create(-2, 0.8, 0), 0.02, "the point lies in the plane")
		Test.ExpectNear(first.Normal, vector.create(0, 1, 0), 0.001)
		local all = Physics2D.RaycastAll(origin, down, 20)
		Test.ExpectEqual(#all, 2, "the crate and the floor")
		Test.ExpectEqual(all[1].Entity, crate)
		Test.ExpectEqual(all[2].Entity, floor)
		Test.Expect(all[1].Distance < all[2].Distance, "sorted by distance")
		Test.ExpectEqual(Physics2D.Raycast(origin, vector.create(0, 1, 0), 20), nil, "nothing above")
	end)

	Test.Case("layer masks select what queries see", function()
		local origin, down = vector.create(-2, 5, 0), vector.create(0, -1, 0)
		Test.Expect(Physics2D.Raycast(origin, down, 20, Physics.LayerMask("Default")) ~= nil, "the crate is on Default")
		local track = Physics2D.Raycast(origin, down, 20, Physics.LayerMask("Track"))
		Test.Expect(track ~= nil and (track :: RaycastHit).Entity == Support.Find("Floor"), "the floor is on Track")
		Test.ExpectEqual(Physics2D.Raycast(origin, down, 20, Physics.LayerMask("Debris")), nil, "nothing on Debris")
		Test.ExpectEqual(#Physics2D.RaycastAll(origin, down, 20, 0), 0)
	end)

	Test.Case("overlaps list colliders in UUID order", function()
		local crate = Support.Find("Crate")
		local around = Physics2D.OverlapCircle(vector.create(-2, 0.4, 0), 0.3)
		Test.Expect(table.find(around, crate) ~= nil, "the crate overlaps its own centre")
		local box = Physics2D.OverlapBox(vector.create(-2, 0, 0), vector.create(0.7, 0.7, 0), 45)
		Test.Expect(#box >= 2, "the crate and the floor")
		for index = 2, #box do
			Test.Expect(box[index - 1].ID < box[index].ID, "sorted by UUID")
		end
		Test.ExpectEqual(#Physics2D.OverlapBox(vector.create(0, 50, 0), vector.one), 0, "nothing high above")
		Test.ExpectEqual(#Physics2D.OverlapCircle(vector.create(-2, 0.4, 0), 0.3, Physics.LayerMask("Debris")), 0)
	end)

	Test.Case("gravity starts at the project setting and a script can change it", function()
		Test.ExpectEqual(Physics2D.GetGravity(), vector.create(0, -12, 0))
		Physics2D.SetGravity(vector.create(0, -6, 5))
		Test.ExpectEqual(Physics2D.GetGravity(), vector.create(0, -6, 0), "Z is ignored")
		local ball = Support.Find("Ball")
		local body = ball.RigidBody2D :: RigidBody2D
		body:Teleport(vector.create(0, 10, 0))
		body:SetLinearVelocity(vector.zero)
		Test.WaitTicks(1)
		local before = body:GetLinearVelocity().y
		Test.WaitTicks(4)
		Test.ExpectNear(body:GetLinearVelocity().y - before, -6 * 4 / 64, 0.02, "the ball falls at the new gravity")
		Test.Expect(not pcall(Physics2D.SetGravity, vector.create(0, math.huge, 0)), "infinite gravity is refused")
	end)
end)
