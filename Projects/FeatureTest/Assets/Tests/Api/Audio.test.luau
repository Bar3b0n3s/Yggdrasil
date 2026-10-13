--!strict
-- The Audio module (§10.2, §11.5): the session's group mix and one-shots in every group, heard through
-- Test.CaptureAudio (deterministic: the session pulls 750 frames per tick at 64 Hz).
local GROUPS: { AudioGroup } = { "Music", "Sfx", "Ui" }

return Test.Suite("Api.Audio", function()
	Test.Case("every group starts at full volume and keeps the volume a script sets", function()
		for _, group in GROUPS do
			Test.ExpectEqual(Audio.GetGroupVolume(group), 1, group .. " starts at 1")
		end
		Audio.SetGroupVolume("Music", 0.5)
		Audio.SetGroupVolume("Sfx", 0.25)
		Audio.SetGroupVolume("Ui", 2)
		Test.ExpectEqual(Audio.GetGroupVolume("Music"), 0.5)
		Test.ExpectEqual(Audio.GetGroupVolume("Sfx"), 0.25)
		Test.ExpectEqual(Audio.GetGroupVolume("Ui"), 2)
		Test.Expect(not pcall(Audio.SetGroupVolume, "Sfx", -1), "a negative volume is refused")
		for _, group in GROUPS do
			Audio.SetGroupVolume(group, 1)
		end
	end)

	Test.Case("one-shots play in every group and are heard", function()
		local clip = Assets.Load("Assets/Audio/Beep.sfx") :: AssetRef
		local silence = Test.CaptureAudio(2)
		Test.ExpectEqual(silence.Peak, 0, "nothing plays before the one-shots")
		Audio.PlayOneShot(clip)
		Audio.PlayOneShot(clip, vector.create(0, 3, 10), 0.5, "Music")
		Audio.PlayOneShot(clip, nil, 0.75, "Sfx")
		Audio.PlayOneShot(clip, nil, 1, "Ui")
		local levels = Test.CaptureAudio(4)
		Test.Expect(levels.Peak > 0 and levels.RmsLeft > 0 and levels.RmsRight > 0, "the one-shots are heard")
		Audio.SetGroupVolume("Music", 0)
		Audio.SetGroupVolume("Sfx", 0)
		Audio.SetGroupVolume("Ui", 0)
		Audio.PlayOneShot(clip, nil, 1, "Ui")
		local muted = Test.CaptureAudio(4)
		Test.Expect(muted.Peak < levels.Peak, "muted groups are quieter")
	end)
end)
