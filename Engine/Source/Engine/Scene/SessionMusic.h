#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <vector>

// The play session's music (Architecture §10.2; M40, Docs/Decisions/0057-m40-music.md decisions 4 and 5; World Tree's brief
// 20): what Audio.PlayMusic, Audio.StopMusic and Audio.GetMusic drive, owned by the play session rather than by a scene, so
// a track plays on through Scene.Load and Scene.Reload.

namespace Engine {

	class AssetManager;

	// The longest fade of Audio.PlayMusic and Audio.StopMusic, in seconds.
	inline constexpr double MaxMusicFadeSeconds = 60.0;
	// The most voices the session's music holds at once: the current one and up to three fading out; a voice past the
	// cap releases the oldest fading one at once.
	inline constexpr uint32_t MaxMusicVoices = 4;

	// Audio.PlayMusic's request: `Clip` (valid) at `Volume` (linear, finite, >= 0), reached over `FadeSeconds` (finite, 0 to
	// MaxMusicFadeSeconds; 0 at once), looping or played once, after `Intro`, a clip played once before it (null for none;
	// see "Voices").
	struct MusicRequest
	{
		AssetHandle Clip{};
		float Volume = 1.0f;
		double FadeSeconds = 0.0;
		bool Loop = true;
		AssetHandle Intro{};
	};

	// Audio.GetMusic: the session's current request, as scripts asked for it (never the voice's progress).
	struct MusicState
	{
		AssetHandle Clip{};
		float Volume = 1.0f;
		bool Loop = true;
		AssetHandle Intro{};

		bool operator==(const MusicState&) const = default;
	};

	// One voice of the music: its engine handle, the clip it was started for (the request's, even when a missing clip plays
	// the silent placeholder) and the intro it played before it (null for none).
	struct MusicVoice
	{
		AudioVoiceHandle Voice{};
		AssetHandle Clip{};
		AssetHandle Intro{};

		bool operator==(const MusicVoice&) const = default;
	};

	// The music's live voices (Inspect), or as of the last Update (GetVoices; Test.GetMusic).
	struct MusicVoices
	{
		// The current request's voice; a null Voice when there is no request, or its voice ended or could not start.
		MusicVoice Current{};
		// Its engine snapshot (AudioEngine::GetVoiceInfo) when Current has a voice.
		std::optional<AudioVoiceInfo> CurrentInfo{};
		// The voices fading out to their release, oldest first.
		std::vector<MusicVoice> FadingOut{};
	};

	struct SessionMusicSpecification
	{
		// The context's audio engine (a documented back-reference that outlives the music); null for a play session without
		// one (see "Without audio").
		AudioEngine* Audio = nullptr;
		// The context's asset manager, through which clips load (GetOrPlaceholder: a missing clip plays the silent clip and
		// records its diagnostic once); null: every clip is missing (silent), with one warning per clip.
		AssetManager* Assets = nullptr;
	};

	// The session's music. Not copyable or movable; main thread only (§4.11).
	//
	// Voices. Every voice is non-spatial, in the Music group, at AudioSystemMusicPriority (Scene/AudioSystem.h), with its
	// Volume 1 and its level on the engine's per-voice fade (AudioEngine.h "Fades"), so a level moves smoothly from wherever
	// it is. Play with no current request starts a voice of the clip whose level rises from 0 to Volume over FadeSeconds (at
	// Volume at once for 0). Play with the clip of the current request whose voice still plays keeps that voice, never
	// restarting it: its level fades from where it is to Volume (which also ducks) and Loop is applied. Play with another clip
	// crossfades: the current voice fades from its level to 0 over FadeSeconds and is released at the fade's end (at once for
	// 0), while the new clip's voice fades in; a voice of the new clip that is still fading out is reclaimed and fades back up
	// to Volume instead of a new one starting. Play of the current request's clip whose voice ended (a clip played once) or
	// could not start starts it again. A voice that starts with an Intro plays the intro once, then the clip from the first
	// frame after the intro's last (an engine chained voice, AudioEngine.h "Chained voices"), Loop looping the clip; the
	// intro matters only when a voice starts, and a missing intro or clip (a placeholder) plays the clip, or its placeholder,
	// alone. Stop fades the current voice out the same way and ends the request. The voices the engine fails to start (a
	// pool of higher-priority voices, a clip that cannot be registered) are logged, never returned, so no script's flow
	// depends on which voices the device has finished; the request stands all the same.
	//
	// Holds. SetPaused pauses every voice of the music (and makes new ones start paused) or resumes them; a paused voice's
	// fade, and so its release, holds. The play session pauses it from its activation to its first AudioUpdate phase and
	// while it is paused and not in lockstep, never at a scene transition and never for the world pause.
	//
	// Without audio. A play session without an audio engine (the feature host's scratch sessions, which discover test
	// suites by starting each suite's scene, and engine contexts without one) still has its music, so a scene's startup
	// callbacks ask for their track there as anywhere: Play and Stop check and record the request (GetState; an Intro's
	// format included), and no voice starts, so GetVoices and Inspect stay empty.
	//
	// Clips load and register through the music's own registrations (Scene/Private/AudioClipRegistry.h), so a scene's
	// AudioSystem going never drops them. Destroying the music releases every voice it started, then its registrations.
	class SessionMusic
	{
	public:
		explicit SessionMusic(const SessionMusicSpecification& specification);
		~SessionMusic();

		SessionMusic(const SessionMusic&) = delete;
		SessionMusic& operator=(const SessionMusic&) = delete;

		// The request's checks without acting on them: ValidateMusicRequest, then an Intro's format (see Play), which loads both
		// clips. Errors: those of Play.
		[[nodiscard]] Status Check(const MusicRequest& request);
		// The request's checks (Check), then the voices of "Voices". Errors: InvalidArgument from
		// ValidateMusicRequest, and for an Intro whose sample rate or channel count differs from the Clip's (naming both; both
		// loaded, neither missing), whether or not a voice starts.
		[[nodiscard]] Status Play(const MusicRequest& request);
		// Fades the current voice out over `fadeSeconds` (finite, 0 to MaxMusicFadeSeconds; 0 releases it at once) and ends the
		// request; nothing without one. Errors: InvalidArgument for `fadeSeconds` out of range.
		[[nodiscard]] Status Stop(double fadeSeconds);
		// The current request; nullopt before the first Play and after Stop.
		[[nodiscard]] const std::optional<MusicState>& GetState() const;

		// See "Holds". Idempotent.
		void SetPaused(bool paused);
		[[nodiscard]] bool IsPaused() const;

		// The AudioUpdate phase's work, after the frame's pulls: forgets the voices the engine released (a fade's end, a clip's
		// end, a steal) and takes the snapshot GetVoices returns.
		void Update();
		// The snapshot of the last Update (empty before the first): what Test.GetMusic reads, the same in every run of a test
		// run, which owns audio time.
		[[nodiscard]] const MusicVoices& GetVoices() const;
		// The voices now, without the ones the engine has released since (audio.stats).
		[[nodiscard]] MusicVoices Inspect() const;
	private:
		// The engine and its registrations, the request, the current voice, the voices fading out and the snapshot
		// (SessionMusic.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// Audio.PlayMusic's and Audio.StopMusic's checks: InvalidArgument for a null clip, a Volume that is not finite or below 0,
	// or a fade (`fadeSeconds`, MusicRequest::FadeSeconds) that is not finite or outside 0 to MaxMusicFadeSeconds. Pure.
	[[nodiscard]] Status ValidateMusicRequest(const MusicRequest& request);
	[[nodiscard]] Status ValidateMusicFade(double fadeSeconds);

	// The engine frames of a fade of `seconds` (round(seconds x 48000), half up), `seconds` clamped to 0 to
	// MaxMusicFadeSeconds (NaN: 0). Pure.
	[[nodiscard]] uint64_t GetMusicFadeFrames(double seconds);

}
