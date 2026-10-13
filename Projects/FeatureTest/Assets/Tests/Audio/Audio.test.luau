--!strict
-- Audio.scene (Architecture §15.5, §10): non-spatial and spatial sources, mixer groups, one-shots, a streamed clip
-- (Tone.mp3 imports with Stream) and procedural .sfx clips, asserted with Test.CaptureAudio. Test runs pull the mix by
-- simulation time: 48000 / 64 = 750 frames per tick at the project's 64 Hz, decoded deterministically, so the levels are
-- exact properties of the clips. The listener sits at the origin facing -Z, so +X is its right.

-- Tone.flac: 0.1 s of a 440 Hz sine at amplitude 0.5 (MakeAudioFixtures.py), mono, so a centred source puts the same
-- signal on both channels.
local TONE_PEAK = 0.5
local TONE_RMS = 0.5 / math.sqrt(2)

local function find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity, `Audio.scene has an entity named {name}`)
	return entity
end

local function source(name: string): AudioSource
	local audioSource = find(name).AudioSource
	assert(audioSource, `{name} has an AudioSource`)
	return audioSource
end

-- Plays `name` from its start and captures `ticks` ticks of the mix, then stops it.
local function capture(name: string, ticks: number): TestAudioLevels
	local audioSource = source(name)
	audioSource:Play()
	local levels = Test.CaptureAudio(ticks)
	audioSource:Stop()
	return levels
end

local function load(path: string): AssetRef
	local asset = Assets.Load(path)
	assert(asset, `{path} loads`)
	return asset
end

return Test.Suite("Audio", function()
	Test.Case("a source with PlayOnStart plays from the first tick and ends with its clip", function()
		Test.Expect(source("Intro"):IsPlaying(), "Intro started with the session")
		local opening = Test.CaptureAudio(4)
		Test.Expect(opening.Peak > 0.1, `the Blip sound effect is audible (peak {opening.Peak})`)
		Test.WaitTicks(8)
		Test.Expect(not source("Intro"):IsPlaying(), "the 0.1 s clip has ended")
		local after = Test.CaptureAudio(2)
		Test.ExpectEqual(after.Peak, 0, "nothing plays after the clip ended")
	end)

	Test.Case("a centred non-spatial source plays its clip at its own level", function()
		-- Four ticks (0.0625 s) lie inside the 0.1 s tone.
		local levels = capture("Effect", 4)
		Test.ExpectNear(levels.Peak, TONE_PEAK, 0.01)
		Test.ExpectNear(levels.RmsLeft, TONE_RMS, 0.01)
		Test.ExpectNear(levels.RmsRight, TONE_RMS, 0.01)
		source("Effect").Volume = 0.5
		local half = capture("Effect", 4)
		Test.ExpectNear(half.RmsLeft, levels.RmsLeft * 0.5, 0.005, "Volume scales the level linearly")
		source("Effect").Volume = 1
	end)

	Test.Case("mixer group volumes scale the sources of their group only", function()
		Test.ExpectEqual(Audio.GetGroupVolume("Music"), 1)
		Test.ExpectEqual(Audio.GetGroupVolume("Sfx"), 1)
		Test.ExpectEqual(Audio.GetGroupVolume("Ui"), 1)
		Audio.SetGroupVolume("Music", 0)
		Test.ExpectEqual(Audio.GetGroupVolume("Music"), 0)
		Test.ExpectEqual(capture("Music", 4).Peak, 0, "a muted Music group silences the Music source")
		Test.ExpectNear(capture("Interface", 4).RmsLeft, TONE_RMS, 0.01, "the Ui group still plays")
		Audio.SetGroupVolume("Ui", 0.25)
		Test.ExpectNear(capture("Interface", 4).RmsLeft, TONE_RMS * 0.25, 0.005, "the Ui group plays at a quarter")
		Test.ExpectNear(capture("Effect", 4).RmsLeft, TONE_RMS, 0.01, "the Sfx group is unchanged")
		Audio.SetGroupVolume("Music", 1)
		Audio.SetGroupVolume("Ui", 1)
		Test.ExpectNear(capture("Music", 4).RmsLeft, TONE_RMS, 0.01, "the restored Music group plays again")
	end)

	Test.Case("spatial sources pan towards their side and fade with distance", function()
		local left = capture("Left", 8)
		local right = capture("Right", 8)
		Test.Expect(left.RmsLeft > 2 * left.RmsRight, `a source at the left is louder on the left ({left.RmsLeft} / {left.RmsRight})`)
		Test.Expect(right.RmsRight > 2 * right.RmsLeft, `a source at the right is louder on the right ({right.RmsLeft} / {right.RmsRight})`)
		Test.ExpectNear(left.RmsLeft, right.RmsRight, 0.001, "mirrored sources are mirrored")
		local near = capture("Near", 8)
		local far = capture("Far", 8)
		Test.Expect(near.Peak > 0.05, `a source 2 m away is audible (peak {near.Peak})`)
		Test.ExpectEqual(far.Peak, 0, "a Linear source beyond its MaxDistance is silent")
		source("Far").Spatial = false
		Test.Expect(capture("Far", 8).Peak > 0.05, "the same source without spatialization is audible")
		source("Far").Spatial = true
	end)

	Test.Case("one-shots play once, centred or at a position, in their group", function()
		local tone = load("Assets/Formats/Audio/Tone.flac")
		Audio.PlayOneShot(tone)
		local centred = Test.CaptureAudio(4)
		Test.ExpectNear(centred.RmsLeft, TONE_RMS, 0.01)
		Test.ExpectNear(centred.RmsRight, TONE_RMS, 0.01)
		Test.WaitTicks(4)
		Test.ExpectEqual(Test.CaptureAudio(2).Peak, 0, "the one-shot ended with its clip")
		Audio.PlayOneShot(tone, vector.create(-4, 0, 0), 0.5)
		local placed = Test.CaptureAudio(4)
		Test.Expect(placed.RmsLeft > 2 * placed.RmsRight, "a one-shot at the left plays on the left")
		Test.WaitTicks(4)
		Audio.SetGroupVolume("Ui", 0)
		Audio.PlayOneShot(tone, nil, 1, "Ui")
		Test.ExpectEqual(Test.CaptureAudio(4).Peak, 0, "a one-shot in a muted group is silent")
		Audio.SetGroupVolume("Ui", 1)
		Test.WaitTicks(4)
	end)

	Test.Case("a streamed clip and procedural sound effects play", function()
		local streamed = capture("Streamed", 32)
		Test.Expect(streamed.Peak > 0.05, `the streamed MP3 is audible (peak {streamed.Peak})`)
		local synth = capture("Synth", 4)
		Test.Expect(synth.Peak > 0.05, `the Blip sound effect is audible (peak {synth.Peak})`)
		local chime = load("Assets/Formats/Audio/Chime.sfx")
		Audio.PlayOneShot(chime)
		Test.Expect(Test.CaptureAudio(8).Peak > 0.05, "the Chime sound effect plays as a one-shot")
		Test.WaitTicks(16)
	end)

	Test.Case("pitch stretches a clip in time", function()
		-- At pitch 0.5 the 0.1 s tone lasts 0.2 s: still audible 0.125 to 0.19 s after it starts, where it would
		-- already have ended at pitch 1.
		local slow = source("Slow")
		slow:Play()
		Test.WaitTicks(8)
		Test.Expect(Test.CaptureAudio(4).Peak > 0.1, "the slowed tone still plays")
		slow:Stop()
		local effect = source("Effect")
		effect:Play()
		Test.WaitTicks(8)
		Test.ExpectEqual(Test.CaptureAudio(4).Peak, 0, "the tone at pitch 1 has ended")
		effect:Stop()
	end)
end)
