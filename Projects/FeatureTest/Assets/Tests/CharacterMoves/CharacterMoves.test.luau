--!strict
-- CharacterMoves.scene (Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md; Skerries' briefs S17 and
-- S19), each case from a fresh load of the scene. The Swimmer holds its feet 1.3 m under a computed swell with
-- CharacterController:MoveFree while it swims 20 m out over the pit and back, then walks up the ramp and stands in the
-- shallows. The Climber goes up a 6 m ladder with free moves, steps off at the top with a forward Move and climbs down
-- again. The Walker, 70 kg, crosses a bridge of thirteen hinged 25 kg planks four times, turning back at once, each time
-- onto the far rim, standing on the light planks' surface while the deck gives under it. Their behaviours read the input
-- this suite injects (Swimmer.luau, Climber.luau, BridgeWalker.luau). Gravity is the project's (0, -10, 0) at 64 Hz.

local DEPTH = 1.3
-- The pit, beyond the ramp's foot; the shelf's edge, where the ramp reaches it; the bridge's rims.
local PIT_EDGE = -20.5
local SHELF_EDGE = -16
local LEFT_RIM = -0.08
local RIGHT_RIM = 11.62
local PLANK_HALF_THICKNESS = 0.05

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `CharacterMoves.scene has an entity named {name}`)
	return entity
end

local function controller(name: string): CharacterController
	local character = find(name).CharacterController
	assert(character, `{name} has a CharacterController`)
	return character
end

local function position(name: string): vector
	return find(name).Transform.WorldPosition
end

-- How far `base` is under the top face of `plank` (a box 0.1 m thick), extended as a plane, at its X and Z.
local function sinkUnder(plank: Entity, base: vector): number
	local rotation = plank.Transform.WorldRotation
	local normal = rotation * vector.create(0, 1, 0)
	local top = plank.Transform.WorldPosition + rotation * vector.create(0, PLANK_HALF_THICKNESS, 0)
	local height = top.y - (normal.x * (base.x - top.x) + normal.z * (base.z - top.z)) / normal.y
	return height - base.y
end

return Test.Suite("Physics.CharacterMoves", function()
	Test.Case("a swimmer holds its feet 1.3 m under the swell while it swims 20 m out and back, then stands in the shallows", function()
		local swimmer = controller("Swimmer")
		local state = find("Swimmer"):GetScript()
		Test.WaitUntil(function(): boolean
			return swimmer:IsGrounded()
		end, 64)
		-- Out along -X: down the ramp, off its foot and 20 m over the pit.
		Test.InjectKey("A", "Down")
		local held = 0
		local worst = 0
		local function swim(arrived: () -> boolean, limit: number)
			for _ = 1, limit do
				Test.WaitTicks(1)
				local base = position("Swimmer")
				if base.x < PIT_EDGE then
					Test.Expect(state.Swimming, `the Swimmer swims over the pit (x {base.x})`)
					held += 1
					worst = math.max(worst, math.abs(base.y - (state.Surface - DEPTH)))
				end
				if arrived() then
					return
				end
			end
			Test.Fail(`the Swimmer did not get there within {limit} ticks (x {position("Swimmer").x})`)
		end
		swim(function(): boolean
			return position("Swimmer").x <= PIT_EDGE - 20
		end, 640)
		Test.InjectKey("A", "Up")
		-- Back along +X, up the ramp onto the shelf.
		Test.InjectKey("D", "Down")
		swim(function(): boolean
			return position("Swimmer").x > SHELF_EDGE + 0.5 and swimmer:IsGrounded() and not state.Swimming
		end, 640)
		local seconds = held * Time.GetFixedDeltaTime()
		Test.Expect(seconds >= 10, `the Swimmer swam at depth for 10 s (it did for {seconds} s)`)
		Test.Expect(worst < 0.05, `its feet stayed 1.3 m (+- 5 cm) under the swell (off by up to {worst} m)`)
		-- Standing in the shallows, it walks on with Move.
		local standing = position("Swimmer")
		Test.WaitTicks(32)
		Test.InjectKey("D", "Up")
		Test.Expect(swimmer:IsGrounded() and not state.Swimming, "the Swimmer stands on the shelf")
		Test.Expect(position("Swimmer").x - standing.x > 1.5, `it walks along the shelf (x {standing.x} to {position("Swimmer").x})`)
		Test.Expect(math.abs(position("Swimmer").y - -1) < 0.02, `on the shelf's top (y {position("Swimmer").y})`)
	end, { TimeoutTicks = 2400 })

	Test.Case("a climber goes up a 6 m ladder with free moves, steps off at the top and climbs down again", function()
		local climber = controller("Climber")
		local state = find("Climber"):GetScript()
		Test.WaitUntil(function(): boolean
			return climber:IsGrounded()
		end, 64)
		-- Forwards onto the ladder's foot, then up, pressed into the wall.
		Test.InjectKey("E", "Down")
		Test.WaitUntil(function(): boolean
			return state.OnLadder
		end, 64)
		Test.InjectKey("E", "Up")
		Test.InjectKey("W", "Down")
		local nearest = -math.huge
		Test.WaitUntil(function(): boolean
			local base = position("Climber")
			if base.y > 0.5 and base.y < 5.5 then
				nearest = math.max(nearest, base.x)
			end
			return base.y > 6.05
		end, 256)
		Test.Expect(nearest < 0.71, `the Climber never goes into the wall's face at x 1 (its axis reached x {nearest})`)
		-- Off at the top with a forward walk.
		Test.InjectKey("E", "Down")
		Test.WaitUntil(function(): boolean
			return climber:IsGrounded() and position("Climber").x > 1.5
		end, 128)
		Test.InjectKey("E", "Up")
		Test.InjectKey("W", "Up")
		Test.WaitTicks(2)
		Test.Expect(math.abs(position("Climber").y - 6) < 0.02, `the Climber stands on the platform (y {position("Climber").y})`)
		-- Back off the edge onto the ladder, where it holds, then down.
		Test.InjectKey("Q", "Down")
		Test.WaitUntil(function(): boolean
			return state.OnLadder and not climber:IsGrounded()
		end, 128)
		Test.InjectKey("Q", "Up")
		Test.WaitTicks(16)
		local holding = position("Climber").y
		Test.WaitTicks(16)
		Test.Expect(math.abs(position("Climber").y - holding) < 0.01, `the Climber holds on the ladder with no input (y {holding} to {position("Climber").y})`)
		Test.InjectKey("S", "Down")
		local fastest = 0
		Test.WaitUntil(function(): boolean
			fastest = math.min(fastest, climber:GetVelocity().y)
			return climber:IsGrounded() and position("Climber").y < 0.05
		end, 320)
		Test.InjectKey("S", "Up")
		Test.Expect(fastest > -2.05, `it climbs down at the speed it asks for, never falling (fastest {fastest} m/s)`)
		Test.Expect(position("Climber").x < 0.71, `at the wall's foot (x {position("Climber").x})`)
	end, { TimeoutTicks = 1200 })

	Test.Case("a walker crosses the bridge of thirteen 25 kg planks four times onto the far rim, on the planks' surface", function()
		local walker = controller("Walker")
		Test.WaitUntil(function(): boolean
			return walker:IsGrounded()
		end, 64)
		-- The deck settles, then hangs unloaded.
		Test.WaitTicks(64)
		local unloaded: { [string]: number } = {}
		for index = 1, 13 do
			local name = string.format("Plank%02d", index)
			unloaded[name] = position(name).y
		end
		for crossing = 1, 4 do
			local forwards = crossing % 2 == 1
			local key: Key = if forwards then "L" else "J"
			local goal = if forwards then RIGHT_RIM + 0.5 else LEFT_RIM - 0.5
			local deepest = -math.huge
			local lowest = math.huge
			Test.InjectKey(key, "Down")
			Test.WaitUntil(function(): boolean
				local base = position("Walker")
				local ground = walker:GetGroundCollider()
				if ground ~= nil and unloaded[ground.Name] ~= nil then
					deepest = math.max(deepest, sinkUnder(ground, base))
					lowest = math.min(lowest, ground.Transform.WorldPosition.y - unloaded[ground.Name])
				end
				return if forwards then base.x >= goal else base.x <= goal
			end, 400)
			Test.InjectKey(key, "Up")
			Test.Expect(deepest < 0.03, `crossing {crossing}: the Walker never sinks into a plank (up to {deepest} m)`)
			Test.Expect(lowest < -0.03, `crossing {crossing}: the deck gives under it (the plank underfoot down {-lowest} m)`)
		end
	end, { TimeoutTicks = 2400 })
end)
