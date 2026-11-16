--!strict
-- InputAim.scene (M34; Docs/Decisions/0049-m34-characters-input.md decisions 5 and 8; World Tree's brief 10): a trigger
-- bound to a Button action (Attack, Gamepad.RightTrigger) fires past 0.5 and releases only below 0.4, and
-- Camera:ScreenToWorldRayFrom aims from the pose it is given, which the Aimer keeps in the fixed step.

local function aimer(): any
	local entity = Scene.FindByName("Aimer")
	assert(entity, "InputAim.scene has an Aimer")
	local instance = entity:GetScript("Aimer")
	assert(instance, "the Aimer runs its behaviour")
	return instance
end

local function camera(): Camera
	local entity = Scene.FindByName("Camera")
	assert(entity and entity.Camera, "InputAim.scene has a Camera")
	return entity.Camera :: Camera
end

return Test.Suite("InputAim", function()
	Test.Case("a trigger bound to a Button action fires past 0.5 and releases only below 0.4", function()
		local shots = aimer().Shots
		Test.InjectGamepad(0, "RightTrigger", 0.45)
		Test.WaitTicks(1)
		Test.Expect(not Input.IsActionDown("Attack"), "a trigger at 0.45 has not fired")
		Test.InjectGamepad(0, "RightTrigger", 0.6)
		Test.WaitTicks(1)
		Test.Expect(Input.IsActionDown("Attack") and Input.IsActionPressed("Attack"), "past 0.5 the action fires")
		Test.ExpectEqual(aimer().Shots, shots + 1, "the Aimer saw the press in its fixed step")
		Test.InjectGamepad(0, "RightTrigger", 0.45)
		Test.WaitTicks(1)
		Test.Expect(Input.IsActionDown("Attack") and not Input.IsActionReleased("Attack"), "between the thresholds it holds")
		Test.InjectGamepad(0, "RightTrigger", 0.35)
		Test.WaitTicks(1)
		Test.Expect(not Input.IsActionDown("Attack") and Input.IsActionReleased("Attack"), "below 0.4 it releases")
		Test.ExpectEqual(aimer().Shots, shots + 1, "one pull, one shot")
	end)

	Test.Case("ScreenToWorldRayFrom aims from the pose it is given, not the rendered one", function()
		Test.InjectMouseMove(vector.create(400, 500, 0))
		Test.WaitTicks(1)
		local state = aimer()
		-- The Aimer's rig pose for the tick that just ran: 14 m from the origin, looking down at 45 degrees, turned by its yaw.
		local rotation = Quat.FromEuler(-45, state.Yaw, 0)
		local position = rotation:Rotate(vector.create(0, 0, 14))
		local origin, direction = camera():ScreenToWorldRayFrom(400, 500, position, rotation)
		Test.ExpectNear(origin, position, 1e-4, "the ray starts at the given position")
		local hit = Physics.Raycast(origin, direction, 100)
		assert(hit, "the ray from the rig's pose meets the ground")
		Test.ExpectNear(state.Target, hit.Point, 1e-4, "the Aimer aimed at the same ground point in its fixed step")
		local marker = Scene.FindByName("Marker")
		assert(marker, "InputAim.scene has a Marker")
		Test.ExpectNear(marker.Transform.WorldPosition, hit.Point, 1e-4, "the marker stands there")
		-- The camera entity stays where the scene put it; its rendered pose gives another ray.
		local renderedOrigin = camera():ScreenToWorldRay(400, 500)
		Test.Expect(vector.magnitude(renderedOrigin - origin) > 1, "ScreenToWorldRay starts at the rendered camera instead")
	end)
end)
