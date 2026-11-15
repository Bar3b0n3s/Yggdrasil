--!strict
-- The recording suites' autopilot (ADR 0038 decision 10): pure pursuit along the level's PathNode polyline, steering
-- the ball like a player through MoveX and MoveZ in the frame of the ball's Heading. It only reads the scene (the
-- ball, the path nodes and their tags, the platforms' phase) and injects input; it never moves an entity, writes a
-- component or draws from the session's Random.
local Actions = require("../../Scripts/Actions")
local BallScript = require("../../Scripts/Ball")
local GameScript = require("../../Scripts/Game")
local Motion = require("../../Scripts/Motion")
local PlatformScript = require("../../Scripts/MovingPlatform")
local Track = require("../../Scripts/Track")

local Autopilot = {}

-- The pursuit point lies this far ahead of the ball's projection on the path.
local Lookahead = 2.5
-- Wanted speeds (m/s): on plain track, on a Fast segment (a jump's run-up), on a Slow one (a beam) and in a curve.
local CruiseSpeed = 7.5
local FastSpeed = 11
local SlowSpeed = 4.5
local CurveSpeed = 6
-- A node where the path turns by more than this (degrees) is on a curve.
local CurveAngle = 5
-- The deceleration (m/s^2) the speed plan assumes ahead of a slower segment or a boarding point.
local Brake = 4
-- The steering per m/s of velocity error, and the wanted m/s per metre from a held point.
local Gain = 0.5
local HoldGain = 1.5
local HoldSpeed = 2.5
-- A boarding waits for a platform dwelling at its start with at least this many ticks of its dwell left.
local BoardDwell = 100
-- The projection searches this many segments behind and ahead of the last one.
local Window = 3
-- Inputs are quantized to 1/Quantum and injected only when they change.
local Quantum = 32

local Hints = { "Fast", "Slow", "Board" }

export type Route = {
	Points: { vector },
	Lengths: { number },
	Hints: { string },
	Limits: { number },
}

local function byName(a: Entity, b: Entity): boolean
	return a.Name < b.Name
end

local function flat(value: vector): vector
	return vector.create(value.x, 0, value.z)
end

local function clampLength(value: vector, maximum: number): vector
	local length = vector.magnitude(value)
	if length > maximum then
		return value * (maximum / length)
	end
	return value
end

-- The level's route: its path nodes in name order, each with its hint and its segment's speed limit.
function Autopilot.Route(): Route
	local nodes = Scene.FindAllByTag("PathNode")
	table.sort(nodes, byName)
	local points: { vector } = {}
	local hints: { string } = {}
	for index, node in nodes do
		points[index] = node.Transform.WorldPosition
		hints[index] = ""
		for _, hint in Hints do
			if node:HasTag(hint) then
				hints[index] = hint
			end
		end
	end
	local turns: { number } = {}
	for index = 1, #points do
		turns[index] = 0
		if index > 1 and index < #points then
			local before = flat(points[index] - points[index - 1])
			local after = flat(points[index + 1] - points[index])
			if vector.magnitude(before) > 0 and vector.magnitude(after) > 0 then
				turns[index] = math.abs(Math.DeltaAngle(Motion.HeadingOf(before), Motion.HeadingOf(after)))
			end
		end
	end
	local limits: { number } = {}
	for index = 1, #points - 1 do
		local hint = hints[index]
		local limit = if hint == "Fast" then FastSpeed elseif hint == "Slow" then SlowSpeed else CruiseSpeed
		if hint ~= "Fast" and (turns[index] > CurveAngle or turns[index + 1] > CurveAngle) then
			limit = math.min(limit, CurveSpeed)
		end
		limits[index] = limit
	end
	return { Points = points, Lengths = Track.Lengths(points), Hints = hints, Limits = limits }
end

-- The moving platform nearest to a point.
local function nearestPlatform(point: vector): (Entity?, PlatformScript.MovingPlatform?)
	local best: Entity? = nil
	local bestScript: PlatformScript.MovingPlatform? = nil
	local bestDistance = math.huge
	for _, entity in Scene.FindAllWithComponent("Script") do
		local platform = entity:GetScript("MovingPlatform") :: PlatformScript.MovingPlatform?
		if platform then
			local distance = vector.magnitude(flat(entity.Transform.WorldPosition - point))
			if distance < bestDistance then
				best = entity
				bestScript = platform
				bestDistance = distance
			end
		end
	end
	return best, bestScript
end

-- Drives the level's ball to its goal, one tick at a time, from a test case.
function Autopilot.Drive()
	local ballEntity = Scene.FindByName("Ball") :: Entity
	local ball = ballEntity:GetScript("Ball") :: BallScript.Ball
	local body = ballEntity.RigidBody :: RigidBody
	local route = Autopilot.Route()
	local points, lengths = route.Points, route.Lengths
	local lastSegment = #points - 1

	local segment = 1
	local state = "Follow"
	local boarded: { boolean } = {}
	local platformEntity: Entity? = nil
	local platform: PlatformScript.MovingPlatform? = nil
	local seenRespawns = ball.Respawns
	local lastX: number? = nil
	local lastZ: number? = nil

	while not ball.ReachedGoal do
		local position = ballEntity.Transform.WorldPosition
		local velocity = flat(body:GetLinearVelocity())

		local along: number
		if ball.Respawns ~= seenRespawns then
			-- A respawn: find the ball on the whole path again and board again whatever lies ahead.
			seenRespawns = ball.Respawns
			state = "Follow"
			along, segment = Track.Project(points, lengths, position, 1, lastSegment)
			for index = 1, #points do
				if lengths[index] > along then
					boarded[index] = nil
				end
			end
		else
			along, segment = Track.Project(points, lengths, position, segment - Window, segment + Window)
		end

		-- The next boarding point ahead that has not been ridden yet.
		local board: number? = nil
		for index = segment, #points do
			if route.Hints[index] == "Board" and not boarded[index] then
				board = index
				break
			end
		end

		local wanted: vector
		if state == "Follow" then
			-- The speed plan: the segment's limit, slowing down ahead of slower segments and to a stop at a boarding
			-- point; a Fast segment (a run-up or a jump) keeps its speed.
			local speed = route.Limits[segment]
			if route.Hints[segment] ~= "Fast" then
				for index = segment + 1, math.min(lastSegment, segment + 8) do
					local distance = math.max(0, lengths[index] - along)
					speed = math.min(speed, math.sqrt(route.Limits[index] * route.Limits[index] + 2 * Brake * distance))
				end
			end
			local targetAlong = along + Lookahead
			if board then
				local distance = math.max(0, lengths[board] - along)
				speed = math.min(speed, math.sqrt(2 * Brake * distance))
				targetAlong = math.min(targetAlong, lengths[board])
				if vector.magnitude(flat(points[board] - position)) < 0.4 and vector.magnitude(velocity) < 0.5 then
					state = "Wait"
					platformEntity, platform = nearestPlatform(points[board])
				end
			end
			local target = Track.PointAt(points, lengths, targetAlong)
			local toTarget = flat(target - position)
			local distance = vector.magnitude(toTarget)
			if board and targetAlong >= lengths[board] and distance < Lookahead then
				-- Close to the boarding point: hold it.
				wanted = clampLength(toTarget * HoldGain, math.max(speed, 0.2))
			else
				wanted = if distance > 0 then toTarget / distance * speed else vector.zero
			end
		elseif state == "Wait" then
			local holdAt = points[board or segment]
			wanted = clampLength(flat(holdAt - position) * HoldGain, HoldSpeed)
			if platform and platform.AtStart and platform.DwellLeft >= BoardDwell then
				state = "Ride"
			end
		else
			-- Ride: hold the platform's centre, moving with it, until it dwells at its end.
			local entity = platformEntity :: Entity
			local platformBody = entity.RigidBody :: RigidBody
			local carry = flat(platformBody:GetLinearVelocity())
			wanted = carry + clampLength(flat(entity.Transform.WorldPosition - position) * HoldGain, HoldSpeed)
			if platform and platform.AtEnd then
				state = "Follow"
				if board then
					boarded[board] = true
				end
			end
		end

		-- Steer like a player: the velocity error, clamped to a unit stick, in the frame of the ball's heading.
		local steer = clampLength((wanted - velocity) * Gain, 1)
		local x = math.round(vector.dot(steer, Motion.Right(ball.Heading)) * Quantum) / Quantum
		local z = math.round(vector.dot(steer, Motion.Forward(ball.Heading)) * Quantum) / Quantum
		if x ~= lastX then
			lastX = x
			Test.InjectAction(Actions.MoveX, nil, x)
		end
		if z ~= lastZ then
			lastZ = z
			Test.InjectAction(Actions.MoveZ, nil, z)
		end
		Test.WaitTicks(1)
	end

	Test.InjectAction(Actions.MoveX, nil, 0)
	Test.InjectAction(Actions.MoveZ, nil, 0)
end

-- The recording case of a level: drive to the goal under par without a fall, then let the game show the finish. The
-- par is the scene's own (its /Game's ParTicks); the suite's constant, which sets the case's TimeoutTicks, must equal it.
function Autopilot.Case(parTicks: number)
	local game = (Scene.FindByName("Game") :: Entity):GetScript("Game") :: GameScript.Game
	local ball = (Scene.FindByName("Ball") :: Entity):GetScript("Ball") :: BallScript.Ball
	Test.ExpectEqual(game.ParTicks, parTicks, "the scene's par is the suite's")
	Autopilot.Drive()
	Test.Expect(ball.ReachedGoal, "the ball reached the goal")
	Test.ExpectEqual(ball.Respawns, 0, "the ball never fell")
	Test.Expect(game.Ticks < game.ParTicks, string.format("the level took %d ticks, par is %d", game.Ticks, game.ParTicks))
	Test.WaitTicks(30)
	Test.Expect(game.Finished, "the game shows the finish")
end

return Autopilot
