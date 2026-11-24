--!strict
-- The recording suite of Assets/Tests/Replays/CharacterMoves.replay (Architecture §13.6 "Recording suites";
-- Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md): its one case only injects input and reads the
-- scene, so the replay it records with test.run {filter: "Record.CharacterMoves", record} reproduces it without the suite.
-- All three at once: the Swimmer swims out over the pit with free moves and back up the ramp onto the shelf; the Climber
-- goes up the ladder and steps off onto the platform; the Walker crosses the bridge of light planks once, onto the far rim.
-- Re-recording is the one call Scaffold/CharacterMovesReplay.jsonl makes.

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

return Test.Suite("Record.CharacterMoves", function()
	Test.Case("swim out and back, climb the ladder and cross the bridge once", function()
		local swimmer = controller("Swimmer")
		local swimmerState = find("Swimmer"):GetScript()
		local climber = controller("Climber")
		local climberState = find("Climber"):GetScript()
		local walker = controller("Walker")
		Test.WaitTicks(16)
		Test.InjectKey("A", "Down")
		Test.InjectKey("E", "Down")
		Test.InjectKey("L", "Down")
		local swum, returned, reached, climbed, stepped, crossed = false, false, false, false, false, false
		for _ = 1, 960 do
			Test.WaitTicks(1)
			local swimmerAt = find("Swimmer").Transform.WorldPosition
			if not swum and swimmerAt.x <= -28 then
				swum = true
				Test.InjectKey("A", "Up")
				Test.InjectKey("D", "Down")
			elseif swum and not returned and swimmerAt.x > -15.5 and swimmer:IsGrounded() and not swimmerState.Swimming then
				returned = true
				Test.InjectKey("D", "Up")
			end
			local climberAt = find("Climber").Transform.WorldPosition
			if not reached and climberState.OnLadder then
				reached = true
				Test.InjectKey("E", "Up")
				Test.InjectKey("W", "Down")
			elseif reached and not climbed and climberAt.y > 6.05 then
				climbed = true
				Test.InjectKey("E", "Down")
			elseif climbed and not stepped and climber:IsGrounded() and climberAt.x > 1.5 then
				stepped = true
				Test.InjectKey("E", "Up")
				Test.InjectKey("W", "Up")
			end
			if not crossed and find("Walker").Transform.WorldPosition.x >= 12.12 then
				crossed = true
				Test.InjectKey("L", "Up")
			end
			if returned and stepped and crossed then
				break
			end
		end
		Test.WaitTicks(8)
		Test.Expect(swimmer:IsGrounded() and find("Swimmer").Transform.WorldPosition.x > -16, "the Swimmer stands in the shallows")
		Test.Expect(climber:IsGrounded() and math.abs(find("Climber").Transform.WorldPosition.y - 6) < 0.02, "the Climber stands on the platform")
		Test.Expect(walker:IsGrounded() and find("Walker").Transform.WorldPosition.x > 11.62, "the Walker is on the far rim")
	end)
end)
