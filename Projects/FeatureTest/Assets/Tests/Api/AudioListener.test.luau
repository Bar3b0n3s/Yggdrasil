--!strict
-- The AudioListener proxy: its one field read and written.
local Support = require("./Support")

return Test.Suite("Api.AudioListener", function()
	Test.Case("Primary reads and writes through the proxy", function()
		local listener = Support.Find("Camera").AudioListener :: AudioListener
		Test.ExpectEqual(listener.Primary, true, "the camera's listener is primary")
		Support.RoundTrip(listener, "Primary", false)
		Support.RoundTrip(listener, "Primary", true)
		local added = Scene.CreateEntity("SecondEar"):AddComponent("AudioListener", { Primary = false }) :: AudioListener
		Test.ExpectEqual(added.Primary, false)
	end)
end)
