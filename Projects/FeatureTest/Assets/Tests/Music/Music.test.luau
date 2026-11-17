--!strict
-- The session's music (M40, Docs/Decisions/0057-m40-music.md decision 9; World Tree's brief 20). MusicA.scene and
-- MusicB.scene each ask for LoopA when they start (MusicConductor), so the track plays on through a Scene.Load and a
-- Scene.Reload: the same voice, its cursor past the doorway's tick. A case that requests a transition returns in that
-- frame and the next case runs in the new scene's VM, so what a case compares across one travels in the load
-- parameters. Test runs pull the mix by simulation time, 48000 / 64 = 750 frames per tick, and Test.GetMusic reads the
-- music's voice as of the last AudioUpdate phase: a case that resumes at a tick reads the ticks before it.
local LOOP_A = "Assets/Audio/Music/LoopA.sfx"
local LOOP_B = "Assets/Audio/Music/LoopB.sfx"
-- A 0.25 s square wave at 0.75 (12,000 frames, 16 ticks; 24575 / 32768 in 16-bit PCM), and Tone.flac's 0.1 s (4,800
-- frames) imported again to stream.
local INTRO_A = "Assets/Audio/Music/IntroA.sfx"
local INTRO_LEVEL = 24575 / 32768
local STREAMED_TONE = "Assets/Audio/Music/Tone.flac"
-- 8 s square waves at 0.5 and 0.25: their RMS is their level.
local LOOP_A_LEVEL = 0.5
local LOOP_B_LEVEL = 0.25
local FRAMES_PER_TICK = 750
-- The scenes' fade-in, 0.5 s, and the suite's own fades, 0.25 s, in frames.
local SCENE_FADE_FRAMES = 24000
local FADE_FRAMES = 12000

local function load(path: string): AssetRef
	local asset = Assets.Load(path)
	assert(asset, `{path} loads`)
	return asset
end

-- The session's music as scripts asked for it; fails the case without it.
local function request(): MusicState
	local music = Audio.GetMusic()
	assert(music, "the session has music")
	return music
end

-- The music's voice as of the last AudioUpdate phase; fails the case without one.
local function voice(): TestMusic
	local music = Test.GetMusic()
	assert(music, "the session's music has a voice")
	return music
end

local function refused(call: () -> (), expected: string)
	local ok, message = pcall(call)
	Test.Expect(not ok and string.find(tostring(message), expected, 1, true) ~= nil, `refused with "{expected}": {message}`)
end

return Test.Suite("Music", function()
	Test.Case("the scene's music fades in from silence on simulation time", function()
		Test.WaitUntil(function()
			return Test.GetMusic() ~= nil
		end, 4)
		local music = Audio.GetMusic()
		assert(music, "MusicA asked for its track")
		Test.ExpectEqual(music.Clip, load(LOOP_A))
		Test.ExpectEqual(music.Volume, 1)
		Test.ExpectEqual(music.Loop, true)
		local rising = voice()
		Test.ExpectEqual(rising.Clip, load(LOOP_A))
		Test.ExpectEqual(rising.LengthFrames, 8 * 48000)
		Test.ExpectEqual(rising.FadingOut, 0)
		-- The fade counts the frames the voice has mixed, as its cursor does: the level is the share of the fade played.
		Test.Expect(rising.FadeVolume < 1, `the fade is under way ({rising.FadeVolume})`)
		Test.ExpectNear(rising.FadeVolume, rising.CursorFrames / SCENE_FADE_FRAMES, 1e-6)
		Test.WaitTicks(8)
		local later = voice()
		Test.ExpectEqual(later.Voice, rising.Voice)
		Test.ExpectEqual(later.CursorFrames, rising.CursorFrames + 8 * FRAMES_PER_TICK, "eight ticks of frames")
		Test.ExpectNear(later.FadeVolume, rising.FadeVolume + 8 * FRAMES_PER_TICK / SCENE_FADE_FRAMES, 1e-6, "a quarter of the fade")
		Test.WaitUntil(function()
			return voice().FadeVolume == 1
		end, 32)
		Test.ExpectNear(Test.CaptureAudio(4).RmsLeft, LOOP_A_LEVEL, 0.01, "LoopA at its full level")
	end)

	Test.Case("asking for the clip already playing keeps its voice and ducks it", function()
		local playing = voice()
		Audio.PlayMusic(load(LOOP_A), { Volume = 0.5, FadeSeconds = 0.25 })
		Test.ExpectEqual(request().Volume, 0.5)
		Test.WaitTicks(1)
		local ducking = voice()
		Test.ExpectEqual(ducking.Voice, playing.Voice, "the same voice, never restarted")
		Test.ExpectNear(ducking.FadeVolume, 1 - 0.5 * FRAMES_PER_TICK / FADE_FRAMES, 1e-6, "falling from where it was")
		Test.WaitTicks(16)
		Test.ExpectEqual(voice().FadeVolume, 0.5)
		Test.ExpectNear(Test.CaptureAudio(4).RmsLeft, LOOP_A_LEVEL * 0.5, 0.01, "LoopA at half its level")
		Audio.PlayMusic(load(LOOP_A))
		Test.WaitTicks(1)
		Test.ExpectEqual(voice().FadeVolume, 1, "without a fade the level is the volume at once")
		Test.ExpectEqual(voice().Voice, playing.Voice)
	end)

	Test.Case("a Scene.Load to a scene that asks for the same clip keeps the voice", function()
		local playing = voice()
		Scene.Load(load("Assets/Scenes/MusicB.scene"), { Voice = playing.Voice, Cursor = playing.CursorFrames })
	end)

	Test.Case("the track plays on through the doorway, its cursor past the doorway's tick", function()
		Test.ExpectEqual(Scene.GetName(), "MusicB")
		local doorway = Scene.GetLoadParameters()
		local through = voice()
		Test.ExpectEqual(through.Voice, doorway.Voice, "the same voice: no restart")
		Test.Expect(through.CursorFrames > doorway.Cursor, `the cursor went on ({through.CursorFrames} after {doorway.Cursor})`)
		Test.ExpectEqual(through.FadingOut, 0)
		Test.ExpectEqual(through.FadeVolume, 1)
		Test.ExpectEqual(request().Clip, load(LOOP_A))
		Scene.Reload({ Voice = through.Voice, Cursor = through.CursorFrames })
	end)

	Test.Case("a Scene.Reload keeps the voice too", function()
		Test.ExpectEqual(Scene.GetName(), "MusicB")
		local retry = Scene.GetLoadParameters()
		local kept = voice()
		Test.ExpectEqual(kept.Voice, retry.Voice, "the same voice after the reload")
		Test.Expect(kept.CursorFrames > retry.Cursor, `the cursor went on ({kept.CursorFrames} after {retry.Cursor})`)
	end)

	Test.Case("another clip crossfades: two voices, then one", function()
		local first = voice()
		Audio.PlayMusic(load(LOOP_B), { FadeSeconds = 0.25 })
		Test.ExpectEqual(request().Clip, load(LOOP_B))
		Test.WaitTicks(1)
		local crossing = voice()
		Test.Expect(crossing.Voice ~= first.Voice, "the new clip has a voice of its own")
		Test.ExpectEqual(crossing.Clip, load(LOOP_B))
		Test.ExpectEqual(crossing.FadingOut, 1, "the old track fades out while the new one fades in")
		Test.ExpectNear(crossing.FadeVolume, FRAMES_PER_TICK / FADE_FRAMES, 1e-6)
		Test.WaitTicks(16)
		local arrived = voice()
		Test.ExpectEqual(arrived.Voice, crossing.Voice)
		Test.ExpectEqual(arrived.FadingOut, 0, "the old track's voice ended with its fade")
		Test.ExpectEqual(arrived.FadeVolume, 1)
		Test.ExpectNear(Test.CaptureAudio(4).RmsLeft, LOOP_B_LEVEL, 0.01, "only LoopB plays")
	end)

	Test.Case("a clip still fading out is taken back when it is asked for again", function()
		local b = voice()
		Audio.PlayMusic(load(LOOP_A), { FadeSeconds = 0.25 })
		Test.WaitTicks(4)
		Audio.PlayMusic(load(LOOP_B), { FadeSeconds = 0.25 })
		Test.WaitTicks(1)
		local back = voice()
		Test.ExpectEqual(back.Voice, b.Voice, "LoopB's voice comes back rather than a restart")
		Test.ExpectEqual(back.FadingOut, 1, "LoopA fades out in its turn")
		-- Four ticks out, 0.75, then one of the way back up.
		Test.ExpectNear(back.FadeVolume, 0.75 + 0.25 * FRAMES_PER_TICK / FADE_FRAMES, 1e-6)
		Test.WaitTicks(16)
		Test.ExpectEqual(voice().FadingOut, 0)
		Test.ExpectEqual(voice().FadeVolume, 1)
	end)

	Test.Case("a streamed track plays as music", function()
		local tone = load("Assets/Formats/Audio/Tone.mp3")
		Audio.PlayMusic(tone)
		Test.WaitTicks(2)
		Test.ExpectEqual(voice().Clip, tone)
		Test.Expect(Test.CaptureAudio(8).Peak > 0.05, "the streamed MP3 is audible")
		Test.WaitTicks(1)
		Test.ExpectEqual(voice().FadingOut, 0, "LoopB stopped at once without a fade")
	end)

	Test.Case("StopMusic fades the music out and GetMusic returns nil", function()
		Audio.PlayMusic(load(LOOP_A))
		Audio.StopMusic(0.25)
		Test.ExpectEqual(Audio.GetMusic(), nil)
		Test.WaitTicks(1)
		Test.ExpectEqual(Test.GetMusic(), nil, "no current voice; the last one fades out")
		Test.WaitTicks(16)
		Test.ExpectEqual(Test.CaptureAudio(2).Peak, 0, "the music faded out and stopped")
		Audio.StopMusic()
		Test.ExpectEqual(Audio.GetMusic(), nil, "stopping without music changes nothing")
	end)

	Test.Case("an intro plays once, then its loop from the first frame after it", function()
		local introA = load(INTRO_A)
		Audio.PlayMusic(load(LOOP_B), { Intro = introA })
		Test.ExpectEqual(request().Intro, introA)
		Test.WaitTicks(14)
		local opening = voice()
		Test.Expect(opening.InIntro, "the intro plays first")
		Test.ExpectEqual(opening.Intro, introA)
		Test.ExpectEqual(opening.Clip, load(LOOP_B))
		Test.ExpectEqual(opening.LengthFrames, 12000)
		Test.ExpectEqual(opening.CursorFrames, 14 * FRAMES_PER_TICK)
		-- The intro's last two ticks at its level, then, a tick into the loop, the loop's (the join's own frames are the
		-- engine's unit case's: the mixer delivers the intro's last frame in the loop's first tick).
		Test.ExpectNear(Test.CaptureAudio(2).RmsLeft, INTRO_LEVEL, 1e-6, "the intro to its last frame")
		Test.WaitTicks(1)
		Test.ExpectNear(Test.CaptureAudio(2).RmsLeft, LOOP_B_LEVEL, 1e-6, "the loop after the intro")
		local looping = voice()
		Test.Expect(not looping.InIntro, "the loop plays")
		Test.ExpectEqual(looping.Voice, opening.Voice, "one voice for both")
		Test.ExpectEqual(looping.LengthFrames, 8 * 48000)
		Test.ExpectEqual(looping.CursorFrames, 19 * FRAMES_PER_TICK - 12000)
	end)

	Test.Case("a streamed intro is followed as exactly", function()
		local tone = load(STREAMED_TONE)
		-- The loop is the same tone imported to decode: both are 32-bit float, so they chain (a sound effect's 16-bit PCM
		-- is refused, below). Its level after the join is the intro's, which a buffer read in the wrong format is not.
		Audio.PlayMusic(load("Assets/Formats/Audio/Tone.flac"), { Intro = tone })
		Test.WaitTicks(1)
		local introLevel = Test.CaptureAudio(2)
		Test.WaitTicks(3)
		local opening = voice()
		Test.Expect(opening.InIntro, "the streamed intro plays first")
		Test.ExpectEqual(opening.LengthFrames, 4800)
		Test.ExpectEqual(opening.CursorFrames, 6 * FRAMES_PER_TICK)
		Test.WaitTicks(2)
		local looping = voice()
		Test.Expect(not looping.InIntro, "the loop follows")
		Test.ExpectEqual(looping.Voice, opening.Voice)
		Test.ExpectEqual(looping.CursorFrames, 8 * FRAMES_PER_TICK - 4800)
		local loopLevel = Test.CaptureAudio(2)
		Test.Expect(introLevel.RmsLeft > 0.05, "the streamed intro sounds")
		Test.ExpectNear(loopLevel.RmsLeft, introLevel.RmsLeft, introLevel.RmsLeft * 0.05, "the loop sounds as its intro did")
	end)

	Test.Case("the music, Music one-shots and Ui sounds play on under the world pause while Sfx holds", function()
		local tone = load("Assets/Formats/Audio/Tone.flac")
		Audio.StopMusic()
		Time.PauseWorld()
		Test.WaitTicks(1)
		Test.Expect(Time.IsWorldPaused(), "the world is paused")
		Audio.PlayOneShot(tone, nil, 1, "Sfx")
		Test.ExpectEqual(Test.CaptureAudio(2).Peak, 0, "an Sfx one-shot holds under the world pause")
		Audio.PlayOneShot(tone, nil, 1, "Music")
		Test.Expect(Test.CaptureAudio(2).Peak > 0.4, "a Music one-shot, a stinger, plays under it")
		Test.WaitTicks(8)
		Audio.PlayOneShot(tone, nil, 1, "Ui")
		Test.Expect(Test.CaptureAudio(2).Peak > 0.4, "a Ui one-shot plays under it")
		Test.WaitTicks(8)
		Audio.PlayMusic(load(LOOP_A))
		Test.WaitTicks(1)
		local first = voice()
		Test.WaitTicks(4)
		Test.ExpectEqual(voice().CursorFrames, first.CursorFrames + 4 * FRAMES_PER_TICK, "the session's music plays under it")
		Audio.StopMusic()
		Time.ResumeWorld()
		Test.WaitTicks(1)
		Test.Expect(Test.CaptureAudio(2).Peak > 0.4, "the held Sfx one-shot plays once the world resumes")
		Test.WaitTicks(8)
	end)

	Test.Case("the music calls refuse a nil clip and options out of range", function()
		local loopA = load(LOOP_A)
		refused(function()
			Audio.PlayMusic(nil :: any)
		end, "AssetRef")
		refused(function()
			Audio.PlayMusic(loopA, { Volum = 1 } :: any)
		end, "unknown MusicOptions key 'Volum'")
		refused(function()
			Audio.PlayMusic(loopA, { Volume = -1 })
		end, "Volume must be finite and from 0 to 1")
		refused(function()
			Audio.PlayMusic(loopA, { FadeSeconds = 61 })
		end, "from 0 to 60 seconds")
		refused(function()
			Audio.StopMusic(-1)
		end, "from 0 to 60 seconds")
		refused(function()
			Audio.PlayMusic(loopA, { Intro = load("Assets/Formats/Audio/Tone.mp3") })
		end, "differ")
		refused(function()
			Audio.PlayMusic(loopA, { Intro = load(STREAMED_TONE) })
		end, "sample format")
		Test.ExpectEqual(Audio.GetMusic(), nil, "a refused call changes nothing")
	end)
end)
