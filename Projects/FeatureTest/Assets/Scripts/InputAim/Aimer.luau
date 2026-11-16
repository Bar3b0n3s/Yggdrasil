--!strict
-- InputAim.scene's Aimer (M34; Docs/Decisions/0049-m34-characters-input.md decisions 5 and 8; World Tree's brief 10):
-- gameplay that aims at the cursor in the fixed step. The rig's pose turns a little each tick in OnFixedUpdate, and
-- Camera:ScreenToWorldRayFrom builds the ray through the cursor from that pose, never from the camera's rendered one, so
-- the marker lands on the ground point under the cursor whatever the frame rate. Each pull of the Attack trigger (a
-- Button action bound to Gamepad.RightTrigger) counts a shot at the point aimed at. InputAim.replay moves the cursor and
-- pulls the trigger, so the marker's place and the shots depend on its input.
export type Aimer = ScriptInstance & {
	Yaw: number,
	Shots: number,
	Aimed: number,
	Target: vector,
}

local TURN = 12 -- degrees of yaw per second
local DISTANCE = 14 -- metres from the rig's pivot at the origin
local VIEW = vector.create(1280, 720, 0) -- the project's Window, the view of every fixed-view session

local Aimer = {}

function Aimer.OnCreate(self: Aimer)
	self.Yaw = 0
	self.Shots = 0
	self.Aimed = 0
	self.Target = vector.zero
end

-- The rig's pose for a yaw: looking down at 45 degrees towards the origin from DISTANCE away.
local function pose(yaw: number): (vector, Quat)
	local rotation = Quat.FromEuler(-45, yaw, 0)
	return rotation:Rotate(vector.create(0, 0, DISTANCE)), rotation
end

function Aimer.OnFixedUpdate(self: Aimer, dt: number)
	self.Yaw += TURN * dt
	local camera = Scene.FindByName("Camera")
	local lens = camera and camera.Camera
	if not lens then
		return
	end
	local cursor = Input.GetMousePosition()
	local x = math.clamp(cursor.x, 0, VIEW.x - 1)
	local y = math.clamp(cursor.y, 0, VIEW.y - 1)
	local position, rotation = pose(self.Yaw)
	local origin, direction = lens:ScreenToWorldRayFrom(x, y, position, rotation)
	local hit = Physics.Raycast(origin, direction, 100)
	if hit then
		self.Aimed += 1
		self.Target = hit.Point
		local marker = Scene.FindByName("Marker")
		if marker then
			marker.Transform.WorldPosition = hit.Point
		end
	end
	if Input.IsActionPressed("Attack") then
		self.Shots += 1
	end
end

return Script.Define("Aimer", Aimer)
