#include "EnginePCH.h"
#include "Engine/Scene/SessionMusic.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/AudioSystem.h"
#include "Engine/Scene/Private/AudioClipRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>

// Scene is a simulation-path module (§4.12): nothing here uses a CRT transcendental function. Audio is not simulated state,
// so nothing here reaches the state hash; Audio.GetMusic reports only the requests scripts made.

namespace Engine {

	struct SessionMusic::State
	{
		State(AudioEngine* audio, AssetManager* assets)
			: Audio(audio), Clips(audio, assets, "SessionMusic")
		{
		}

		AudioEngine* Audio = nullptr; // documented back-reference; null: requests only (SessionMusic.h "Without audio")
		AudioClipRegistry Clips;
		std::optional<MusicState> Request{};
		// The current request's voice (a null Voice without one) and the voices fading out to their release, oldest first.
		MusicVoice Current{};
		std::vector<MusicVoice> FadingOut{};
		bool Paused = false;
		MusicVoices Snapshot{};

		// Forgets the voices the engine released (a fade's or a clip's end, a steal); whether it forgot any.
		bool Prune();
		// Drops the registrations of the clips no voice and no request uses any more (the request's clip and intro stay
		// registered, so asking for them again never decodes them again).
		void ReleaseUnused();
		// Stops `voice` when it is alive.
		void Release(const MusicVoice& voice);
		// Fades `voice` out over `frames` to its release (at once for 0), keeping it among FadingOut until then.
		void FadeOut(const MusicVoice& voice, uint64_t frames);
		// Releases the oldest voices fading out until `added` more voices fit within MaxMusicVoices.
		void MakeRoom(uint32_t added);
		// Fades the playing `voice` from its level to `volume` over `frames` and applies `loop`.
		void Adjust(const MusicVoice& voice, float volume, uint64_t frames, bool loop);
		// Whether the voice of `request` starts with its Intro: both clips loaded, neither a placeholder. Errors:
		// InvalidArgument for clips of different sample rates or channel counts.
		[[nodiscard]] Result<bool> PrepareIntro(const MusicRequest& request);
		// Starts the voice of `request` as the current one, after its intro when `intro`, its level rising from 0 over
		// `frames` (at the request's Volume at once for 0); a failure is logged and leaves no current voice.
		void Start(const MusicRequest& request, bool intro, uint64_t frames);
	};

	bool SessionMusic::State::Prune()
	{
		if (Audio == nullptr)
			return false;
		bool forgot = false;
		if (!Current.Voice.IsNull() && !Audio->IsVoiceAlive(Current.Voice))
		{
			Current = MusicVoice{};
			forgot = true;
		}
		const size_t released = std::erase_if(FadingOut, [this](const MusicVoice& voice)
		{
			return !Audio->IsVoiceAlive(voice.Voice);
		});
		return forgot || released > 0;
	}

	void SessionMusic::State::ReleaseUnused()
	{
		if (Audio == nullptr)
			return;
		std::vector<AudioClipHandle> kept;
		const auto keepVoice = [this, &kept](const MusicVoice& voice)
		{
			if (Result<AudioVoiceInfo> info = Audio->GetVoiceInfo(voice.Voice); info.has_value())
			{
				kept.push_back(info->Clip);
				if (!info->NextClip.IsNull())
					kept.push_back(info->NextClip);
			}
		};
		keepVoice(Current);
		for (const MusicVoice& voice : FadingOut)
			keepVoice(voice);
		if (Request.has_value())
		{
			for (const AssetHandle clip : { Request->Clip, Request->Intro })
			{
				if (!clip.IsValid())
					continue;
				if (const std::optional<AudioClipHandle> registered = Clips.Find(clip); registered.has_value())
					kept.push_back(*registered);
			}
		}
		Clips.Retain(kept);
	}

	void SessionMusic::State::Release(const MusicVoice& voice)
	{
		if (Audio == nullptr || !Audio->IsVoiceAlive(voice.Voice))
			return;
		if (const Status stopped = Audio->StopVoice(voice.Voice); !stopped)
			ENGINE_CORE_WARN("SessionMusic: stopping a voice failed: {}", stopped.error().ToString());
	}

	void SessionMusic::State::FadeOut(const MusicVoice& voice, uint64_t frames)
	{
		if (frames == 0)
		{
			Release(voice);
			return;
		}
		if (const Status faded = Audio->FadeVoice(voice.Voice, 0.0f, frames, AudioFadeEnd::Release); !faded)
		{
			ENGINE_CORE_WARN("SessionMusic: fading a voice out failed, so it stops at once: {}", faded.error().ToString());
			Release(voice);
			return;
		}
		FadingOut.push_back(voice);
	}

	void SessionMusic::State::MakeRoom(uint32_t added)
	{
		const auto held = [this]()
		{
			return static_cast<uint32_t>(FadingOut.size()) + (Current.Voice.IsNull() ? 0u : 1u);
		};
		while (!FadingOut.empty() && held() + added > MaxMusicVoices)
		{
			Release(FadingOut.front());
			FadingOut.erase(FadingOut.begin());
		}
	}

	void SessionMusic::State::Adjust(const MusicVoice& voice, float volume, uint64_t frames, bool loop)
	{
		if (const Status faded = Audio->FadeVoice(voice.Voice, volume, frames, AudioFadeEnd::Hold); !faded)
			ENGINE_CORE_WARN("SessionMusic: fading the music failed: {}", faded.error().ToString());
		Result<AudioVoiceInfo> info = Audio->GetVoiceInfo(voice.Voice);
		if (!info || info->Settings.Loop == loop)
			return;
		AudioVoiceSettings settings = info->Settings;
		settings.Loop = loop;
		if (const Status applied = Audio->SetVoiceSettings(voice.Voice, settings); !applied)
			ENGINE_CORE_WARN("SessionMusic: applying Loop to the music failed: {}", applied.error().ToString());
	}

	Result<bool> SessionMusic::State::PrepareIntro(const MusicRequest& request)
	{
		if (!request.Intro.IsValid())
			return false;
		const ResolvedAudioClip intro = Clips.Resolve(request.Intro);
		const ResolvedAudioClip clip = Clips.Resolve(request.Clip);
		if (AudioClipRegistry::IsPlaceholder(intro) || AudioClipRegistry::IsPlaceholder(clip))
			return false;
		if (intro.Data->SampleRate != clip.Data->SampleRate || intro.Data->ChannelCount != clip.Data->ChannelCount)
		{
			return MakeError(ErrorCode::InvalidArgument,
				"the music's intro {} ({} Hz, {} channel(s)) and its clip {} ({} Hz, {} channel(s)) differ: the clip follows its intro "
				"seamlessly, so both share one sample rate and channel count",
				Clips.GetName(request.Intro), intro.Data->SampleRate, intro.Data->ChannelCount, Clips.GetName(request.Clip), clip.Data->SampleRate,
				clip.Data->ChannelCount);
		}
		return true;
	}

	void SessionMusic::State::Start(const MusicRequest& request, bool intro, uint64_t frames)
	{
		MakeRoom(1);
		const Result<AudioClipHandle> clip = Clips.Acquire(request.Clip);
		if (!clip)
		{
			ENGINE_CORE_WARN("SessionMusic: the music {} cannot play: {}", request.Clip.ToString(), clip.error().ToString());
			return;
		}
		AudioVoiceDescription description;
		description.Clip = *clip;
		if (intro)
		{
			// The intro is the voice's head and the clip follows it (AudioEngine.h "Chained voices").
			const Result<AudioClipHandle> opening = Clips.Acquire(request.Intro);
			if (!opening)
			{
				ENGINE_CORE_WARN("SessionMusic: the intro {} cannot play: {}", request.Intro.ToString(), opening.error().ToString());
				return;
			}
			description.Clip = *opening;
			description.Next = *clip;
		}
		description.Settings.Group = AudioGroup::Music;
		description.Settings.Loop = request.Loop;
		description.Priority = AudioSystemMusicPriority;
		description.StartPaused = Paused;
		// The engine's level is a float; ValidateMusicRequest keeps Volume within its range.
		const auto level = static_cast<float>(request.Volume);
		description.Fade = frames == 0 ? AudioVoiceFade{ .From = level, .To = level, .Frames = 0 } : AudioVoiceFade{ .From = 0.0f, .To = level, .Frames = frames };
		const Result<AudioVoiceHandle> voice = Audio->PlayVoice(description);
		if (!voice)
		{
			ENGINE_CORE_WARN("SessionMusic: the music {} cannot play: {}", request.Clip.ToString(), voice.error().ToString());
			return;
		}
		Current = MusicVoice{ .Voice = *voice, .Clip = request.Clip, .Intro = intro ? request.Intro : AssetHandle() };
	}

	SessionMusic::SessionMusic(const SessionMusicSpecification& specification)
		: m_State(CreateScope<State>(specification.Audio, specification.Assets))
	{
	}

	SessionMusic::~SessionMusic()
	{
		// Every voice first, then the registrations (AudioClipRegistry), so no voice outlives the session.
		State& state = *m_State;
		state.Release(state.Current);
		for (const MusicVoice& voice : state.FadingOut)
			state.Release(voice);
		state.Clips.Clear();
	}

	Status SessionMusic::Check(const MusicRequest& request)
	{
		ENGINE_TRY(ValidateMusicRequest(request));
		ENGINE_TRY(m_State->PrepareIntro(request));
		return {};
	}

	Status SessionMusic::Play(const MusicRequest& request)
	{
		ENGINE_TRY(ValidateMusicRequest(request));
		State& state = *m_State;
		// The intro's format is checked on every call, so a call's error never depends on whether its voice still plays or on
		// whether the session has audio.
		ENGINE_TRY_ASSIGN(const bool intro, state.PrepareIntro(request));
		state.Request = MusicState{ .Clip = request.Clip, .Volume = request.Volume, .Loop = request.Loop, .Intro = request.Intro };
		// Without audio the request is all (SessionMusic.h, "Without audio").
		if (state.Audio == nullptr)
			return {};
		state.Prune();
		const uint64_t frames = GetMusicFadeFrames(request.FadeSeconds);
		if (!state.Current.Voice.IsNull() && state.Current.Clip == request.Clip)
		{
			// The same music: its voice is kept, its level and Loop follow the request.
			state.Adjust(state.Current, static_cast<float>(request.Volume), frames, request.Loop);
		}
		else
		{
			// The newest voice of the clip that is still fading out is reclaimed, rather than a new one started.
			std::optional<MusicVoice> reclaimed;
			const auto found = std::find_if(state.FadingOut.rbegin(), state.FadingOut.rend(), [&request](const MusicVoice& voice)
			{
				return voice.Clip == request.Clip;
			});
			if (found != state.FadingOut.rend())
			{
				reclaimed = *found;
				state.FadingOut.erase(std::next(found).base());
			}
			if (!state.Current.Voice.IsNull())
				state.FadeOut(std::exchange(state.Current, MusicVoice{}), frames);
			if (reclaimed.has_value())
			{
				state.Current = *reclaimed;
				state.Adjust(state.Current, static_cast<float>(request.Volume), frames, request.Loop);
			}
			else
			{
				state.Start(request, intro, frames);
			}
		}
		state.ReleaseUnused();
		return {};
	}

	Status SessionMusic::Stop(double fadeSeconds)
	{
		ENGINE_TRY(ValidateMusicFade(fadeSeconds));
		State& state = *m_State;
		state.Prune();
		if (!state.Request.has_value())
			return {};
		state.Request.reset();
		if (!state.Current.Voice.IsNull())
			state.FadeOut(std::exchange(state.Current, MusicVoice{}), GetMusicFadeFrames(fadeSeconds));
		state.ReleaseUnused();
		return {};
	}

	const std::optional<MusicState>& SessionMusic::GetState() const
	{
		return m_State->Request;
	}

	void SessionMusic::SetPaused(bool paused)
	{
		State& state = *m_State;
		if (state.Paused == paused)
			return;
		state.Paused = paused;
		const auto apply = [&state, paused](const MusicVoice& voice)
		{
			if (state.Audio == nullptr || !state.Audio->IsVoiceAlive(voice.Voice))
				return;
			if (const Status set = state.Audio->SetVoicePaused(voice.Voice, paused); !set)
				ENGINE_CORE_WARN("SessionMusic: pausing a voice failed: {}", set.error().ToString());
		};
		apply(state.Current);
		for (const MusicVoice& voice : state.FadingOut)
			apply(voice);
	}

	bool SessionMusic::IsPaused() const
	{
		return m_State->Paused;
	}

	void SessionMusic::Update()
	{
		State& state = *m_State;
		if (state.Prune())
			state.ReleaseUnused();
		state.Snapshot = Inspect();
	}

	const MusicVoices& SessionMusic::GetVoices() const
	{
		return m_State->Snapshot;
	}

	MusicVoices SessionMusic::Inspect() const
	{
		const State& state = *m_State;
		MusicVoices voices;
		if (state.Audio == nullptr)
			return voices;
		if (Result<AudioVoiceInfo> info = state.Audio->GetVoiceInfo(state.Current.Voice); info.has_value())
		{
			voices.Current = state.Current;
			voices.CurrentInfo = std::move(*info);
		}
		for (const MusicVoice& voice : state.FadingOut)
		{
			if (state.Audio->IsVoiceAlive(voice.Voice))
				voices.FadingOut.push_back(voice);
		}
		return voices;
	}

	Status ValidateMusicFade(double fadeSeconds)
	{
		if (!std::isfinite(fadeSeconds) || fadeSeconds < 0.0 || fadeSeconds > MaxMusicFadeSeconds)
			return MakeError(ErrorCode::InvalidArgument, "the music's fade must be finite and from 0 to {} seconds (got {})", MaxMusicFadeSeconds, fadeSeconds);
		return {};
	}

	Status ValidateMusicRequest(const MusicRequest& request)
	{
		if (!request.Clip.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "the music needs a clip");
		if (!std::isfinite(request.Volume) || request.Volume < 0.0 || request.Volume > static_cast<double>(std::numeric_limits<float>::max()))
			return MakeError(ErrorCode::InvalidArgument, "the music's Volume must be finite and at least 0 (got {})", request.Volume);
		return ValidateMusicFade(request.FadeSeconds);
	}

	uint64_t GetMusicFadeFrames(double seconds)
	{
		// Clamped into the valid range, so even an unvalidated value converts safely.
		const double clamped = seconds > 0.0 ? std::min(seconds, MaxMusicFadeSeconds) : 0.0;
		return static_cast<uint64_t>(std::floor(clamped * static_cast<double>(AudioSampleRate) + 0.5));
	}

}
