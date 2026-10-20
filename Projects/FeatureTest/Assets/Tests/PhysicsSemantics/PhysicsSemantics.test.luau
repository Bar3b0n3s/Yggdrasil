--!strict
-- PhysicsSemantics.scene (Docs/Decisions/0031-m20-contract.md decisions 2 to 5): a static Level body gathering three
-- bricks, the middle one scripted, with a ball dropped on it; a ball gliding 1 cm above a scripted brick, within Jolt's
-- speculative distance but never touching; a falling body whose scripted child collider passes through a trigger zone;
-- Anvil and Hammer, which destroy and read each other in one dispatch; then the read rule, Scene.GetCurrent and
-- Scene.Reload. Gravity is the project's (0, -10, 0) at 64 Hz; every probe is in the scene, so it records from tick 0.

local ColliderProbeScript = require("../../Scripts/PhysicsSemantics/ColliderProbe")
local CrusherScript = require("../../Scripts/PhysicsSemantics/Crusher")

local SCENE = "Assets/Scenes/PhysicsSemantics.scene"

local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, `PhysicsSemantics.scene has an entity named {name}`)
	return entity
end

local function Probe(name: string): ColliderProbeScript.ColliderProbe
	local instance = Find(name):GetScript() :: ColliderProbeScript.ColliderProbe?
	assert(instance ~= nil, `{name} runs ColliderProbe`)
	return instance
end

local function FirstEvent(events: { ColliderProbeScript.Event }, kind: string, other: string): ColliderProbeScript.Event?
	for _, event in events do
		if event.Kind == kind and event.Other == other then
			return event
		end
	end
	return nil
end

local function Count(events: { ColliderProbeScript.Event }, kind: string): number
	local count = 0
	for _, event in events do
		if event.Kind == kind then
			count += 1
		end
	end
	return count
end

return Test.Suite("PhysicsSemantics", function()
	Test.Case("a scripted collider gathered into a static body gets its own enter after its body's", function()
		-- DropBall (radius 0.25) falls from y 3 onto BrickB, whose top is at y 1.5, about 32 ticks in.
		Test.WaitTicks(90)
		local brick = Probe("BrickB")
		local ball = Probe("DropBall")
		local brickEnter = FirstEvent(brick.Events, "CollisionEnter", "DropBall")
		local ballEnter = FirstEvent(ball.Events, "CollisionEnter", "Level")
		assert(brickEnter ~= nil, "BrickB received its own OnCollisionEnter")
		assert(ballEnter ~= nil, "DropBall received the Level body's OnCollisionEnter")
		Test.ExpectEqual(brickEnter.Collider, "BrickB", "contact.Collider is the brick itself")
		Test.ExpectEqual(brickEnter.OtherCollider, "DropBall")
		Test.ExpectEqual(ballEnter.Collider, "DropBall")
		Test.ExpectEqual(ballEnter.OtherCollider, "BrickB", "the body event names the brick's collider")
		Test.ExpectEqual(brickEnter.Tick, ballEnter.Tick, "both arrive at the step the ball lands")
		local brickIndex = table.find(brick.Log, brickEnter)
		local ballIndex = table.find(brick.Log, ballEnter)
		Test.Expect(brickIndex ~= nil and ballIndex ~= nil and ballIndex < brickIndex, "the body's event is dispatched before the collider's")
		Test.ExpectEqual(Count(brick.Events, "CollisionExit"), 0, "the ball rests on the brick")
	end)

	Test.Case("a ball gliding within the speculative distance never enters", function()
		-- Glider moves at 3 m/s without gravity or damping, its lowest point 1 cm above GlideBrick's top, and has crossed it by now.
		local glider = Find("Glider")
		Test.Expect(glider.Transform.WorldPosition.x > 7.5, `the glider crossed the brick (x {glider.Transform.WorldPosition.x})`)
		Test.ExpectNear(glider.Transform.WorldPosition.y, 3.26, 1e-3, "the glider never touched the brick")
		Test.ExpectNear((glider.RigidBody :: RigidBody):GetLinearVelocity(), vector.create(3, 0, 0), 1e-3)
		Test.ExpectEqual(#Probe("Glider").Events, 0, "the glider received no callback")
		Test.ExpectEqual(#Probe("GlideBrick").Events, 0, "the brick received no callback")
	end)

	Test.Case("a scripted child collider of a falling body gets its own trigger enter and exit", function()
		-- Crate falls from y 5 through Zone (y 2 to 3) and lands on the floor; Tag is its only collider.
		local tag = Probe("Tag")
		local enter = FirstEvent(tag.Events, "TriggerEnter", "Zone")
		local exit = FirstEvent(tag.Events, "TriggerExit", "Zone")
		assert(enter ~= nil, "Tag received OnTriggerEnter from the zone")
		assert(exit ~= nil, "Tag received OnTriggerExit from the zone")
		Test.Expect(enter.Tick < exit.Tick, "the trigger enter comes first")
		local landing = FirstEvent(tag.Events, "CollisionEnter", "Floor")
		assert(landing ~= nil, "Tag received its own OnCollisionEnter from the floor")
		Test.ExpectEqual(landing.Collider, "Tag")
		Test.Expect(exit.Tick <= landing.Tick, "the crate left the zone before it landed")
	end)

	Test.Case("a partner's callback reads an entity destroyed earlier in the same dispatch", function()
		local anvil = Scene.FindByName("Anvil")
		local hammer = Scene.FindByName("Hammer")
		Test.Expect((anvil == nil) ~= (hammer == nil), "the entity with the lower id destroyed itself and the other remains")
		local survivor = anvil or hammer
		assert(survivor ~= nil, "one of Anvil and Hammer remains")
		local crusher = survivor:GetScript() :: CrusherScript.Crusher
		local reading = crusher.Read
		assert(reading ~= nil, `{survivor.Name} met its partner`)
		Test.Expect(reading.Ok, "the destroyed partner's name and transform were readable")
		Test.ExpectEqual(reading.Name, crusher.Partner)
		Test.Expect(reading.Height > 0, "its transform was read")
		Test.ExpectEqual(reading.Valid, false, "the partner was already destroyed when the callback ran")
	end)

	Test.Case("a scripted collider gets its own exit when the ball leaves its body", function()
		local body = Find("DropBall").RigidBody :: RigidBody
		body:Teleport(vector.create(-9, 0.25, 4))
		body:SetLinearVelocity(vector.zero)
		Test.WaitTicks(2)
		Test.Expect(FirstEvent(Probe("BrickB").Events, "CollisionExit", "DropBall") ~= nil, "BrickB received its own OnCollisionExit")
		Test.Expect(FirstEvent(Probe("DropBall").Events, "CollisionExit", "Level") ~= nil, "DropBall left the Level body")
		Test.ExpectEqual(Count(Probe("BrickB").Events, "CollisionEnter"), 1, "one enter, from the ball")
	end)

	Test.Case("an entity destroyed this tick stays readable until the flush and refuses writes", function()
		local doomed = Scene.CreateEntity("Doomed")
		doomed.Transform.Translation = vector.create(1, 2, 3)
		Scene.Destroy(doomed)
		Test.Expect(not doomed:IsValid(), "a destroyed entity is invalid at once")
		Test.ExpectEqual(Scene.FindByName("Doomed"), nil, "searches leave it out")
		Test.ExpectEqual(doomed.Name, "Doomed")
		Test.ExpectNear(doomed.Transform.Translation, vector.create(1, 2, 3), 1e-6)
		Test.Expect(not pcall(function()
			doomed.Transform.Translation = vector.zero
		end), "writes to it are refused")
		Test.WaitTicks(1)
		Test.Expect(not pcall(function()
			return doomed.Name
		end), "it is unreadable once flushed")
	end)

	Test.Case("Scene.GetCurrent names the scene asset the suite plays", function()
		local current = Scene.GetCurrent()
		Test.Expect(current ~= nil and current == Assets.Load(SCENE), "the suite's scene")
	end)

	Test.Case("Scene.Reload requests a restart from the start document and refuses a second transition", function()
		Scene.CreateEntity("BeforeReload")
		Scene.Reload({ Reloaded = true, Round = 2 })
		Test.Expect(not pcall(function()
			Scene.Reload()
		end), "a second transition in the same frame is refused")
		-- The replacement happens at the end of the frame; this case returns in the requesting frame.
		Test.Expect(Scene.FindByName("BeforeReload") ~= nil, "the scene lives until the frame ends")
	end)

	Test.Case("the reloaded scene starts over from its start document with the given parameters", function()
		Test.ExpectEqual(Scene.FindByName("BeforeReload"), nil, "entities created before the reload are gone")
		local parameters = Scene.GetLoadParameters()
		Test.ExpectEqual(parameters.Reloaded, true)
		Test.ExpectEqual(parameters.Round, 2)
		Test.Expect(Scene.GetCurrent() == Assets.Load(SCENE), "Scene.Reload keeps the current scene asset")
		Test.Expect(Scene.FindByName("Anvil") ~= nil and Scene.FindByName("Hammer") ~= nil, "both crushers are back")
		Test.Expect(Find("DropBall").Transform.WorldPosition.y > 2.5, "the ball is back at its start")
		Test.ExpectEqual(#Probe("BrickB").Events, 0, "the probes start over")
		Scene.Reload()
	end)

	Test.Case("Scene.Reload without parameters keeps the current ones", function()
		local parameters = Scene.GetLoadParameters()
		Test.ExpectEqual(parameters.Reloaded, true)
		Test.ExpectEqual(parameters.Round, 2)
		Test.Expect(Scene.GetCurrent() == Assets.Load(SCENE), "the current scene asset is unchanged")
	end)
end)
