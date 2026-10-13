--!strict
-- The CharacterController proxy (§9.6, §11.5): a character settles on the ground, moves at the velocity it is given,
-- and every field reads and writes.
local Support = require("./Support")

return Test.Suite("Api.CharacterController", function()
	Test.Case("a character stands on the ground and walks at the requested velocity", function()
		local hero = Support.Find("Hero")
		local character = hero.CharacterController :: CharacterController
		Test.WaitUntil(function(): boolean
			return character:IsGrounded()
		end, 120)
		Test.ExpectNear(character:GetGroundNormal(), vector.create(0, 1, 0), 0.001, "flat ground")
		local start = hero.Transform.WorldPosition
		for _ = 1, 16 do
			character:Move(vector.create(2, 0, 0))
			Test.WaitTicks(1)
		end
		Test.ExpectNear(character:GetVelocity().x, 2, 0.01, "the character walks at 2 m/s")
		local moved = hero.Transform.WorldPosition - start
		Test.ExpectNear(moved.x, 2 * 16 / 64, 0.05, "16 steps at 2 m/s")
		Test.ExpectNear(moved.z, 0, 0.001)
		Test.Expect(character:IsGrounded(), "still on the ground")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local character = Support.Find("Hero").CharacterController :: CharacterController
		Test.ExpectNear(Support.RoundTrip(character, "Height", 1.6, 1e-6), 1.8, 1e-6, "the default height")
		Support.RoundTrip(character, "Radius", 0.35, 1e-6)
		Support.RoundTrip(character, "MaxSlopeAngle", 30, 1e-5)
		Support.RoundTrip(character, "StepHeight", 0.2, 1e-6)
		Support.RoundTrip(character, "Mass", 80, 1e-5)
		Support.RoundTrip(character, "GravityFactor", 0.5, 1e-6)
		Test.ExpectEqual(Support.RoundTrip(character, "Layer", "Character"), "Default")
	end)
end)
