--!strict
-- StaticScenery.scene (M43; Docs/Decisions/0061-m43-static-scenery.md decision 3; StaticScenery.jsonl, IdSeed 4001): a
-- Static root (Scope Subtree) over a floor, six rocks (each a static collider whose mesh is on a child) and a lift the
-- Director raises; a windmill under it opting out (NotStatic), whose blade stays dynamic; and a beacon static on its own
-- (EntityOnly), whose lamp stays dynamic. Every run mode reads Scope through the Entity.Static shortcut and writes it
-- through the proxy, and checks the readout: a quiet tick recomputes no world matrix, visits only the holders of the
-- per-tick passes and rebuilds no static cell, and raising the lift recomputes its matrix alone and rebuilds its one cell,
-- the lift's pose, its extracted draw and a raycast's hit on its collider following it. Where the game view renders, its
-- camera sees all four static cells. Each case starts afresh.
local Support = require("../Api/Support")

type Director = { LiftTicks: number, Lifts: number, Pushes: number }

-- The floor, the six rocks' shapes, the lift and the beacon; in four cells of 32 m.
local StaticItems = 9
local StaticCells = 4

local function Director(): Director
	return Support.Find("Director"):GetScript() :: any
end

local function Scope(name: string): StaticScope
	return (Support.Find(name).Static :: Static).Scope
end

-- The readout once nothing moves: the Ball asleep, then a few more ticks, so the last extracted frame changed nothing.
local function Quiet(): PerformanceStats
	local ball = Support.Find("Ball").RigidBody :: RigidBody
	Test.WaitUntil(function(): boolean
		return ball:IsSleeping()
	end, 600)
	Test.WaitTicks(4)
	return Application.GetStats()
end

return Test.Suite("StaticScenery", function()
	Test.Case("the authored scopes read back through the Entity.Static shortcut", function()
		Test.ExpectEqual(Scope("Scenery"), "Subtree", "the scenery root covers its subtree")
		Test.ExpectEqual(Scope("Windmill"), "NotStatic", "the windmill opts out")
		Test.ExpectEqual(Scope("Beacon"), "EntityOnly", "the beacon is static alone")
		Test.ExpectEqual(Support.Find("Rock0").Static, nil, "a rock holds no Static of its own")
		Test.ExpectEqual(Quiet().StaticItems, StaticItems, "the static set holds the scenery's meshes and the beacon's")
		-- The last extraction draws the static beacon where it stands (its item in the static set, not the mesh list).
		local beacon = Support.Find("Beacon")
		Test.ExpectNear(Test.GetExtractedPosition(beacon) :: vector, beacon.Transform.WorldPosition, 1e-5)
		Test.Screenshot("StaticScenery")
	end)

	Test.Case("Scope reads and writes through the proxy, and the static set follows it", function()
		local beacon = Support.Find("Beacon").Static :: Static
		Support.RoundTrip(beacon, "Scope", "Subtree")
		Test.ExpectEqual(Quiet().StaticItems, StaticItems + 1, "the beacon's lamp joins the static set")
		Support.RoundTrip(beacon, "Scope", "NotStatic")
		Test.ExpectEqual(Quiet().StaticItems, StaticItems - 1, "the beacon leaves it")
		Support.RoundTrip(beacon, "Scope", "EntityOnly")
		Test.ExpectEqual(Quiet().StaticItems, StaticItems, "the beacon alone again")
	end)

	Test.Case("a quiet tick recomputes no matrix, and raising the lift rebuilds its one cell", function()
		local quiet = Quiet()
		Test.ExpectEqual(quiet.TransformsUpdated, 0, "a quiet tick recomputes no world matrix")
		-- A quiet tick still visits the holders of the per-tick passes, the same 17 in every mode: the Ball's body owner in
		-- the physics write-back (asleep or not), and the Director twice (the Script view and the lifecycle order) in each of
		-- the tick's script instance synchronizations. The static scenery adds none.
		Test.ExpectEqual(quiet.EntitiesVisited, 17, "a quiet tick visits the Ball's owner and the Director alone")
		Test.ExpectEqual(quiet.StaticCellRebuilds, 0, "its frame phase rebuilds no static cell")
		if quiet.RenderAvailable then
			Test.ExpectEqual(quiet.StaticCellsVisible, StaticCells, "the game view sees every static cell")
		else
			Test.ExpectEqual(quiet.StaticCellsVisible, 0, "no view culls without a renderer")
		end

		local lift = Support.Find("Lift")
		local before = lift.Transform.Translation
		Director().LiftTicks = 1
		-- The counters are the last tick's (StaticCellRebuilds its frame phase's extraction's, never an editor view's): the
		-- largest each reports over the next ticks is the raise's.
		local updated = 0
		local visited = 0
		local rebuilt = 0
		for _ = 1, 4 do
			Test.WaitTicks(1)
			local stats = Application.GetStats()
			updated = math.max(updated, stats.TransformsUpdated)
			visited = math.max(visited, stats.EntitiesVisited)
			rebuilt = math.max(rebuilt, stats.StaticCellRebuilds)
		end
		Test.ExpectEqual(Director().Lifts, 1, "the director raised the lift once")
		Test.ExpectEqual(updated, 1, "the raise recomputes the lift's world matrix alone")
		Test.Expect(visited > quiet.EntitiesVisited, "the physics pre-step also visits the lift's body owner, whose transform changed")
		Test.ExpectEqual(rebuilt, 1, "and rebuilds the one cell that holds it")
		Test.ExpectNear(Test.GetExtractedPosition(lift) :: vector, lift.Transform.WorldPosition, 1e-5)
		Test.Expect(vector.magnitude(lift.Transform.Translation - (before + vector.create(0, 0.25, 0))) < 1e-5, "the lift rose a quarter metre")
		local hit = Physics.Raycast(vector.create(4, 10, 0), vector.create(0, -1, 0), 20)
		Test.Expect(hit ~= nil, "a ray down onto the lift hits")
		if hit then
			Test.ExpectEqual(hit.Entity.Name, "Lift", "the lift's collider, at its new pose")
			Test.ExpectNear(hit.Point.y, before.y + 0.25 + 0.5, 1e-3)
		end
		Test.ExpectEqual(Quiet().StaticCellRebuilds, 0, "the ticks after it rebuild nothing")
	end)
end)
