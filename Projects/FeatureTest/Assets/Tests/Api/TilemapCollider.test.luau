--!strict
-- The TilemapCollider proxy (Docs/Decisions/0032-m21-contract.md decisions 25 and 32): every field reads and writes
-- through the proxy, and a write reshapes the 2D world: a ground made a trigger stops nothing, so the resting ball falls
-- through it.
local Support = require("./Support")

return Test.Suite("Api.TilemapCollider", function()
	Test.Case("every field reads and writes through the proxy", function()
		local collider = Support.Find("Goal").TilemapCollider :: TilemapCollider
		local level = Assets.Load("Assets/Tilemaps/Level.tilemap")
		local goal = Assets.Load("Assets/Tilemaps/Goal.tilemap")
		Test.ExpectEqual(Support.RoundTrip(collider, "Tilemap", level), goal)
		Test.ExpectEqual(Support.RoundTrip(collider, "Tilemap", goal), level)
		Test.ExpectEqual(Support.RoundTrip(collider, "IsTrigger", false), true)
		Test.ExpectEqual(Support.RoundTrip(collider, "IsTrigger", true), false)
	end)

	Test.Case("a ground collider made a trigger lets the resting ball fall through", function()
		local ball = Support.Find("Ball")
		Test.WaitTicks(8)
		Test.ExpectNear(ball.Transform.WorldPosition.y, 0.4, 0.01, "the ball rests on the ground")
		local collider = Support.Find("Level").TilemapCollider :: TilemapCollider
		collider.IsTrigger = true
		Test.WaitTicks(32)
		Test.Expect(ball.Transform.WorldPosition.y < -1, "the ball fell through the trigger tiles")
	end)
end)
