--!strict
-- Surfaces.scene (Docs/Decisions/0053-m36-contact-surfaces.md; World Tree's briefs 15 and 16): one Static /Level compound
-- of marble (the body's friction, 0.5), polished ice (a ColliderSurface of friction 0.02), a bouncing pad (restitution 0.8)
-- and a belt (a surface velocity of 2.5 m/s along +X), with shards struck alike on the marble and the ice, balls dropped on
-- the pad and the marble, and a crate and a character, the Rider, on the belt; on the anvil a piston that pushes the
-- Pushed and a press that comes down on the Pinned, and on the feeder belt the Husk carried into a resting ram. Gravity is
-- the project's (0, -10, 0) at 64 Hz; every case starts from the scene as saved (Isolation Case).

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `Surfaces.scene has an entity named {name}`)
	return entity
end

local function position(name: string): vector
	return find(name).Transform.WorldPosition
end

local function rider(): CharacterController
	local character = find("Rider").CharacterController
	assert(character, "the Rider has a CharacterController")
	return character
end

return Test.Suite("Physics.Surfaces", function()
	Test.Case("shards struck alike travel farther on ice than on marble of one static compound", function()
		local marbleStart, iceStart = position("ShardOnMarble"), position("ShardOnIce")
		Test.WaitTicks(320)
		local marble = position("ShardOnMarble").z - marbleStart.z
		local ice = position("ShardOnIce").z - iceStart.z
		Test.Expect(marble > 0.5, `the marble shard slid a little (z {marble} m)`)
		Test.Expect(ice > 3 * marble, `the ice shard slid farther (ice {ice} m, marble {marble} m)`)
		Test.Expect(position("ShardOnIce").z < 6, "the ice shard came to rest on the ice")
		local iceSurface = Physics.GetSurface(find("Ice"))
		assert(iceSurface, "the ice has a surface")
		Test.ExpectNear(iceSurface.Friction, 0.02, 1e-6, "the ice's friction")
		local marbleSurface = Physics.GetSurface(find("Marble"))
		assert(marbleSurface, "the marble has its body's material")
		Test.ExpectNear(marbleSurface.Friction, 0.5, 1e-6, "the marble keeps the level's friction")
		Test.ExpectEqual(marbleSurface.SurfaceVelocity, vector.zero)
		Test.ExpectEqual(Physics.GetSurface(find("Level")), nil, "the level holds no collider of its own")
	end)

	Test.Case("a ball bounces off the pad and not off the marble", function()
		local pad, marble = find("BallOnPad"), find("BallOnMarble")
		local padLanded, marbleLanded = false, false
		local padHighest, marbleHighest = -math.huge, -math.huge
		for _ = 1, 160 do
			Test.WaitTicks(1)
			local padY = pad.Transform.WorldPosition.y
			local marbleY = marble.Transform.WorldPosition.y
			padLanded = padLanded or padY < 0.4
			marbleLanded = marbleLanded or marbleY < 0.4
			if padLanded then
				padHighest = math.max(padHighest, padY)
			end
			if marbleLanded then
				marbleHighest = math.max(marbleHighest, marbleY)
			end
		end
		Test.Expect(padLanded and marbleLanded, "both balls landed")
		Test.Expect(padHighest > 1.2, `the pad's restitution bounced its ball up (to {padHighest} m)`)
		Test.Expect(marbleHighest < 0.4, `the marble's ball stayed down (at most {marbleHighest} m)`)
		local surface = Physics.GetSurface(find("Pad"))
		assert(surface, "the pad has a surface")
		Test.ExpectNear(surface.Restitution, 0.8, 1e-6)
	end)

	Test.Case("the belt carries a crate and a character, stops and reverses", function()
		local character = rider()
		Test.WaitUntil(function(): boolean
			return character:IsGrounded()
		end, 64)
		local ground = character:GetGroundCollider()
		Test.ExpectEqual(ground, find("Belt"), "the Rider stands on the belt")
		assert(ground, "the Rider stands on a collider")
		local surface = Physics.GetSurface(ground)
		assert(surface, "the belt has a surface")
		Test.ExpectNear(surface.SurfaceVelocity, vector.create(2.5, 0, 0), 1e-5, "the belt's surface velocity in world space")
		Test.ExpectNear(surface.Friction, 0.8, 1e-6)

		-- Running: the Rider rides at the belt's speed, the crate catches up with it through friction.
		local riderStart, crateStart = position("Rider"), position("Crate")
		Test.WaitTicks(64)
		Test.ExpectNear(position("Rider").x - riderStart.x, 2.5, 0.1, "the Rider carried 2.5 m in a second")
		Test.Expect(position("Crate").x - crateStart.x > 1.5, `the crate rides the belt ({position("Crate").x - crateStart.x} m)`)

		-- Stopped through the proxy: both stop.
		local belt = find("Belt").ColliderSurface :: ColliderSurface
		belt.SurfaceVelocity = vector.zero
		Test.WaitTicks(32)
		local riderStopped, crateStopped = position("Rider"), position("Crate")
		Test.WaitTicks(32)
		Test.ExpectNear(position("Rider"), riderStopped, 0.001, "the Rider stops with the belt")
		Test.ExpectNear(position("Crate"), crateStopped, 0.02, "the crate stops with the belt")

		-- Reversed: both ride back.
		belt.SurfaceVelocity = vector.create(-2.5, 0, 0)
		Test.WaitTicks(64)
		Test.Expect(position("Rider").x < riderStopped.x - 2, "the Rider rides back")
		Test.Expect(position("Crate").x < crateStopped.x - 1, "the crate rides back")
		local reversed = Physics.GetSurface(find("Belt"))
		assert(reversed, "the belt has a surface")
		Test.ExpectNear(reversed.SurfaceVelocity, vector.create(-2.5, 0, 0), 1e-5)
	end)

	Test.Case("a Kinematic piston pushes a character at its velocity", function()
		local pushed = find("Pushed")
		local character = pushed.CharacterController :: CharacterController
		local piston = find("Piston")
		local start = pushed.Transform.WorldPosition
		local deepest = -math.huge
		local lastSecond = start
		for tick = 1, 96 do
			if tick == 33 then
				lastSecond = pushed.Transform.WorldPosition
			end
			Test.WaitTicks(1)
			-- The piston body's face, where the last step left it (its Transform already holds the next step's target, which
			-- its mover wrote in this tick's fixed update), against the capsule's back (its 0.3 m radius behind).
			local _, pistonMax = Physics.GetBodyBounds(piston)
			assert(pistonMax, "the piston has a body")
			local overlap = pistonMax.x - (pushed.Transform.WorldPosition.x - 0.3)
			deepest = math.max(deepest, overlap)
			Test.Expect(not character:IsSqueezed(), "a pushed character is not squeezed")
		end
		-- Its last second at the piston's 2 m/s (GetVelocity stays what the character asked for: none).
		Test.ExpectNear(pushed.Transform.WorldPosition.x - lastSecond.x, 2, 0.05, "the character moves at the piston's 2 m/s")
		Test.Expect(pushed.Transform.WorldPosition.x - start.x > 2, "the piston pushed the character along")
		Test.Expect(deepest < 0.005, `the piston never entered the capsule (at most {deepest} m)`)
		Test.ExpectEqual(character:GetSqueezingBody(), nil)
	end)

	Test.Case("a press brought down onto a grounded character reports it squeezed by the press", function()
		local pinned = find("Pinned")
		local character = pinned.CharacterController :: CharacterController
		local press = find("Press")
		local squeezed = false
		for _ = 1, 48 do
			Test.WaitTicks(1)
			-- The hammer body's face, where the last step left it (as the piston's), above the 1.8 m character's head.
			local pressMin = Physics.GetBodyBounds(press)
			assert(pressMin, "the press has a body")
			local gap = pressMin.y - (pinned.Transform.WorldPosition.y + 1.8)
			if gap > 0.025 then
				Test.Expect(not character:IsSqueezed(), `not squeezed while the hammer is {gap} m above its head`)
			end
			if character:IsSqueezed() then
				squeezed = true
				Test.ExpectEqual(character:GetSqueezingBody(), press, "squeezed by the press")
			end
		end
		Test.Expect(squeezed, "the press squeezed the character")
		Test.ExpectNear(pinned.Transform.WorldPosition.y, 0, 0.02, "the floor holds the character")
	end)

	Test.Case("a character carried into a resting ram stops against it and is not squeezed", function()
		local husk = find("Husk")
		local character = husk.CharacterController :: CharacterController
		for _ = 1, 192 do
			Test.WaitTicks(1)
			Test.Expect(not character:IsSqueezed(), "a resting ram squeezes nothing")
		end
		local stopped = husk.Transform.WorldPosition
		-- The ram's face at x 1.5, the capsule's 0.3 m radius and the character's 2 cm padding before it.
		Test.ExpectNear(stopped.x, 1.18, 0.03, "the Husk stops against the ram")
		Test.WaitTicks(16)
		Test.ExpectNear(husk.Transform.WorldPosition, stopped, 0.001, "and stays there while the belt runs on")
		Test.ExpectEqual(character:GetGroundCollider(), find("Feeder"), "still on the belt")
		Test.ExpectEqual(character:GetSqueezingBody(), nil)
	end)

	Test.Case("a character in the air stands on no collider", function()
		local character = find("Faller").CharacterController :: CharacterController
		Test.ExpectEqual(character:GetGroundCollider(), nil, "the Faller starts in the air")
		Test.WaitUntil(function(): boolean
			return character:IsGrounded()
		end, 128)
		Test.ExpectEqual(character:GetGroundCollider(), find("Marble"), "the Faller lands on the marble")
	end)
end)
