--!strict
-- The PixelPerfectCamera proxy (Docs/Decisions/0032-m21-contract.md decision 6): under the camera, Camera:WorldToScreen
-- and ScreenToWorldRay map the session's 1280 x 720 view through the output mapping (240 x 136 scene pixels at 8 per
-- unit, scaled 5 times into 1200 x 680 with a 40 and 20 pixel letterbox), and every field reads and writes.
local Support = require("./Support")

return Test.Suite("Api.PixelPerfectCamera", function()
	Test.Case("screen points and rays follow the integer scale and the letterbox", function()
		local entity = Support.Find("Camera")
		local camera = entity.Camera :: Camera
		-- Rays use the rendered camera pose, so let one frame render first.
		Test.WaitTicks(1)
		local origin = camera:WorldToScreen(vector.zero)
		local right = camera:WorldToScreen(vector.create(1, 0, 0))
		local up = camera:WorldToScreen(vector.create(0, 1, 0))
		Test.ExpectNear(right.x - origin.x, 40, 1e-3, "one unit is 8 scene pixels scaled 5 times")
		Test.ExpectNear(origin.y - up.y, 40, 1e-3, "and screen Y runs down")
		local rayOrigin, direction = camera:ScreenToWorldRay(origin.x, origin.y)
		Test.ExpectNear(direction, vector.create(0, 0, -1), 1e-5, "an orthographic camera looks down -Z")
		Test.ExpectNear(vector.create(rayOrigin.x, rayOrigin.y, 0), vector.zero, 1e-4, "the screen point of the origin casts back through it")
		-- A view pixel in the left letterbox casts a ray beside the 30 unit wide image.
		local beside = camera:ScreenToWorldRay(10, 360)
		Test.Expect(beside.x < -15, "a letterbox pixel lies outside the image")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local camera = Support.Find("Camera").PixelPerfectCamera :: PixelPerfectCamera
		Test.ExpectEqual(Support.RoundTrip(camera, "ReferenceWidth", 320), 240)
		Test.ExpectEqual(Support.RoundTrip(camera, "ReferenceHeight", 180), 136)
		Test.ExpectEqual(Support.RoundTrip(camera, "PixelsPerUnit", 16, 1e-6), 8)
		local color = Color.New(0.5, 0.25, 0)
		camera.LetterboxColor = color
		Support.ExpectColorNear(camera.LetterboxColor, color, 1e-6, "LetterboxColor reads back what was written")
		Test.Expect(not pcall(function()
			camera.ReferenceWidth = 0
		end), "a zero reference width is refused")
	end)
end)
