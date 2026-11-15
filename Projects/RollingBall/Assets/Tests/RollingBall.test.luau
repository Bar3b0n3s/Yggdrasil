--!strict
-- Rolling Ball's integration suite (ADR 0038 decision 10): the main-path support check and the HUD's level and par on
-- every level, the respawn at the last checkpoint, the CHECKPOINT banner, the goal and the timer carried across
-- Scene.Load, the lift carrying the ball, a platform's phased start, and Restart. The cases share one session and cross
-- the levels by Scene.Load: the case during which a transition is requested returns in that frame, and the next case
-- runs in the new scene's VM.
local Actions = require("../Scripts/Actions")
local BallScript = require("../Scripts/Ball")
local Clock = require("../Scripts/Clock")
local GameScript = require("../Scripts/Game")
local Motion = require("../Scripts/Motion")
local PlatformScript = require("../Scripts/MovingPlatform")
local Track = require("../Scripts/Track")

local Down = vector.create(0, -1, 0)
local Lift = vector.create(0, 0.5, 0)

local function byName(a: Entity, b: Entity): boolean
	return a.Name < b.Name
end

local function find(path: string): Entity
	local entity = Scene.FindByPath(path)
	if not entity then
		Test.Fail("the scene has no " .. path)
	end
	return entity :: Entity
end

local function ballParts(): (Entity, BallScript.Ball, RigidBody)
	local entity = find("/Ball")
	return entity, entity:GetScript("Ball") :: BallScript.Ball, entity.RigidBody :: RigidBody
end

local function game(): GameScript.Game
	return find("/Game"):GetScript("Game") :: GameScript.Game
end

local function hudText(name: string): string
	local element = find("/HUD"):FindChild(name, true)
	local label = if element then element.UiText else nil
	return if label then label.Text else ""
end

-- The HUD's level and par, which the game writes once at the level's start.
local function expectHud(level: number)
	Test.ExpectEqual(hudText("LevelText"), string.format("LEVEL %d/3", level), "the HUD names the level")
	Test.ExpectEqual(hudText("ParText"), "PAR " .. Clock.Format(game().ParTicks), "the HUD shows the level's par")
end

-- Places the ball at rest (a test case may move it; recording suites never do).
local function place(body: RigidBody, position: vector)
	body:Teleport(position, Quat.Identity())
	body:SetLinearVelocity(vector.zero)
	body:SetAngularVelocity(vector.zero)
end

-- Presses Restart as a player would, and waits for the game to request its transition (it applies at the frame's
-- end, so the case returns in that frame and the next case runs in the scene it loads).
local function restart(current: GameScript.Game)
	Test.InjectAction(Actions.Restart, "Tap")
	Test.WaitUntil(function()
		return current.Leaving
	end, 5)
	Test.Expect(not current.LoadRequested, "a restart is not the next level's load")
end

-- The piece of the level a collider entity belongs to: its ancestor just below /Level.
local function pieceOf(entity: Entity): string
	local current = entity
	local parent = current:GetParent()
	while parent and parent.Name ~= "Level" do
		current = parent
		parent = current:GetParent()
	end
	return current.Name
end

-- The solid collider nearest to a point by its collider bounds, as "<collider> of <piece>". Trigger volumes (the
-- checkpoints' and the goal's) are left out: a miss inside one would otherwise name the trigger, not the track.
local function nearestPiece(point: vector): string
	local best = "none"
	local bestDistance = math.huge
	for _, entity in Scene.FindAllWithComponent("BoxCollider") do
		local collider = entity.BoxCollider
		if collider and not collider.IsTrigger then
			local low, high = Physics.GetColliderBounds(entity)
			if low and high then
				local closest = vector.create(math.clamp(point.x, low.x, high.x), math.clamp(point.y, low.y, high.y), math.clamp(point.z, low.z, high.z))
				local distance = vector.magnitude(closest - point)
				if distance < bestDistance then
					bestDistance = distance
					best = string.format("%s of %s (%.2f m away)", entity.Name, pieceOf(entity), distance)
				end
			end
		end
	end
	return best
end

-- The main-path support check: every 0.25 m of the PathNode polyline, starting half a step in, outside every Gap
-- marker, has the Track layer below it.
local function supportCheck(minimumSamples: number)
	local nodes = Scene.FindAllByTag("PathNode")
	table.sort(nodes, byName)
	local points: { vector } = {}
	for index, node in nodes do
		points[index] = node.Transform.WorldPosition
	end
	local markers = Scene.FindAllByTag("Gap")
	local samples, distances = Track.Sample(points, 0.25, 0.125)
	Test.Expect(#samples >= minimumSamples, string.format("%d path samples, at least %d expected", #samples, minimumSamples))
	local mask = Physics.LayerMask("Track")
	local misses = 0
	for index, sample in samples do
		local inGap = false
		for _, marker in markers do
			if Track.InsideBox(marker.Transform:InverseTransformPoint(sample)) then
				inGap = true
				break
			end
		end
		if not inGap and not Physics.Raycast(sample + Lift, Down, 3, mask) then
			misses += 1
			if misses <= 8 then
				Test.Expect(false, string.format("no Track below the path at %.3f m, (%.3f, %.3f, %.3f); nearest: %s", distances[index], sample.x, sample.y, sample.z, nearestPiece(sample)))
			end
		end
	end
	Test.ExpectEqual(misses, 0, "path samples without support")
end

return Test.Suite("RollingBall", function()
	Test.Case("Level 1: the main path is supported everywhere but inside its gaps", function()
		supportCheck(400)
	end)

	Test.Case("Level 1: the HUD shows the level and its par", function()
		expectHud(1)
	end)

	Test.Case("Level 1: a fall respawns the ball at the last checkpoint with no velocity", function()
		local ballEntity, ball, body = ballParts()
		local checkpoint = find("/Level/Checkpoint1")
		local root = checkpoint.Transform.WorldPosition
		place(body, root + vector.create(0, 1, 0))
		Test.WaitUntil(function()
			return ball.LastOrder == 1
		end, 30)
		local bar = checkpoint:FindChild("Bar") :: Entity
		local graph = checkpoint:GetScript("Checkpoint")
		Test.Expect(graph.Activated == true, "the checkpoint's graph is activated")
		Test.ExpectEqual((bar.MeshRenderer :: MeshRenderer):GetMaterial(1), graph.ActiveMaterial, "the bar shows the active material")
		local timerBefore = game().TotalTicks
		-- Over the void beside the track.
		place(body, root + vector.create(0, 1, 8))
		Test.WaitUntil(function()
			return ball.Respawns == 1
		end, 240)
		local velocity = body:GetLinearVelocity()
		Test.ExpectNear(ballEntity.Transform.WorldPosition, root + vector.create(0, 0.6, 0), 0.05, "the ball respawned at the checkpoint")
		Test.Expect(vector.magnitude(velocity) < 0.01, string.format("linear speed %.4f after the respawn", vector.magnitude(velocity)))
		Test.Expect(vector.magnitude(body:GetAngularVelocity()) < 0.01, "no angular velocity after the respawn")
		Test.ExpectNear(ball.Heading, Motion.Wrap(checkpoint.Transform.EulerAngles.y), 0.001, "the heading is the checkpoint's yaw")
		Test.Expect(game().TotalTicks > timerBefore, "the timer kept running")
	end)

	Test.Case("Level 1: rolling back through an earlier checkpoint never moves the spawn backwards", function()
		local ballEntity, ball, body = ballParts()
		local first = find("/Level/Checkpoint1").Transform.WorldPosition
		local second = find("/Level/Checkpoint2")
		local secondRoot = second.Transform.WorldPosition
		place(body, secondRoot + vector.create(0, 1, 0))
		Test.WaitUntil(function()
			return ball.LastOrder == 2
		end, 30)
		-- Back through Checkpoint1: the ball enters its trigger again and rests inside it.
		local respawns = ball.Respawns
		place(body, first + vector.create(0, 1, 0))
		Test.WaitTicks(20)
		Test.ExpectEqual(ball.LastOrder, 2, "an earlier checkpoint keeps the last order")
		-- Then a fall beside Checkpoint1.
		place(body, first + vector.create(0, 1, 8))
		Test.WaitUntil(function()
			return ball.Respawns == respawns + 1
		end, 240)
		Test.ExpectNear(ballEntity.Transform.WorldPosition, secondRoot + vector.create(0, 0.6, 0), 0.05, "the ball respawned at Checkpoint2")
		Test.ExpectNear(ball.Heading, Motion.Wrap(second.Transform.EulerAngles.y), 0.001, "the heading is Checkpoint2's yaw")
	end)

	Test.Case("Level 1: the goal stops the timer and loads Level 2 with it", function()
		local _, ball, body = ballParts()
		local goal = find("/Level/Goal").Transform.WorldPosition
		place(body, goal + vector.create(0, 0.6, 0))
		Test.WaitUntil(function()
			return ball.ReachedGoal
		end, 30)
		local current = game()
		Test.WaitUntil(function()
			return current.Finished
		end, 5)
		local stopped = current.TotalTicks
		Test.WaitTicks(10)
		Test.ExpectEqual(current.TotalTicks, stopped, "the timer stopped")
		Test.Expect(string.sub(hudText("BannerText"), 1, 14) == "LEVEL COMPLETE", "the banner reads LEVEL COMPLETE: " .. hudText("BannerText"))
		Test.WaitUntil(function()
			return current.LoadRequested
		end, current.LoadDelayTicks + 10)
		Test.Expect(current.Leaving, "the load is the level's one transition")
		Test.ExpectEqual(current.SentParameters.ElapsedTicks, stopped, "Level 2 is loaded with the stopped time")
	end)

	Test.Case("Level 2: the timer carried across Scene.Load continues", function()
		Test.ExpectEqual(Scene.GetName(), "Level2")
		local carried = Scene.GetLoadParameters().ElapsedTicks
		local current = game()
		Test.Expect(type(carried) == "number" and carried > 0, "Level 2 was loaded with the elapsed ticks")
		Test.ExpectEqual(current.StartTicks, carried, "the game starts from the carried time")
		local before = current.TotalTicks
		Test.WaitTicks(60)
		Test.ExpectEqual(current.TotalTicks, before + 60, "the timer counts on")
		Test.ExpectEqual(hudText("TimeText"), Clock.Format(current.TotalTicks), "the HUD shows the run's time")
	end)

	Test.Case("Level 2: the HUD shows the level and its par", function()
		expectHud(2)
	end)

	Test.Case("Level 2: the lift carries the ball up to the upper deck", function()
		local ballEntity, _, body = ballParts()
		local lift = find("/Level/Lift")
		local platform = lift:GetScript("MovingPlatform") :: PlatformScript.MovingPlatform
		Test.WaitUntil(function()
			return platform.AtStart and platform.DwellLeft > 45
		end, 700)
		-- The lift's root is its box's centre, 0.25 m below its top: the ball is placed 0.1 m above the top and
		-- measured once it has settled on it.
		place(body, lift.Transform.WorldPosition + vector.create(0, 0.85, 0))
		Test.WaitTicks(20)
		local start = ballEntity.Transform.WorldPosition
		Test.WaitUntil(function()
			return platform.AtEnd
		end, 700)
		local finish = ballEntity.Transform.WorldPosition
		Test.ExpectNear(finish.y - start.y, 4, 0.1, "the ball rose with the lift")
		local low, high = Physics.GetColliderBounds(lift)
		Test.Expect(low ~= nil and high ~= nil, "the lift has collider bounds")
		if low and high then
			Test.Expect(finish.x >= low.x and finish.x <= high.x and finish.z >= low.z and finish.z <= high.z, "the ball is still on the lift")
		end
	end, { TimeoutTicks = 1200 })

	Test.Case("Level 2: a platform started part-way through its cycle moves on from its phased pose", function()
		local prefab = Assets.Load("Assets/Prefabs/MovingPlatform.prefab")
		Test.Expect(prefab ~= nil, "the MovingPlatform prefab loads")
		if not prefab then
			return
		end
		-- Away from the track and the ball; the authored pose is the cycle's start pose. The phase, halfway up the
		-- rise, is set before the instance's first tick, so that its OnStart reads it.
		local origin = vector.create(0, 20, 40)
		local entity = Scene.Instantiate(prefab, origin)
		local platform = entity:GetScript("MovingPlatform") :: PlatformScript.MovingPlatform
		local body = entity.RigidBody :: RigidBody
		platform.PhaseTicks = platform.DwellTicks + platform.MoveTicks // 2
		local phase = platform.PhaseTicks
		local function ease(tick: number): number
			return Motion.Ease(tick, platform.MoveTicks, platform.DwellTicks)
		end
		for step = 1, 2 do
			Test.WaitTicks(1)
			Test.ExpectEqual(platform.Tick, phase + step, "the platform counts on from its phase")
			local expected = origin + platform.Travel * ease(phase + step)
			Test.ExpectNear(entity.Transform.WorldPosition, expected, 0.001, string.format("the platform's pose %d ticks on from its phase", step))
		end
		-- A Kinematic body's velocity is its last physics step's sweep, and a test case runs before its tick's step:
		-- two ticks after the creation it is the sweep of the platform's first tick. From the phased pose that sweep is
		-- one tick's step; from the authored pose (the platform not placed at its phase) it would be the jump there.
		local perSecond = 1 / Time.GetFixedDeltaTime()
		local travel = vector.magnitude(platform.Travel)
		local stepSpeed = travel * (ease(phase + 1) - ease(phase)) * perSecond
		local jumpSpeed = travel * ease(phase + 1) * perSecond
		local speed = vector.magnitude(body:GetLinearVelocity())
		Test.Expect(speed < 10, string.format("the first tick's sweep moved the platform at %.3f m/s (one tick's step %.3f m/s, the jump from the authored pose %.1f m/s)", speed, stepSpeed, jumpSpeed))
		Scene.Destroy(entity)
	end)

	Test.Case("Level 2: the main path is supported everywhere but inside its gaps", function()
		supportCheck(400)
	end)

	Test.Case("Level 2: Restart reloads the level", function()
		local current = game()
		Test.Expect(current.Ticks > 60, "the level has run before the restart")
		restart(current)
	end)

	Test.Case("Level 2: the restarted level starts again from the time it was entered with", function()
		Test.ExpectEqual(Scene.GetName(), "Level2")
		local carried = Scene.GetLoadParameters().ElapsedTicks
		local current = game()
		local ballEntity, ball = ballParts()
		Test.Expect(type(carried) == "number" and carried > 0, "the reload keeps the elapsed ticks Level 2 was loaded with")
		Test.ExpectEqual(current.StartTicks, carried, "the timer starts again from the carried time")
		Test.Expect(current.Ticks < 10, string.format("the level's own time starts again (%d ticks)", current.Ticks))
		Test.ExpectEqual(current.TotalTicks, current.StartTicks + current.Ticks, "the run's time is the carried time plus the level's")
		Test.ExpectEqual(ball.Respawns, 0, "the ball starts afresh")
		Test.ExpectEqual(ball.LastOrder, 0, "no checkpoint is touched")
		Test.ExpectNear(ballEntity.Transform.WorldPosition, ball.Spawn, 0.15, "the ball is back at the level's start")
	end)

	Test.Case("Level 2: its next level is Level 3, which the suite loads with a carried time", function()
		local nextLevel = game().NextLevel
		Test.ExpectEqual(nextLevel, Assets.Load("Assets/Scenes/Level3.scene"), "Level 2's next level is Level 3")
		if nextLevel then
			Scene.Load(nextLevel, { ElapsedTicks = 3600 })
		end
	end)

	Test.Case("Level 3: the suite's load arrives with its carried time", function()
		Test.ExpectEqual(Scene.GetName(), "Level3")
		Test.ExpectEqual(Scene.GetLoadParameters().ElapsedTicks, 3600, "Level 3 was loaded with the suite's time")
		Test.ExpectEqual(game().StartTicks, 3600, "the game starts from the carried time")
	end)

	Test.Case("Level 3: the HUD shows the level and its par", function()
		expectHud(3)
	end)

	Test.Case("Level 3: the main path is supported everywhere but inside its gaps", function()
		supportCheck(500)
	end)

	Test.Case("Level 3: the goal shows the finish with the total time", function()
		local _, ball, body = ballParts()
		local goal = find("/Level/Goal").Transform.WorldPosition
		place(body, goal + vector.create(0, 0.6, 0))
		Test.WaitUntil(function()
			return ball.ReachedGoal
		end, 30)
		local current = game()
		Test.WaitUntil(function()
			return current.Finished
		end, 5)
		Test.ExpectEqual(hudText("BannerText"), "FINISHED  " .. Clock.Format(current.TotalTicks) .. "\nPRESS R TO PLAY AGAIN", "the banner shows the run's time")
		Test.WaitTicks(current.LoadDelayTicks + 10)
		Test.Expect(not current.LoadRequested and not current.Leaving, "the last level loads nothing")
	end, { TimeoutTicks = 300 })

	Test.Case("Level 3: Restart after the finish starts a new run", function()
		local current = game()
		Test.Expect(current.Finished, "Level 3 is finished")
		restart(current)
	end)

	Test.Case("Level 1: the new run starts on Level 1 with no time", function()
		Test.ExpectEqual(Scene.GetName(), "Level1")
		Test.ExpectEqual(Scene.GetLoadParameters().ElapsedTicks, nil, "Level 1 is loaded without a carried time")
		local current = game()
		Test.ExpectEqual(current.StartTicks, 0, "the new run's timer starts at 0")
		Test.ExpectEqual(current.TotalTicks, current.Ticks, "the run's time is Level 1's own")
	end)

	Test.Case("Level 1: a new checkpoint shows the CHECKPOINT banner for BannerTicks ticks", function()
		local _, ball, body = ballParts()
		local current = game()
		local banner = find("/HUD/Banner")
		Test.Expect(not banner:IsActive(), "no banner shows before the checkpoint")
		place(body, find("/Level/Checkpoint1").Transform.WorldPosition + vector.create(0, 1, 0))
		Test.WaitUntil(function()
			return ball.LastOrder == 1
		end, 30)
		-- The ball enters the trigger in a tick's physics step, after that tick's fixed step and this case; the game
		-- sees the new order in the next tick's fixed step and shows the banner there, before this case resumes in that
		-- tick. So the banner shows from the tick in which the case first sees the order.
		local shown = Time.GetTick()
		Test.Expect(banner:IsActive(), "the banner shows in the tick the order rises")
		Test.ExpectEqual(hudText("BannerText"), "CHECKPOINT", "the banner reads CHECKPOINT")
		-- The first tick without the banner, counted from the tick it showed in, is the number of ticks it showed for:
		-- shown in its tick and BannerTicks - 1 more, gone one tick later. With the countdown run after showBanner in the
		-- tick that shows it (S2's order), it would be one fewer.
		Test.WaitUntil(function()
			return not banner:IsActive()
		end, current.BannerTicks + 10)
		Test.ExpectEqual(Time.GetTick() - shown, current.BannerTicks, string.format("the banner shown in tick %d shows for BannerTicks ticks", shown))
	end)
end)
