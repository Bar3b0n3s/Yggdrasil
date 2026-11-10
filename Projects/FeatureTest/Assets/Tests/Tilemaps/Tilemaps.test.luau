--!strict
-- Tilemaps.scene (Docs/Decisions/0032-m21-contract.md decisions 23 to 25 and 32): the tilemap renderer, the tilemap
-- colliders and a Field.Asset("Tilemap") property keep their authored values in every run mode; the Level's solid tiles
-- collide as boxes merged within each chunk of 4 x 4 cells, so the ball rests on the ground and rolls across the seams
-- between the chunks' boxes without a bump, while its grass tiles, which are not Solid, let rays through; the Goal's
-- trigger tiles report the ball entering and leaving them.
local GoalZoneScript = require("../../Scripts/Tilemaps/GoalZone")
local LevelProbeScript = require("../../Scripts/Tilemaps/LevelProbe")
local Support = require("../Api/Support")

local DOWN = vector.create(0, -1, 0)

return Test.Suite("Tilemaps", function()
	Test.Case("the authored tilemap components keep their values", function()
		local level = Support.Find("Level")
		local tilemap = Assets.Load("Assets/Tilemaps/Level.tilemap")
		local renderer = level.TilemapRenderer :: TilemapRenderer
		Test.ExpectEqual(renderer.Tilemap, tilemap)
		Support.ExpectColorNear(renderer.Color, Color.New(0.95, 0.9, 0.8, 1), 1e-6, "the level's tint")
		Test.ExpectEqual(renderer.PixelsPerUnit, 8)
		Test.ExpectEqual(renderer.SortingLayer, "Background")
		Test.ExpectEqual(renderer.OrderInLayer, -5)
		Test.ExpectEqual(renderer.Lit, true)
		local collider = level.TilemapCollider :: TilemapCollider
		Test.ExpectEqual(collider.Tilemap, nil, "a null Tilemap takes the TilemapRenderer's")
		Test.ExpectEqual(collider.IsTrigger, false)
		local goal = Support.Find("Goal").TilemapCollider :: TilemapCollider
		Test.ExpectEqual(goal.Tilemap, Assets.Load("Assets/Tilemaps/Goal.tilemap"))
		Test.ExpectEqual(goal.IsTrigger, true)
		local probe = level:GetScript("LevelProbe") :: LevelProbeScript.LevelProbe
		Test.ExpectEqual(probe.Level, tilemap, "a Tilemap-typed script property resolves to its tilemap")
	end)

	Test.Case("the ball rests on the ground and rolls across its boxes' seams without a bump", function()
		local ball = Support.Find("Ball")
		local body = ball.RigidBody2D :: RigidBody2D
		Test.WaitTicks(16)
		Test.ExpectNear(ball.Transform.WorldPosition.y, 0.4, 0.01, "the ball rests on the ground row's top")
		-- Rolling at 5 units per second: the angular velocity of a disc of radius 0.4 that does not slide.
		body:SetLinearVelocity(vector.create(5, 0, 0))
		body:SetAngularVelocity(-5 / 0.4)
		local lowest, highest = math.huge, -math.huge
		-- From x = -6 across the seams at x = -4 and x = 0, short of the ledge at x = 4.
		for _ = 1, 96 do
			Test.WaitTicks(1)
			local y = ball.Transform.WorldPosition.y
			lowest = math.min(lowest, y)
			highest = math.max(highest, y)
		end
		Test.Expect(ball.Transform.WorldPosition.x > 0.5, "the ball rolled past the seam at x = 0")
		Test.ExpectNear(lowest, 0.4, 0.01, "no seam lets the ball sink")
		Test.ExpectNear(highest, 0.4, 0.01, "no seam bumps the ball up")
	end)

	Test.Case("rays hit the solid tiles and pass the grass", function()
		Test.WaitTicks(1) -- bodies appear at the first step's pre-step
		local level = Support.Find("Level")
		-- The grass tile of cell (-2, 0) is not Solid: a ray down through it reaches the ground row's top.
		local ground = Physics2D.Raycast(vector.create(-1.5, 5, 0), DOWN, 20)
		Test.Expect(ground ~= nil, "the ray down through the grass hits")
		Test.ExpectEqual((ground :: RaycastHit).Entity, level)
		Test.ExpectNear((ground :: RaycastHit).Point.y, 0, 1e-3, "through the grass to the ground")
		local ledge = Physics2D.Raycast(vector.create(5.5, 5, 0), DOWN, 20)
		Test.Expect(ledge ~= nil, "the ray down onto the ledge hits")
		Test.ExpectEqual((ledge :: RaycastHit).Entity, level)
		Test.ExpectNear((ledge :: RaycastHit).Point.y, 1, 1e-3, "the ledge's top is one row up")
		local found = Physics2D.OverlapCircle(vector.create(9, 2, 0), 0.25)
		Test.Expect(table.find(found, Support.Find("Goal")) ~= nil, "the goal's trigger tiles overlap their cells")
		Test.ExpectEqual(#Physics2D.OverlapCircle(vector.create(-6, 6, 0), 0.25), 0, "nothing in the empty cells above")
	end)

	Test.Case("the ball entering the goal's trigger tiles is reported", function()
		local ball = Support.Find("Ball")
		local body = ball.RigidBody2D :: RigidBody2D
		local goal = Support.Find("Goal"):GetScript("GoalZone") :: GoalZoneScript.GoalZone
		Test.WaitTicks(1)
		Test.ExpectEqual(goal.Entered, 0)
		body:Teleport(vector.create(9, 2, 0))
		body:SetLinearVelocity(vector.zero)
		Test.WaitTicks(2)
		Test.ExpectEqual(goal.Entered, 1, "the goal saw the ball enter")
		Test.ExpectEqual(goal.Last, "Ball")
		body:Teleport(vector.create(-6, 0.4, 0))
		Test.WaitTicks(2)
		Test.ExpectEqual(goal.Left, 1, "and leave")
	end)
end)
