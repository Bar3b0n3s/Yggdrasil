--!strict
-- CharacterDebris.scene (Docs/Decisions/0049-m34-characters-input.md decisions 1 and 2; World Tree's brief 11): a
-- character pushes Dynamic bodies through its own contacts only, and a Dynamic body lighter than it never moves it. The
-- Warden (1.8 m, radius 0.35 m, 80 kg, step 0.35 m) walks at 6.5 m/s through fifteen 0.35 kg shards on the floor; the
-- Climber and the Pusher, characters like it, walk at 2 m/s into a 0.25 m high crate below their step, the Climber's of
-- 500 kg and the Pusher's of 0.35 kg. Gravity is the project's (0, -10, 0) at 64 Hz.

local SHARD_COUNT = 15

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `CharacterDebris.scene has an entity named {name}`)
	return entity
end

local function controller(name: string): CharacterController
	local character = find(name).CharacterController
	assert(character, `{name} has a CharacterController`)
	return character
end

local function shardName(index: number): string
	return string.format("Shard%02d", index)
end

-- Walks `name` at `velocity` for `ticks` fixed steps and returns the highest its base reached.
local function walk(name: string, velocity: vector, ticks: number): number
	local character = controller(name)
	local highest = -math.huge
	for _ = 1, ticks do
		character:Move(velocity)
		Test.WaitTicks(1)
		highest = math.max(highest, find(name).Transform.WorldPosition.y)
	end
	return highest
end

return Test.Suite("Physics.CharacterDebris", function()
	Test.Case("a character walks through fifteen light shards on the floor, stays on it and pushes them aside", function()
		local warden = controller("Warden")
		Test.WaitUntil(function(): boolean
			return warden:IsGrounded()
		end, 120)
		local rest: { vector } = {}
		for index = 1, SHARD_COUNT do
			rest[index] = find(shardName(index)).Transform.WorldPosition
		end
		local highest = -math.huge
		local lowestShard = math.huge
		for _ = 1, 60 do
			warden:Move(vector.create(6.5, 0, 0))
			Test.WaitTicks(1)
			highest = math.max(highest, find("Warden").Transform.WorldPosition.y)
			for index = 1, SHARD_COUNT do
				lowestShard = math.min(lowestShard, find(shardName(index)).Transform.WorldPosition.y)
			end
		end
		Test.Expect(highest < 0.1, `the Warden stays on the floor (its base reached {highest} m)`)
		Test.Expect(lowestShard > -0.05, `no shard goes through the floor (the lowest reached {lowestShard} m)`)
		-- Pushing them slows it: it walks some 4 of the 6 m it asks for.
		Test.Expect(find("Warden").Transform.WorldPosition.x > 2, `the Warden walked on (x {find("Warden").Transform.WorldPosition.x})`)
		local pushed = 0
		for index = 1, SHARD_COUNT do
			local moved = find(shardName(index)).Transform.WorldPosition - rest[index]
			if vector.magnitude(vector.create(moved.x, 0, moved.z)) > 0.3 then
				pushed += 1
			end
		end
		Test.Expect(pushed >= 5, `the shards are pushed aside along the ground ({pushed} moved more than 0.3 m)`)
	end)

	Test.Case("a character climbs a crate heavier than it below its step, and pushes a lighter one instead", function()
		Test.WaitUntil(function(): boolean
			return controller("Climber"):IsGrounded() and controller("Pusher"):IsGrounded()
		end, 120)
		local climbed = walk("Climber", vector.create(2, 0, 0), 64)
		Test.Expect(climbed > 0.2, `the Climber stepped onto the 500 kg crate (its base reached {climbed} m)`)
		Test.Expect(find("HeavyCrate").Transform.WorldPosition.x < 1.5, "the heavy crate stays where it was")
		-- The Pusher reaches its crate after about 45 ticks, then pushes it along for the rest.
		local pushing = walk("Pusher", vector.create(2, 0, 0), 96)
		Test.Expect(pushing < 0.05, `the Pusher stays on the floor (its base reached {pushing} m)`)
		Test.Expect(find("LightCrate").Transform.WorldPosition.x > 2, `the light crate is pushed ahead (x {find("LightCrate").Transform.WorldPosition.x})`)
	end)
end)
