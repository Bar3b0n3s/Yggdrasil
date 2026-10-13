--!strict
-- The Camera proxy (§4.3, §11.5): screen rays and screen points in the session's 1280 x 720 view, in both projections,
-- and every field read and written.
local Support = require("./Support")

local CENTER_X, CENTER_Y = 639.5, 359.5

return Test.Suite("Api.Camera", function()
	Test.Case("the view center ray follows the camera and points project back to the center", function()
		local entity = Support.Find("Camera")
		local camera = entity.Camera :: Camera
		-- Rays use the rendered camera pose, so let one frame render first.
		Test.WaitTicks(1)
		local origin, direction = camera:ScreenToWorldRay(CENTER_X, CENTER_Y)
		Test.ExpectNear(origin, entity.Transform.WorldPosition, 0.0001, "a perspective ray starts at the camera")
		Test.ExpectNear(direction, entity.Transform:Forward(), 0.0001, "and runs along its forward axis")
		local ahead = entity.Transform.WorldPosition + entity.Transform:Forward() * 5
		local screen = camera:WorldToScreen(ahead)
		Test.ExpectNear(vector.create(screen.x, screen.y, 0), vector.create(CENTER_X, CENTER_Y, 0), 0.01)
		Test.ExpectNear(screen.z, camera.NearClip / 5, 0.0001, "reverse-Z depth")
		local _, corner = camera:ScreenToWorldRay(0.5, 0.5)
		Test.Expect(vector.dot(corner, entity.Transform:Right()) < 0 and vector.dot(corner, entity.Transform:Up()) > 0, "the top-left pixel looks up and left")
		camera.Projection = "Orthographic"
		local orthoOrigin, orthoDirection = camera:ScreenToWorldRay(CENTER_X, CENTER_Y)
		Test.ExpectNear(orthoDirection, entity.Transform:Forward(), 0.0001, "orthographic rays are parallel")
		Test.ExpectNear(orthoOrigin, entity.Transform.WorldPosition + entity.Transform:Forward() * camera.NearClip, 0.0001)
		camera.Projection = "Perspective"
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local camera = Support.Find("Camera").Camera :: Camera
		for _, projection: ProjectionType in { "Orthographic", "Perspective" } do
			Support.RoundTrip(camera, "Projection", projection)
		end
		Test.ExpectNear(Support.RoundTrip(camera, "VerticalFov", 70, 1e-5), 50, 1e-5, "the scene's field of view")
		Support.RoundTrip(camera, "OrthographicSize", 6, 1e-6)
		Support.RoundTrip(camera, "NearClip", 0.5, 1e-6)
		Support.RoundTrip(camera, "FarClip", 250, 1e-4)
		Test.ExpectEqual(Support.RoundTrip(camera, "Primary", false), true)
		Test.ExpectEqual(Scene.GetPrimaryCamera(), nil, "no primary camera is left")
		camera.Primary = true
		for _, clear: ClearMode in { "Color", "Skybox" } do
			Support.RoundTrip(camera, "Clear", clear)
		end
		local color = Color.New(0.25, 0.5, 0.75)
		camera.ClearColor = color
		Support.ExpectColorNear(camera.ClearColor, color, 1e-6, "ClearColor reads back what was written")
		Test.Expect(not pcall(function()
			camera.VerticalFov = 0
		end), "a field of view below 1 degree is refused")
	end)
end)
