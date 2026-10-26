--!strict
-- Physics.scene (Architecture §15.5, §9): dynamic, kinematic and static bodies, a compound whose contacts name the child
-- collider, a shared static body of track segments with a ball rolling across their seams, implicit static and sensor
-- bodies, a kinematic platform carrying a rider, a kinematic platform carrying a character with InheritGroundVelocity,
-- continuous collision detection, a character controller seen by a sensor and by raycasts, a trigger sweeping over a
-- sleeping body, queries and bounds, and the project's layer matrix (Ball and Debris do not collide; both collide with
-- Track). Gravity is the project's (0, -10, 0) at 64 Hz.

local ContactProbe = require("./ContactProbe")

local EPSILON = 1e-3

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `Physics.scene has an entity named {name}`)
	return entity
end

local function body(name: string): RigidBody
	local rigidBody = find(name).RigidBody
	assert(rigidBody, `{name} has a RigidBody`)
	return rigidBody
end

local function probe(name: string): ContactProbe.ContactProbe
	local instance = find(name):GetScript() :: ContactProbe.ContactProbe?
	assert(instance, `{name} runs ContactProbe`)
	return instance
end

local function contains(names: { string }, name: string): boolean
	return table.find(names, name) ~= nil
end

local function position(name: string): vector
	return find(name).Transform.WorldPosition
end

return Test.Suite("Physics", function()
	Test.Case("contact probes attach to the compound, the sensor and the kinematic trigger", function()
		-- The probes are attached at run time, before anything touches (the compound lands about 30 ticks in), so
		-- Physics.scene itself references nothing under Assets/Tests/, which exports leave out.
		local script = Assets.Load("Assets/Tests/Physics/ContactProbe.luau")
		assert(script, "ContactProbe.luau loads")
		for _, name in { "Compound", "Zone", "Scanner" } do
			local entity = find(name)
			Test.Expect(entity:AddComponent("Script", { Script = script }) ~= nil, `{name} takes a Script component`)
			Test.ExpectEqual(#probe(name).Collisions, 0, `{name}'s probe starts empty`)
		end
	end)

	Test.Case("a ball rolls across the seams of a shared static body without hopping", function()
		-- SeamBall (radius 0.25) starts rolling at 3 m/s on TrackA (top at y 0.5); the seams lie at x -1.5 and 1.5, and
		-- the track's end stop at x 4.5.
		local ball = find("SeamBall")
		local lowest, highest, fastestVertical = math.huge, -math.huge, 0
		for _ = 1, 150 do
			Test.WaitTicks(1)
			local at = ball.Transform.WorldPosition
			if at.x < 4 then
				lowest = math.min(lowest, at.y)
				highest = math.max(highest, at.y)
				fastestVertical = math.max(fastestVertical, math.abs(body("SeamBall"):GetLinearVelocity().y))
			end
		end
		Test.Expect(ball.Transform.WorldPosition.x > 1.6, `the ball crossed both seams (x {ball.Transform.WorldPosition.x})`)
		Test.ExpectNear(lowest, 0.75, 0.01, "the ball never sinks into a seam")
		Test.ExpectNear(highest, 0.75, 0.01, "the ball never hops at a seam")
		Test.Expect(fastestVertical < 0.1, `the ball's vertical speed stays small ({fastestVertical})`)
	end)

	Test.Case("the layer matrix drops the ball through the debris box onto the track", function()
		-- Ball-Debris is not in Physics.Collisions; Ball-Track and Debris-Track are.
		local ball = position("GhostBall")
		local box = position("DebrisBox")
		Test.ExpectNear(ball, vector.create(-8, 0.5, 0), 0.02, "the ball rests on the floor inside the box")
		Test.ExpectNear(box, vector.create(-8, 0.5, 0), 0.02, "the box rests on the floor")
		local ballMask = Physics.LayerMask("Ball")
		local debrisMask = Physics.LayerMask("Debris")
		Test.Expect(ballMask ~= debrisMask, "each layer has its own mask bit")
		Test.ExpectEqual(Physics.LayerMask("Ball", "Debris"), Physics.LayerMask("Debris", "Ball"))
		local down = vector.create(0, -1, 0)
		local ballHit = Physics.Raycast(vector.create(-8, 5, 0), down, 10, ballMask)
		local debrisHit = Physics.Raycast(vector.create(-8, 5, 0), down, 10, debrisMask)
		local trackHit = Physics.Raycast(vector.create(-8, 5, 0), down, 10, Physics.LayerMask("Track"))
		Test.Expect(ballHit ~= nil and ballHit.Entity == find("GhostBall"), "a Ball mask sees only the ball")
		Test.Expect(debrisHit ~= nil and debrisHit.Entity == find("DebrisBox"), "a Debris mask sees only the box")
		Test.Expect(trackHit ~= nil and trackHit.Entity == find("Floor"), "a Track mask sees only the floor")
		if trackHit then
			Test.ExpectNear(trackHit.Distance, 5, EPSILON)
			Test.ExpectNear(trackHit.Normal, vector.create(0, 1, 0), EPSILON)
		end
		local all = Physics.RaycastAll(vector.create(-8, 5, 0), down, 10)
		Test.ExpectEqual(#all, 3, "the ray passes the ball, the box and the floor")
		if #all == 3 then
			Test.ExpectEqual(all[3].Entity, find("Floor"))
			Test.Expect(all[1].Distance <= all[2].Distance and all[2].Distance <= all[3].Distance, "hits are sorted by distance")
		end
	end)

	Test.Case("a compound body's contacts name the child collider that touched", function()
		local contacts = probe("Compound").Collisions
		Test.Expect(#contacts > 0, "the compound landed")
		local touched: { string } = {}
		for _, contact in contacts do
			Test.Expect(contact.Collider == "CompoundBox" or contact.Collider == "CompoundSphere",
				`the contact names a child collider, not {contact.Collider}`)
			Test.ExpectEqual(contact.Other, "Floor")
			Test.ExpectEqual(contact.OtherCollider, "Floor")
			if not contains(touched, contact.Collider) then
				table.insert(touched, contact.Collider)
			end
		end
		Test.Expect(contains(touched, "CompoundBox"), "the lower box touched first")
		Test.ExpectEqual(contacts[1].Collider, "CompoundBox", "the box, which hangs lower, lands before the sphere")
		-- The body belongs to the RigidBody entity; each child has the bounds of its own collider, inside the body's.
		Test.Expect(Physics.GetBodyBounds(find("CompoundBox")) == nil, "a child collider owns no body")
		local box0, box1 = Physics.GetColliderBounds(find("CompoundBox"))
		local sphere0, sphere1 = Physics.GetColliderBounds(find("CompoundSphere"))
		local whole0, whole1 = Physics.GetBodyBounds(find("Compound"))
		Test.Expect(box0 ~= nil and sphere0 ~= nil and whole0 ~= nil, "the compound and its colliders have bounds")
		if box0 and box1 and sphere0 and sphere1 and whole0 and whole1 then
			Test.ExpectNear(vector.min(box0, sphere0), whole0, 0.01, "the body's bounds enclose exactly its colliders")
			Test.ExpectNear(vector.max(box1, sphere1), whole1, 0.01)
		end
	end)

	Test.Case("a shared static body reports its segments and the compound's bounds", function()
		local hit = Physics.Raycast(vector.create(0, 5, 6), vector.create(0, -1, 0), 10)
		Test.Expect(hit ~= nil, "the ray hits the track")
		if hit then
			Test.ExpectEqual(hit.Entity, find("TrackB"), "the hit names the segment")
			Test.ExpectEqual(hit.Body, find("Track"), "the hit names the shared body")
			Test.ExpectNear(hit.Distance, 4.5, EPSILON)
			Test.ExpectNear(hit.Point, vector.create(0, 0.5, 6), EPSILON)
		end
		Test.Expect(Physics.GetBodyBounds(find("TrackA")) == nil, "a segment owns no body")
		local segment0, segment1 = Physics.GetColliderBounds(find("TrackA"))
		local body0, body1 = Physics.GetBodyBounds(find("Track"))
		Test.Expect(segment0 ~= nil and body0 ~= nil, "the segment has collider and body bounds")
		if segment0 and segment1 and body0 and body1 then
			Test.ExpectNear(segment0, vector.create(-4.5, 0, 5), 0.05)
			Test.ExpectNear(segment1, vector.create(-1.5, 0.5, 7), 0.05)
			Test.ExpectNear(body0, vector.create(-4.5, 0, 5), 0.05)
			Test.ExpectNear(body1, vector.create(5, 1.5, 7), 0.05, "the body's bounds include the end stop")
		end
	end)

	Test.Case("colliders without a body above them form implicit static and sensor bodies", function()
		local wallHit = Physics.Raycast(vector.create(12, 1, -4), vector.create(-1, 0, 0), 10)
		Test.Expect(wallHit ~= nil and wallHit.Entity == find("Wall") and wallHit.Body == find("Wall"), "the wall is its own static body")
		if wallHit then
			Test.ExpectNear(wallHit.Distance, 3.75, EPSILON)
		end
		local zoneHit = Physics.Raycast(vector.create(4, 5, -4), vector.create(0, -1, 0), 10)
		Test.Expect(zoneHit ~= nil and zoneHit.Entity == find("Zone"), "raycasts include the implicit sensor")
		if zoneHit then
			Test.ExpectNear(zoneHit.Distance, 3, EPSILON)
		end
		Test.Expect(table.find(Physics.OverlapBox(vector.create(4, 1, -4), vector.create(0.5, 0.5, 0.5)), find("Zone")) ~= nil,
			"an overlap query finds the sensor")
	end)

	Test.Case("a character controller walks through a sensor and is seen by raycasts", function()
		local character = find("Character")
		local controller = character.CharacterController
		assert(controller, "Character has a CharacterController")
		Test.Expect(controller:IsGrounded(), "the character stands on the floor")
		Test.ExpectNear(controller:GetGroundNormal(), vector.create(0, 1, 0), EPSILON)
		local hit = Physics.Raycast(vector.create(1, 1, 2), vector.create(0, 0, -1), 10)
		Test.Expect(hit ~= nil and hit.Entity == character and hit.Body == character, "a raycast hits the character's inner body")
		local zone = probe("Zone")
		Test.ExpectEqual(#zone.TriggerEnters, 0, "nothing entered the sensor yet")
		for _ = 1, 60 do
			controller:Move(vector.create(3, 0, 0))
			Test.WaitTicks(1)
		end
		Test.ExpectNear(controller:GetVelocity(), vector.create(3, 0, 0), 0.05)
		Test.Expect(contains(zone.TriggerEnters, "Character"), "the sensor saw the character enter")
		for _ = 1, 60 do
			controller:Move(vector.create(3, 0, 0))
			Test.WaitTicks(1)
		end
		Test.Expect(contains(zone.TriggerExits, "Character"), "the sensor saw the character leave")
		Test.Expect(character.Transform.WorldPosition.x > 5.5, `the character walked past the sensor (x {character.Transform.WorldPosition.x})`)
	end)

	Test.Case("a kinematic platform carries its rider", function()
		local platform = body("Platform")
		local start = position("Platform")
		local rider = position("Rider").x
		for tick = 1, 64 do
			platform:MoveKinematic(start + vector.create(tick / 64, 0, 0), Quat.Identity())
			Test.WaitTicks(1)
		end
		Test.WaitTicks(1)
		Test.ExpectNear(position("Platform"), start + vector.create(1, 0, 0), EPSILON)
		Test.Expect(position("Rider").x - rider > 0.8, `the rider moved with the platform ({position("Rider").x - rider} m)`)
		Test.ExpectNear(position("Rider").y, 1.25, 0.02, "the rider stays on top")
	end)

	Test.Case("a kinematic platform carries a character with InheritGroundVelocity v * t and slides under one without", function()
		-- CarryPlatform (4 m square, its top at y 0.75) moves 2 m/s along +X for one second; Passenger stands on it with
		-- InheritGroundVelocity, Bystander without (Architecture §9.6).
		local passenger = find("Passenger").CharacterController
		assert(passenger, "Passenger has a CharacterController")
		local bystander = find("Bystander").CharacterController
		assert(bystander, "Bystander has a CharacterController")
		Test.Expect(passenger.InheritGroundVelocity, "the passenger opts in")
		Test.Expect(not bystander.InheritGroundVelocity, "the bystander keeps the default")
		Test.Expect(passenger:IsGrounded() and bystander:IsGrounded(), "both stand on the platform")
		local platform = body("CarryPlatform")
		local start = position("CarryPlatform")
		local passengerStart = position("Passenger")
		local bystanderStart = position("Bystander")
		for tick = 1, 64 do
			platform:MoveKinematic(start + vector.create(2 * tick / 64, 0, 0), Quat.Identity())
			Test.WaitTicks(1)
		end
		Test.ExpectNear(passenger:GetVelocity(), vector.create(2, 0, 0), 0.01, "the passenger moves at the platform's velocity")
		Test.ExpectNear(bystander:GetVelocity(), vector.zero, 0.01, "the bystander stands still")
		Test.WaitTicks(1)
		Test.ExpectNear(position("CarryPlatform"), start + vector.create(2, 0, 0), EPSILON)
		Test.ExpectNear(position("Passenger"), passengerStart + vector.create(2, 0, 0), 0.05, "carried 2 m/s for one second")
		Test.ExpectNear(position("Bystander"), bystanderStart, 0.05, "the platform slid under the bystander")
		Test.Expect(passenger:IsGrounded() and bystander:IsGrounded(), "both still stand on the platform")
	end)

	Test.Case("continuous collision detection stops a fast ball at a thin wall", function()
		-- Both balls started at 200 m/s towards the 5 cm wall at z -12; only the LinearCast one is stopped, against the
		-- wall's face at z -11.975 within Jolt's penetration slop (0.02 m).
		Test.ExpectNear(position("CcdBall").z, -11.885, 0.015,
			`the LinearCast ball rests against the wall (at {position("CcdBall")}, velocity {body("CcdBall"):GetLinearVelocity()})`)
		Test.Expect(position("DiscreteBall").z < -12.5, `the Discrete ball tunnelled through (z {position("DiscreteBall").z})`)
		Test.ExpectEqual(body("CcdBall").MotionQuality, "LinearCast")
	end)

	Test.Case("a kinematic trigger reports the sleeping body it sweeps over", function()
		local sleeper = body("Sleeper")
		Test.Expect(sleeper:IsSleeping(), `the resting box fell asleep (at {position("Sleeper")}, velocity {sleeper:GetLinearVelocity()})`)
		local scanner = body("Scanner")
		local start = position("Scanner")
		local target = position("Sleeper")
		for tick = 1, 32 do
			scanner:MoveKinematic(start + (target - start) * (tick / 32), Quat.Identity())
			Test.WaitTicks(1)
		end
		Test.WaitTicks(1)
		Test.Expect(contains(probe("Scanner").TriggerEnters, "Sleeper"), "the trigger saw the sleeping box")
	end)

	Test.Case("queries and bounds agree with the scene", function()
		local target = find("Target")
		local render0, render1 = target:GetWorldBounds()
		local collider0, collider1 = Physics.GetColliderBounds(target)
		local body0, body1 = Physics.GetBodyBounds(target)
		Test.Expect(render0 ~= nil and collider0 ~= nil and body0 ~= nil, "the target has render, collider and body bounds")
		if render0 and render1 and collider0 and collider1 and body0 and body1 then
			Test.ExpectNear(render0, vector.create(-11, 0, -7), EPSILON)
			Test.ExpectNear(render1, vector.create(-9, 2, -5), EPSILON)
			Test.ExpectNear(collider0, render0, 0.05)
			Test.ExpectNear(collider1, render1, 0.05)
			Test.ExpectNear(body0, collider0, EPSILON)
			Test.ExpectNear(body1, collider1, EPSILON)
		end
		local cast = Physics.SphereCast(vector.create(-10, 6, -6), 0.25, vector.create(0, -1, 0), 10)
		Test.Expect(cast ~= nil and cast.Entity == target, "a sphere cast hits the target")
		if cast then
			Test.ExpectNear(cast.Distance, 3.75, 0.01)
		end
		Test.Expect(table.find(Physics.OverlapSphere(vector.create(-10, 1, -6), 0.5), target) ~= nil, "an overlap sphere finds the target")
		Test.ExpectEqual(#Physics.OverlapSphere(vector.create(-10, 1, -6), 0.5, Physics.LayerMask("Ball")), 0,
			"a layer mask filters overlap queries")
		Test.ExpectNear(Physics.GetGravity(), vector.create(0, -10, 0), EPSILON)
		Physics.SetGravity(vector.create(0, -5, 0))
		Test.ExpectNear(Physics.GetGravity(), vector.create(0, -5, 0), EPSILON)
		Physics.SetGravity(vector.create(0, -10, 0))
		Test.Screenshot("Physics")
	end)
end)
