--!strict
-- The AudioSource proxy (§10.2, §11.5): playback control and every field read and written.
local Support = require("./Support")

local ATTENUATIONS: { Attenuation } = { "None", "Inverse", "Linear", "Exponential" }
local GROUPS: { AudioGroup } = { "Music", "Sfx", "Ui" }

return Test.Suite("Api.AudioSource", function()
	Test.Case("a source plays, pauses, resumes and stops its clip", function()
		local source = Support.Find("Speaker").AudioSource :: AudioSource
		Test.ExpectEqual(source:IsPlaying(), false, "PlayOnStart is off")
		source:Play()
		Test.ExpectEqual(source:IsPlaying(), true)
		Test.WaitTicks(1)
		Test.ExpectEqual(source:IsPlaying(), true, "still playing a tick later")
		source:Pause()
		Test.ExpectEqual(source:IsPlaying(), false, "a paused voice is not playing")
		source:Resume()
		Test.ExpectEqual(source:IsPlaying(), true)
		source:Stop()
		Test.ExpectEqual(source:IsPlaying(), false)
		source.Loop = true
		source:Play()
		Test.WaitTicks(40)
		Test.ExpectEqual(source:IsPlaying(), true, "a looping clip keeps playing past its end")
		source:Stop()
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local source = Support.Find("Speaker").AudioSource :: AudioSource
		local clip = Assets.Load("Assets/Audio/Beep.sfx") :: AssetRef
		Test.ExpectEqual(Support.RoundTrip(source, "Clip", clip), clip, "the scene assigns the beep")
		Test.ExpectNear(Support.RoundTrip(source, "Volume", 0.25, 1e-6), 0.8, 1e-6, "the scene's volume")
		Support.RoundTrip(source, "Pitch", 1.5, 1e-6)
		Support.RoundTrip(source, "Loop", true)
		Support.RoundTrip(source, "PlayOnStart", true)
		Support.RoundTrip(source, "Spatial", false)
		Support.RoundTrip(source, "MinDistance", 2, 1e-6)
		Support.RoundTrip(source, "MaxDistance", 30, 1e-6)
		for _, attenuation in ATTENUATIONS do
			Support.RoundTrip(source, "Attenuation", attenuation)
		end
		Support.RoundTrip(source, "Rolloff", 0.5, 1e-6)
		Support.RoundTrip(source, "DopplerFactor", 0, 1e-6)
		for _, group in GROUPS do
			Support.RoundTrip(source, "Group", group)
		end
		Support.RoundTrip(source, "Clip", nil)
		Test.Expect(not pcall(function()
			source.Volume = -1
		end), "a volume below its minimum is refused")
	end)
end)
