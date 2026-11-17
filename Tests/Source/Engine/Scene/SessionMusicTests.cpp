#include "TestsPCH.h"
#include "Engine/Scene/SessionMusic.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AudioTestData.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// The session's music (Architecture §10.2; M40, Docs/Decisions/0057-m40-music.md decisions 4 and 5; World Tree's brief 20):
// keeping, ducking, crossfading, reclaiming, the voice cap and stopping on a device-less engine whose frames the tests pull,
// and the music kept through a session's Scene.Load and Scene.Reload.

namespace Engine {

	namespace {

		constexpr AssetHandle TrackA(0x4000a1);
		constexpr AssetHandle TrackB(0x4000a2);
		constexpr AssetHandle TrackC(0x4000a3);
		constexpr AssetHandle TrackD(0x4000a4);
		constexpr AssetHandle TrackE(0x4000a5);
		constexpr AssetHandle Jingle(0x4000a6);
		constexpr AssetHandle Opening(0x4000a7);
		constexpr AssetHandle Odd(0x4000a8);

		// A mono Pcm16 tone of `seconds` at `sampleRate`, as a synthesized sound effect is (at 48 kHz).
		AssetRef<AudioClipData> MakeToneClip(double seconds, double amplitude, uint32_t sampleRate = 48000)
		{
			const Test::TestToneSpecification tone{ .SampleRate = sampleRate,
				.ChannelCount = 1,
				.FrameCount = static_cast<uint64_t>(seconds * static_cast<double>(sampleRate)),
				.Frequency = 440.0,
				.Amplitude = amplitude };
			auto clip = CreateRef<AudioClipData>();
			clip->Encoding = AudioClipEncoding::Pcm16;
			clip->SampleRate = tone.SampleRate;
			clip->ChannelCount = tone.ChannelCount;
			clip->FrameCount = tone.FrameCount;
			clip->Bytes = Test::MakeTonePcm16Bytes(tone);
			return clip;
		}

		// A device-less engine, five 10 s tracks and a 0.1 s jingle, and the music over them. The music goes first, so no
		// voice outlives the engine.
		class MusicFixture
		{
		public:
			MusicFixture()
			{
				Result<Scope<AudioEngine>> created = AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, Vfs);
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				Audio = std::move(*created);
				Assets.Publish(TrackA, MakeToneClip(10.0, 0.5), "Assets/Audio/A.sfx");
				Assets.Publish(TrackB, MakeToneClip(10.0, 0.4), "Assets/Audio/B.sfx");
				Assets.Publish(TrackC, MakeToneClip(10.0, 0.3), "Assets/Audio/C.sfx");
				Assets.Publish(TrackD, MakeToneClip(10.0, 0.2), "Assets/Audio/D.sfx");
				Assets.Publish(TrackE, MakeToneClip(10.0, 0.1), "Assets/Audio/E.sfx");
				Assets.Publish(Jingle, MakeToneClip(0.1, 0.5), "Assets/Audio/Jingle.sfx");
				Music = CreateScope<SessionMusic>(SessionMusicSpecification{ .Audio = Audio.get(), .Assets = &Assets });
			}

			~MusicFixture() { Music.reset(); }

			MusicFixture(const MusicFixture&) = delete;
			MusicFixture& operator=(const MusicFixture&) = delete;

			void Pull(uint64_t frames) const
			{
				std::vector<float> samples(static_cast<size_t>(frames) * AudioChannelCount);
				const Status read = Audio->ReadFrames(samples);
				REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			}

			[[nodiscard]] AudioVoiceInfo Info(AudioVoiceHandle voice) const
			{
				Result<AudioVoiceInfo> info = Audio->GetVoiceInfo(voice);
				REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
				return std::move(*info);
			}

			// The current voice, after an Update; fails the test case without one.
			[[nodiscard]] AudioVoiceHandle Current() const
			{
				Music->Update();
				const MusicVoices& voices = Music->GetVoices();
				REQUIRE(voices.CurrentInfo.has_value());
				return voices.Current.Voice;
			}

			VirtualFileSystem Vfs{};
			Test::InMemoryAssetManager Assets{};
			Scope<AudioEngine> Audio{};
			Scope<SessionMusic> Music{};
		};

		std::vector<AudioVoiceHandle> GetHandles(const MusicVoices& voices)
		{
			std::vector<AudioVoiceHandle> handles;
			for (const MusicVoice& voice : voices.FadingOut)
				handles.push_back(voice.Voice);
			return handles;
		}

		void AttachScript(Entity entity, AssetHandle script)
		{
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			entity.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("SessionMusic: PlayMusic starts its clip in the Music group at music priority and fades it in")
		{
			MusicFixture fixture;
			REQUIRE(fixture.Music->Play({ .Clip = TrackA, .Volume = 0.8f, .FadeSeconds = 0.1, .Loop = true }).has_value());
			CHECK(fixture.Music->GetState() == MusicState{ .Clip = TrackA, .Volume = 0.8f, .Loop = true });
			const std::vector<AudioVoiceInfo> voices = fixture.Audio->GetVoices();
			REQUIRE(voices.size() == 1);
			const AudioVoiceInfo& started = voices.front();
			CHECK(started.Settings.Group == AudioGroup::Music);
			CHECK(started.Priority == AudioSystemMusicPriority);
			CHECK_FALSE(started.Settings.Spatial);
			CHECK(started.Settings.Loop);
			CHECK(started.Settings.Volume == 1.0f);
			CHECK(started.Owner == 0);
			CHECK(started.FadeVolume == 0.0f);
			CHECK(started.FadeTarget == 0.8f);
			// 0.1 s is 4,800 frames: halfway at 2,400, then held at the volume.
			fixture.Pull(2400);
			const float halfway = fixture.Info(started.Voice).FadeVolume;
			CHECK(halfway == 0.4f);
			fixture.Pull(2400);
			const AudioVoiceInfo full = fixture.Info(started.Voice);
			CHECK(full.FadeVolume == 0.8f);
			CHECK(full.FadeTarget == 0.8f);
			CHECK(fixture.Current() == started.Voice);
			const MusicVoices& snapshot = fixture.Music->GetVoices();
			CHECK(snapshot.Current.Clip == TrackA);
			CHECK(snapshot.CurrentInfo->CursorFrames == 4800);
			CHECK(snapshot.FadingOut.empty());

			// Without a fade, a new clip starts at its volume at once.
			REQUIRE(fixture.Music->Play({ .Clip = TrackB, .Volume = 0.5f }).has_value());
			const AudioVoiceInfo atOnce = fixture.Info(fixture.Current());
			CHECK(atOnce.FadeVolume == 0.5f);
			CHECK(fixture.Audio->GetVoices().size() == 1);
		}

		TEST_CASE("SessionMusic: the same clip keeps its voice, fading it from its level to the new volume and applying Loop")
		{
			MusicFixture fixture;
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			const AudioVoiceHandle voice = fixture.Current();
			fixture.Pull(4800);
			// Ducked to 0.25 over 0.05 s (2,400 frames), played once from now on.
			REQUIRE(fixture.Music->Play({ .Clip = TrackA, .Volume = 0.25f, .FadeSeconds = 0.05, .Loop = false }).has_value());
			CHECK(fixture.Current() == voice);
			const AudioVoiceInfo ducking = fixture.Info(voice);
			CHECK(ducking.FadeVolume == 1.0f);
			CHECK(ducking.FadeTarget == 0.25f);
			CHECK_FALSE(ducking.Settings.Loop);
			fixture.Pull(1200);
			const float halfway = fixture.Info(voice).FadeVolume;
			CHECK(halfway == 0.625f);
			fixture.Pull(1200);
			const AudioVoiceInfo ducked = fixture.Info(voice);
			CHECK(ducked.FadeVolume == 0.25f);
			// The voice played on: no restart, no second voice.
			CHECK(ducked.CursorFrames == 7200);
			CHECK(fixture.Audio->GetStats().StartedVoices == 1);
			CHECK(fixture.Music->GetState() == MusicState{ .Clip = TrackA, .Volume = 0.25f, .Loop = false });
		}

		TEST_CASE("SessionMusic: another clip crossfades over FadeSeconds")
		{
			MusicFixture fixture;
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			const AudioVoiceHandle first = fixture.Current();
			fixture.Pull(4800);
			REQUIRE(fixture.Music->Play({ .Clip = TrackB, .FadeSeconds = 0.1 }).has_value());
			const AudioVoiceHandle second = fixture.Current();
			CHECK(second != first);
			CHECK(GetHandles(fixture.Music->GetVoices()) == std::vector<AudioVoiceHandle>{ first });
			const AudioVoiceInfo out = fixture.Info(first);
			CHECK(out.FadeVolume == 1.0f);
			CHECK(out.FadeTarget == 0.0f);
			CHECK(out.FadeEnd == AudioFadeEnd::Release);
			const AudioVoiceInfo in = fixture.Info(second);
			CHECK(in.FadeVolume == 0.0f);
			CHECK(in.FadeTarget == 1.0f);
			fixture.Pull(2400);
			const float outHalfway = fixture.Info(first).FadeVolume;
			const float inHalfway = fixture.Info(second).FadeVolume;
			CHECK(outHalfway == 0.5f);
			CHECK(inHalfway == 0.5f);
			fixture.Pull(2400);
			CHECK_FALSE(fixture.Audio->IsVoiceAlive(first));
			const float arrived = fixture.Info(second).FadeVolume;
			CHECK(arrived == 1.0f);
			CHECK(fixture.Current() == second);
			CHECK(fixture.Music->GetVoices().FadingOut.empty());
			CHECK(fixture.Music->GetState()->Clip == TrackB);

			// With no fade, the playing voice stops at once and the new one starts at its volume.
			REQUIRE(fixture.Music->Play({ .Clip = TrackC, .Volume = 0.5f }).has_value());
			CHECK_FALSE(fixture.Audio->IsVoiceAlive(second));
			CHECK(fixture.Audio->GetVoices().size() == 1);
			CHECK(fixture.Music->GetVoices().FadingOut.empty());
		}

		TEST_CASE("SessionMusic: a voice still fading out is reclaimed when its clip is asked for again")
		{
			MusicFixture fixture;
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			const AudioVoiceHandle first = fixture.Current();
			REQUIRE(fixture.Music->Play({ .Clip = TrackB, .FadeSeconds = 0.1 }).has_value());
			const AudioVoiceHandle second = fixture.Current();
			fixture.Pull(2400);
			// Back through the doorway: A, halfway out at 0.5, comes back up from there, and B goes out from its 0.5.
			REQUIRE(fixture.Music->Play({ .Clip = TrackA, .FadeSeconds = 0.1 }).has_value());
			CHECK(fixture.Current() == first);
			CHECK(GetHandles(fixture.Music->GetVoices()) == std::vector<AudioVoiceHandle>{ second });
			const AudioVoiceInfo reclaimed = fixture.Info(first);
			CHECK(reclaimed.FadeVolume == 0.5f);
			CHECK(reclaimed.FadeTarget == 1.0f);
			CHECK(reclaimed.FadeEnd == AudioFadeEnd::Hold);
			CHECK(reclaimed.CursorFrames == 2400);
			const AudioVoiceInfo leaving = fixture.Info(second);
			CHECK(leaving.FadeVolume == 0.5f);
			CHECK(leaving.FadeTarget == 0.0f);
			CHECK(leaving.FadeEnd == AudioFadeEnd::Release);
			fixture.Pull(4800);
			CHECK_FALSE(fixture.Audio->IsVoiceAlive(second));
			const AudioVoiceInfo back = fixture.Info(first);
			CHECK(back.FadeVolume == 1.0f);
			CHECK(back.CursorFrames == 7200);
			CHECK(fixture.Audio->GetStats().StartedVoices == 2);
		}

		TEST_CASE("SessionMusic: at most four music voices play, the oldest fading one stopping first")
		{
			MusicFixture fixture;
			std::vector<AudioVoiceHandle> started;
			for (const AssetHandle track : { TrackA, TrackB, TrackC, TrackD })
			{
				REQUIRE(fixture.Music->Play({ .Clip = track, .FadeSeconds = 1.0 }).has_value());
				started.push_back(fixture.Current());
			}
			CHECK(fixture.Audio->GetVoices().size() == MaxMusicVoices);
			CHECK(GetHandles(fixture.Music->GetVoices()) == std::vector<AudioVoiceHandle>{ started[0], started[1], started[2] });
			REQUIRE(fixture.Music->Play({ .Clip = TrackE, .FadeSeconds = 1.0 }).has_value());
			CHECK(fixture.Audio->GetVoices().size() == MaxMusicVoices);
			CHECK_FALSE(fixture.Audio->IsVoiceAlive(started[0]));
			fixture.Music->Update();
			CHECK(GetHandles(fixture.Music->GetVoices()) == std::vector<AudioVoiceHandle>{ started[1], started[2], started[3] });
			// Stopping keeps the four: the current one joins those fading out.
			REQUIRE(fixture.Music->Stop(1.0).has_value());
			CHECK(fixture.Audio->GetVoices().size() == MaxMusicVoices);
			fixture.Music->Update();
			CHECK(fixture.Music->GetVoices().FadingOut.size() == MaxMusicVoices);
		}

		TEST_CASE("SessionMusic: StopMusic fades the music out and ends the request")
		{
			MusicFixture fixture;
			// Nothing to stop: nothing happens.
			REQUIRE(fixture.Music->Stop(0.5).has_value());
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			const AudioVoiceHandle voice = fixture.Current();
			REQUIRE(fixture.Music->Stop(0.05).has_value());
			CHECK_FALSE(fixture.Music->GetState().has_value());
			const AudioVoiceInfo stopping = fixture.Info(voice);
			CHECK(stopping.FadeTarget == 0.0f);
			CHECK(stopping.FadeEnd == AudioFadeEnd::Release);
			fixture.Music->Update();
			CHECK_FALSE(fixture.Music->GetVoices().CurrentInfo.has_value());
			CHECK(fixture.Music->GetVoices().FadingOut.size() == 1);
			fixture.Pull(2400);
			CHECK(fixture.Audio->GetVoices().empty());

			// Without a fade the voice stops at once.
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			REQUIRE(fixture.Music->Stop(0.0).has_value());
			CHECK(fixture.Audio->GetVoices().empty());
			for (const double fade : { -0.5, MaxMusicFadeSeconds + 1.0, std::numeric_limits<double>::quiet_NaN() })
			{
				const Status refused = fixture.Music->Stop(fade);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("SessionMusic: a clip played once stays the request and starts again when asked for after it ended")
		{
			MusicFixture fixture;
			REQUIRE(fixture.Music->Play({ .Clip = Jingle, .Loop = false }).has_value());
			const AudioVoiceHandle once = fixture.Current();
			// Its 4,800 frames and the pull that finds its end.
			fixture.Pull(5600);
			CHECK_FALSE(fixture.Audio->IsVoiceAlive(once));
			fixture.Music->Update();
			CHECK_FALSE(fixture.Music->GetVoices().CurrentInfo.has_value());
			CHECK(fixture.Music->GetState() == MusicState{ .Clip = Jingle, .Volume = 1.0f, .Loop = false });
			REQUIRE(fixture.Music->Play({ .Clip = Jingle, .Loop = false }).has_value());
			const AudioVoiceHandle again = fixture.Current();
			CHECK(again != once);
			CHECK(fixture.Info(again).CursorFrames == 0);
		}

		TEST_CASE("SessionMusic: paused, its voices and their fades hold and new voices start paused")
		{
			MusicFixture fixture;
			fixture.Music->SetPaused(true);
			CHECK(fixture.Music->IsPaused());
			REQUIRE(fixture.Music->Play({ .Clip = TrackA, .FadeSeconds = 0.1 }).has_value());
			const AudioVoiceHandle voice = fixture.Current();
			fixture.Pull(4800);
			const AudioVoiceInfo held = fixture.Info(voice);
			CHECK(held.Paused);
			CHECK(held.CursorFrames == 0);
			CHECK(held.FadeVolume == 0.0f);
			fixture.Music->SetPaused(false);
			fixture.Pull(2400);
			const AudioVoiceInfo playing = fixture.Info(voice);
			CHECK_FALSE(playing.Paused);
			CHECK(playing.CursorFrames == 2400);
			CHECK(playing.FadeVolume == 0.5f);
			// A voice fading out holds too.
			REQUIRE(fixture.Music->Play({ .Clip = TrackB, .FadeSeconds = 0.1 }).has_value());
			fixture.Music->SetPaused(true);
			fixture.Pull(9600);
			CHECK(fixture.Audio->IsVoiceAlive(voice));
			CHECK(fixture.Info(voice).Paused);
		}

		TEST_CASE("SessionMusic: invalid requests and fades are InvalidArgument and change nothing")
		{
			MusicFixture fixture;
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float infinity = std::numeric_limits<float>::infinity();
			const MusicRequest refused[] = {
				{ .Clip = AssetHandle() },
				{ .Clip = TrackA, .Volume = -0.5f },
				{ .Clip = TrackA, .Volume = nan },
				{ .Clip = TrackA, .Volume = infinity },
				{ .Clip = TrackA, .FadeSeconds = -1.0 },
				{ .Clip = TrackA, .FadeSeconds = MaxMusicFadeSeconds + 0.5 },
				{ .Clip = TrackA, .FadeSeconds = std::numeric_limits<double>::infinity() },
			};
			for (const MusicRequest& request : refused)
			{
				const Status played = fixture.Music->Play(request);
				REQUIRE_FALSE(played.has_value());
				CHECK(played.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK_FALSE(fixture.Music->GetState().has_value());
			CHECK(fixture.Audio->GetVoices().empty());
			REQUIRE(fixture.Music->Play({ .Clip = TrackA, .Volume = 0.0f, .FadeSeconds = MaxMusicFadeSeconds }).has_value());
			CHECK(GetMusicFadeFrames(1.0) == 48000);
			CHECK(GetMusicFadeFrames(0.5 / 48000.0) == 1);
			CHECK(GetMusicFadeFrames(MaxMusicFadeSeconds) == 2'880'000);
			CHECK(GetMusicFadeFrames(MaxMusicFadeSeconds * 2.0) == 2'880'000);
			CHECK(GetMusicFadeFrames(-1.0) == 0);
			CHECK(GetMusicFadeFrames(std::numeric_limits<double>::quiet_NaN()) == 0);
		}

		TEST_CASE("SessionMusic: a voice the engine cannot start is logged and the request stands")
		{
			MusicFixture fixture;
			// Every voice of the pool at a priority above music's (the editor's preview has 2).
			const Result<AudioClipHandle> clip = fixture.Audio->RegisterClip(MakeAudioClipSource(TrackE, 1, MakeToneClip(1.0, 0.1)));
			REQUIRE(clip.has_value());
			for (uint32_t index = 0; index < MaxAudioVoices; ++index)
				REQUIRE(fixture.Audio->PlayVoice({ .Clip = *clip, .Priority = AudioSystemMusicPriority + 1 }).has_value());
			{
				const Test::ExpectLog logged(LogLevel::Warn, "SessionMusic: the music");
				REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			}
			CHECK(fixture.Music->GetState()->Clip == TrackA);
			fixture.Music->Update();
			CHECK_FALSE(fixture.Music->GetVoices().CurrentInfo.has_value());
			for (const AudioVoiceInfo& voice : fixture.Audio->GetVoices())
				REQUIRE(fixture.Audio->StopVoice(voice.Voice).has_value());
			REQUIRE(fixture.Audio->UnregisterClip(*clip).has_value());
			// Asked for again once there is room, the clip starts.
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			CHECK(fixture.Audio->GetVoices().size() == 1);
		}

		// Decision 5: a play session without an audio engine (the feature host's scratch sessions) still has its music.
		TEST_CASE("SessionMusic: without an audio engine, requests are checked and recorded and no voice starts")
		{
			Test::InMemoryAssetManager assets;
			assets.Publish(TrackA, MakeToneClip(1.0, 0.5), "Assets/Audio/A.sfx");
			assets.Publish(Odd, MakeToneClip(0.05, 0.75, 44100), "Assets/Audio/Odd.sfx");
			SessionMusic music(SessionMusicSpecification{ .Audio = nullptr, .Assets = &assets });
			const Status refused = music.Play({ .Clip = TrackA, .Volume = -1.0f });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			// An intro of another format is refused as with audio (W40.2).
			const Status odd = music.Play({ .Clip = TrackA, .Intro = Odd });
			REQUIRE_FALSE(odd.has_value());
			CHECK(odd.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK_FALSE(music.GetState().has_value());
			REQUIRE(music.Play({ .Clip = TrackA, .Volume = 0.5f, .FadeSeconds = 1.0, .Loop = false }).has_value());
			REQUIRE(music.GetState().has_value());
			CHECK(*music.GetState() == MusicState{ .Clip = TrackA, .Volume = 0.5f, .Loop = false });
			music.SetPaused(true);
			CHECK(music.IsPaused());
			music.Update();
			CHECK(music.GetVoices().Current.Voice.IsNull());
			CHECK_FALSE(music.GetVoices().CurrentInfo.has_value());
			CHECK(music.Inspect().FadingOut.empty());
			const Status badFade = music.Stop(-1.0);
			REQUIRE_FALSE(badFade.has_value());
			CHECK(badFade.error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(music.Stop(0.5).has_value());
			CHECK_FALSE(music.GetState().has_value());
		}

		// The W40.1 review's follow-up: a track stays registered only while a voice or the request uses it, so a run through
		// many scenes' tracks keeps the ones playing, not every one it ever played.
		TEST_CASE("SessionMusic: a track's registration is released once no voice or request uses it")
		{
			MusicFixture fixture;
			const auto registered = [&fixture]()
			{
				return fixture.Audio->GetStats().RegisteredClips;
			};
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			CHECK(registered() == 1);
			// At once: A's voice stops and A is no longer asked for.
			REQUIRE(fixture.Music->Play({ .Clip = TrackB }).has_value());
			CHECK(registered() == 1);
			// A crossfade keeps B registered while its voice fades out, then releases it.
			REQUIRE(fixture.Music->Play({ .Clip = TrackC, .FadeSeconds = 0.5 }).has_value());
			CHECK(registered() == 2);
			fixture.Pull(24000 + 480);
			fixture.Music->Update();
			CHECK(registered() == 1);
			// A clip played once that ended stays registered while it is the request, and goes with it.
			REQUIRE(fixture.Music->Play({ .Clip = Jingle, .Loop = false }).has_value());
			fixture.Pull(4800 + 480);
			fixture.Music->Update();
			CHECK_FALSE(fixture.Music->GetVoices().CurrentInfo.has_value());
			CHECK(registered() == 1);
			REQUIRE(fixture.Music->Stop(0.0).has_value());
			CHECK(registered() == 0);
		}

		TEST_CASE("SessionMusic: destroying it releases every voice and every clip it registered")
		{
			MusicFixture fixture;
			REQUIRE(fixture.Music->Play({ .Clip = TrackA }).has_value());
			REQUIRE(fixture.Music->Play({ .Clip = TrackB, .FadeSeconds = 1.0 }).has_value());
			CHECK(fixture.Audio->GetVoices().size() == 2);
			CHECK(fixture.Audio->GetStats().RegisteredClips == 2);
			fixture.Music.reset();
			CHECK(fixture.Audio->GetVoices().empty());
			CHECK(fixture.Audio->GetStats().RegisteredClips == 0);
		}

		// M40 (decision 11): an intro then its clip, chained by the engine; a missing intro plays the clip alone, and an intro of
		// another format is refused on every call.
		TEST_CASE("SessionMusic: an intro plays once, then its clip loops from the first frame after it")
		{
			MusicFixture fixture;
			fixture.Assets.Publish(Opening, MakeToneClip(0.05, 0.75), "Assets/Audio/Opening.sfx");
			fixture.Assets.Publish(Odd, MakeToneClip(0.05, 0.75, 44100), "Assets/Audio/Odd.sfx");
			REQUIRE(fixture.Music->Play({ .Clip = TrackA, .Intro = Opening }).has_value());
			CHECK(fixture.Music->GetState() == MusicState{ .Clip = TrackA, .Volume = 1.0f, .Loop = true, .Intro = Opening });
			const AudioVoiceHandle voice = fixture.Current();
			CHECK(fixture.Music->GetVoices().Current == MusicVoice{ .Voice = voice, .Clip = TrackA, .Intro = Opening });
			const AudioVoiceInfo opening = fixture.Info(voice);
			CHECK_FALSE(opening.InNext);
			CHECK_FALSE(opening.NextClip.IsNull());
			CHECK(opening.Settings.Loop);
			fixture.Pull(2400 + 100);
			const AudioVoiceInfo looping = fixture.Info(voice);
			CHECK(looping.InNext);
			CHECK(looping.CursorFrames == 100);

			// Kept: an intro matters only when a voice starts; the request reports it as asked.
			REQUIRE(fixture.Music->Play({ .Clip = TrackA, .Volume = 0.5f, .Intro = Opening }).has_value());
			CHECK(fixture.Current() == voice);
			const bool stillLooping = fixture.Info(voice).InNext;
			CHECK(stillLooping);

			// An intro of another format is refused, whether or not a voice would start, and changes nothing.
			for (const AssetHandle clip : { TrackA, TrackB })
			{
				const Status refused = fixture.Music->Play({ .Clip = clip, .Intro = Odd });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			// The W40.2 review: a decoded file (32-bit float) never chains to a sound effect (16-bit PCM), either way round.
			auto file = CreateRef<AudioClipData>();
			file->Encoding = AudioClipEncoding::Wav;
			file->SampleRate = 48000;
			file->ChannelCount = 1;
			file->FrameCount = 2400;
			file->Bytes = Test::MakeToneWav({ .FrameCount = 2400 });
			const AssetHandle decoded(0x4000a9);
			fixture.Assets.Publish(decoded, file, "Assets/Audio/Opening.wav");
			for (const MusicRequest& request : { MusicRequest{ .Clip = TrackA, .Intro = decoded }, MusicRequest{ .Clip = decoded, .Intro = Opening } })
			{
				const Status refused = fixture.Music->Play(request);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK(fixture.Music->GetState()->Volume == 0.5f);
			CHECK(fixture.Current() == voice);

			// A missing intro plays the clip alone.
			const AssetHandle missing(0x4000ff);
			const Test::ExpectLog logged(LogLevel::Error, missing.ToString());
			REQUIRE(fixture.Music->Play({ .Clip = TrackB, .Intro = missing }).has_value());
			const AudioVoiceHandle alone = fixture.Current();
			CHECK(fixture.Info(alone).NextClip.IsNull());
			CHECK(fixture.Music->GetVoices().Current.Intro == AssetHandle());
			CHECK(fixture.Music->GetState()->Intro == missing);
		}

		// The brief's check: walking a doorway (Scene.Load) and retrying (Scene.Reload) keep the track playing, the same
		// voice with its cursor past the doorway's tick, because each scene asks for the clip already playing.
		TEST_CASE("SessionMusic: the same clip keeps its voice through Scene.Load and Scene.Reload")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle conductor(0x4000b1), next(0x4000b2);
			fixture.GetAssetManager().Publish(TrackA, MakeToneClip(10.0, 0.5), "Assets/Audio/Realm.sfx");
			REQUIRE(fixture.AddScript(conductor, "Assets/Scripts/Conductor.luau", R"(
local C = {}
function C:OnStart()
	Audio.PlayMusic(Assets.Load("Assets/Audio/Realm.sfx"), { FadeSeconds = 0.5 })
end
function C:OnFixedUpdate()
	if Time.GetTick() == 20 and Scene.GetCurrent() == nil then
		Scene.Load(Assets.Load("Assets/Scenes/Arena2.scene"))
	end
end
return Script.Define("Conductor", C)
)"));
			// The next arena asks for the same track.
			Scene& scene = fixture.GetScene();
			AttachScript(scene.CreateEntity("Conductor"), conductor);
			Result<Json> document = SceneSerializer::ToJson(scene);
			REQUIRE(document.has_value());
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			fixture.GetAssetManager().Publish(next, data, "Assets/Scenes/Arena2.scene");

			VirtualFileSystem vfs;
			Result<Scope<AudioEngine>> createdAudio = AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, vfs);
			REQUIRE_MESSAGE(createdAudio.has_value(), createdAudio.error().ToString());
			AudioEngine& audio = **createdAudio;
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 40;
			specification.Audio = &audio;
			// A test run: the session owns audio time, so every tick pulls 800 frames.
			specification.OwnsAudioTime = true;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			PlaySession& session = **started;
			session.SetExtractionEnabled(false);
			SessionMusic* music = session.GetMusic();
			REQUIRE(music != nullptr);

			Test::RunTicks(session, 20);
			const MusicVoices before = music->GetVoices();
			REQUIRE(before.CurrentInfo.has_value());
			CHECK(before.Current.Clip == TrackA);
			CHECK(before.CurrentInfo->CursorFrames == 20 * 800);
			const float levelBefore = before.CurrentInfo->FadeVolume;
			CHECK(levelBefore > 0.0f);

			// The doorway: the load requested at tick 20 applies at that frame's end, and the arena's OnStart asks for the track.
			Test::RunTicks(session, 10);
			REQUIRE(session.GetSceneGeneration() == 2);
			const MusicVoices through = music->GetVoices();
			REQUIRE(through.CurrentInfo.has_value());
			CHECK(through.Current.Voice == before.Current.Voice);
			CHECK(through.CurrentInfo->CursorFrames == 30 * 800);
			CHECK(through.CurrentInfo->FadeVolume > levelBefore);
			CHECK(through.FadingOut.empty());
			CHECK(audio.GetStats().StartedVoices == 1);

			// A retry: Scene.Reload keeps it too.
			REQUIRE(session.GetScripts()->Evaluate("Scene.Reload()", {}));
			Test::RunTicks(session, 5);
			REQUIRE(session.GetSceneGeneration() == 3);
			const MusicVoices retried = music->GetVoices();
			REQUIRE(retried.CurrentInfo.has_value());
			CHECK(retried.Current.Voice == before.Current.Voice);
			CHECK(retried.CurrentInfo->CursorFrames == 35 * 800);
			CHECK(audio.GetStats().StartedVoices == 1);
			CHECK(session.GetScriptErrors().GetErrors().empty());
		}

		// Phase 6's landing (M38 and M40 share PlaySession and IScriptHost in disjoint regions, the ledger): one Scene.Load keeps
		// both the session's locale, which the new scene's VM starts in (Docs/Decisions/0055-m38-runtime-locale.md decision 1),
		// and the session's music, whose voice plays on (Docs/Decisions/0057-m40-music.md decision 5).
		TEST_CASE("PlaySession: a Scene.Load keeps both the session's locale and its music")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle conductor(0x4000b4), next(0x4000b5);
			fixture.GetAssetManager().Publish(TrackA, MakeToneClip(10.0, 0.5), "Assets/Audio/Realm.sfx");
			REQUIRE(fixture.AddScript(conductor, "Assets/Scripts/Conductor.luau", R"(
local C = {}
function C:OnCreate() self.CreatedIn = Loc.GetLocale() end
function C:OnStart()
	Audio.PlayMusic(Assets.Load("Assets/Audio/Realm.sfx"))
end
function C:OnFixedUpdate()
	if self.CreatedIn ~= "fr" then return end
	if Time.GetTick() == 3 then Loc.SetLocale("de") end
	if Time.GetTick() == 5 then Scene.Load(Assets.Load("Assets/Scenes/Hall.scene")) end
end
return Script.Define("Conductor", C)
)"));
			// The hall holds the same conductor, which asks for the track playing.
			Scene& scene = fixture.GetScene();
			AttachScript(scene.CreateEntity("Conductor"), conductor);
			Result<Json> document = SceneSerializer::ToJson(scene);
			REQUIRE(document.has_value());
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			fixture.GetAssetManager().Publish(next, data, "Assets/Scenes/Hall.scene");

			VirtualFileSystem vfs;
			Result<Scope<AudioEngine>> createdAudio = AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, vfs);
			REQUIRE_MESSAGE(createdAudio.has_value(), createdAudio.error().ToString());
			AudioEngine& audio = **createdAudio;
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 40;
			specification.Audio = &audio;
			specification.OwnsAudioTime = true;
			specification.Project.Localization.Locale = "fr";
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			PlaySession& session = **started;
			session.SetExtractionEnabled(false);
			SessionMusic* music = session.GetMusic();
			REQUIRE(music != nullptr);

			Test::RunTicks(session, 4);
			CHECK(session.GetLocale() == "de");
			const MusicVoices before = music->GetVoices();
			REQUIRE(before.CurrentInfo.has_value());
			Test::RunTicks(session, 6);
			REQUIRE(session.GetSceneGeneration() == 2);
			CHECK(session.GetScriptErrors().GetErrors().empty());
			CHECK(session.GetLocale() == "de");
			CHECK(session.GetStartLocale() == "fr");
			const UUID hallConductor = session.GetScene().FindEntityByPath("/Conductor").GetUUID();
			const Result<ScriptEvaluation> createdIn = session.GetScripts()->Evaluate("return self.CreatedIn", {}, hallConductor);
			REQUIRE_MESSAGE(createdIn.has_value(), createdIn.error().ToString());
			CHECK(createdIn->Value.Get() == Json("de"));
			const MusicVoices through = music->GetVoices();
			REQUIRE(through.CurrentInfo.has_value());
			CHECK(through.Current.Voice == before.Current.Voice);
			CHECK(through.Current.Clip == TrackA);
			CHECK(audio.GetStats().StartedVoices == 1);
		}

		// Decisions 5 and 6: the feature host discovers test suites in scratch sessions without audio, whose scene's startup
		// callbacks ask for their track and set the mix as in any session: the music records the request, and the mixer calls
		// are checked, then ignored, GetGroupVolume reading 1 (the W40.1 review's follow-up).
		TEST_CASE("SessionMusic: a play session without an audio engine records its music and checks then ignores its mixer calls")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle conductor(0x4000b3);
			fixture.GetAssetManager().Publish(TrackA, MakeToneClip(1.0, 0.5), "Assets/Audio/Realm.sfx");
			REQUIRE(fixture.AddScript(conductor, "Assets/Scripts/Conductor.luau", R"(
local C = {}
function C:OnStart()
	Audio.PlayMusic(Assets.Load("Assets/Audio/Realm.sfx"), { Volume = 0.25 })
	Audio.SetGroupVolume("Music", 0.5)
	Audio.PlayOneShot(Assets.Load("Assets/Audio/Realm.sfx"), nil, 1, "Music")
	assert(Audio.GetGroupVolume("Music") == 1)
	assert(not pcall(Audio.SetGroupVolume, "Music", -1))
	assert(not pcall(Audio.PlayOneShot, Assets.Load("Assets/Audio/Realm.sfx"), nil, 0 / 0))
end
return Script.Define("Conductor", C)
)"));
			Scene& scene = fixture.GetScene();
			AttachScript(scene.CreateEntity("Conductor"), conductor);
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 40;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, scene);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			PlaySession& session = **started;
			session.SetExtractionEnabled(false);
			CHECK(session.GetAudioSystem() == nullptr);
			SessionMusic* music = session.GetMusic();
			REQUIRE(music != nullptr);
			Test::RunTicks(session, 3);
			CHECK(session.GetScriptErrors().GetErrors().empty());
			REQUIRE(music->GetState().has_value());
			CHECK(music->GetState()->Clip == TrackA);
			CHECK(music->GetState()->Volume == 0.25f);
			CHECK_FALSE(music->GetVoices().CurrentInfo.has_value());
			REQUIRE(session.GetScripts()->Evaluate("Audio.StopMusic(0.5)", {}));
			CHECK_FALSE(music->GetState().has_value());
			CHECK(session.GetScriptErrors().GetErrors().empty());
		}
	}

}
