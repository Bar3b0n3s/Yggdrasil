#include "EnginePCH.h"
#include "Engine/Automation/Methods/AudioMethods.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SessionMusic.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// A voice handle as audio.stats reports it: 16 hexadecimal digits.
		static std::string FormatAudioVoice(AudioVoiceHandle voice)
		{
			return std::format("{:016x}", voice.GetValue());
		}

		// The {id, path, type} of a voice's clip: its registration name is the clip's AssetHandle (AudioSystem and the preview
		// always register so), expanded through the host's asset manager when it has one; the name alone otherwise.
		static AssetSummary SummarizeAudioClip(const AssetManager* assets, const std::string& clipName)
		{
			AssetSummary summary;
			const std::optional<UUID> handle = UUID::FromString(clipName);
			if (!handle.has_value() || !handle->IsValid())
			{
				summary.Id = clipName;
				return summary;
			}
			summary.Id = handle->ToString();
			if (assets != nullptr)
			{
				summary.Path = assets->GetReferencePath(*handle);
				summary.Type = assets->GetAssetType(*handle);
			}
			return summary;
		}

	}

	namespace Automation {

		Result<AudioStatsResult> AudioStats(AutomationMethodContext& context, const NoParams& /*params*/)
		{
			const AudioEngine* audio = context.GetAudioEngine();
			if (audio == nullptr)
				return MakeError(ErrorCode::Unsupported, "this host has no audio engine");

			const AudioEngineStats stats = audio->GetStats();
			AudioStatsResult result;
			result.DeviceState = stats.DeviceState;
			result.DeviceName = stats.DeviceName;
			result.TimeSource = stats.TimeSource;
			result.SampleRate = AudioSampleRate;
			result.MasterVolume = audio->GetMasterVolume();
			for (const AudioGroup group : std::array{ AudioGroup::Music, AudioGroup::Sfx, AudioGroup::Ui })
				result.Groups.push_back(AudioGroupVolume{ .Group = group, .Volume = audio->GetGroupVolume(group) });
			result.VoiceCount = stats.LiveVoices;
			result.VoiceCapacity = stats.VoiceCapacity;
			result.StolenVoices = ToAutomationCounter(stats.StolenVoices);
			result.PulledFrames = ToAutomationCounter(stats.PulledFrames);
			result.DeviceReadFrames = ToAutomationCounter(stats.DeviceReadFrames);

			// The owners of the play session's voices are entities of its scene (AudioVoiceDescription::Owner).
			const PlaySession* session = context.GetPlaySession();
			const AssetManager* assets = context.GetAssets();
			for (const AudioVoiceInfo& voice : audio->GetVoices())
			{
				AudioVoiceSummary summary;
				summary.Voice = Utils::FormatAudioVoice(voice.Voice);
				summary.Clip = Utils::SummarizeAudioClip(assets, voice.ClipName);
				if (voice.Owner != 0 && session != nullptr)
				{
					if (const ConstEntity owner = session->GetScene().FindEntityByID(UUID(voice.Owner)); owner.IsValid())
						summary.Entity = context.MakeEntitySummary(owner);
				}
				summary.Group = voice.Settings.Group;
				summary.Volume = voice.Settings.Volume;
				summary.Pitch = voice.Settings.Pitch;
				summary.Loop = voice.Settings.Loop;
				summary.Spatial = voice.Settings.Spatial;
				if (voice.Settings.Spatial)
					summary.Position = { voice.Transform.Position.x, voice.Transform.Position.y, voice.Transform.Position.z };
				summary.Paused = voice.Paused;
				summary.Streamed = voice.Streamed;
				summary.Priority = voice.Priority;
				summary.CursorFrames = ToAutomationCounter(voice.CursorFrames);
				summary.LengthFrames = ToAutomationCounter(voice.LengthFrames);
				summary.FadeVolume = voice.FadeVolume;
				if (!voice.NextClip.IsNull())
					summary.Next = Utils::SummarizeAudioClip(assets, voice.NextClipName);
				summary.InNext = voice.InNext;
				result.Voices.push_back(std::move(summary));
			}

			// M40 (decision 7): the session's music, its request and its live voices.
			const SessionMusic* music = session != nullptr ? session->GetMusic() : nullptr;
			if (music != nullptr)
			{
				if (const std::optional<MusicState>& request = music->GetState(); request.has_value())
				{
					result.Music.Clip = Utils::SummarizeAudioClip(assets, request->Clip.ToString());
					if (request->Intro.IsValid())
						result.Music.Intro = Utils::SummarizeAudioClip(assets, request->Intro.ToString());
					result.Music.Volume = request->Volume;
					result.Music.Loop = request->Loop;
				}
				const MusicVoices voices = music->Inspect();
				if (voices.CurrentInfo.has_value())
					result.Music.Voice = Utils::FormatAudioVoice(voices.Current.Voice);
				for (const MusicVoice& voice : voices.FadingOut)
					result.Music.FadingVoices.push_back(Utils::FormatAudioVoice(voice.Voice));
			}
			return result;
		}

	}

	void RegisterAudioMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<AudioDeviceState>("AudioDeviceState", "The state of the audio engine's playback device.")
			.Entry(AudioDeviceState::None, "None", "No device by configuration (headless processes): the host pulls the frames.")
			.Entry(AudioDeviceState::Running, "Running", "The device exists and plays.")
			.Entry(AudioDeviceState::Recreating, "Recreating", "The device stopped unexpectedly; a re-creation is scheduled.")
			.Entry(AudioDeviceState::Failed, "Failed", "No device could be created: the engine runs without one.");

		registry.Enum<AudioTimeSource>("AudioTimeSource", "Who advances the audio engine's voices.")
			.Entry(AudioTimeSource::Device, "Device", "The playback device, in real time.")
			.Entry(AudioTimeSource::Simulation, "Simulation", "The play session's ticks (lockstep or a test run): one tick of frames per tick.")
			.Entry(AudioTimeSource::Host, "Host", "The host's frames, without a device.");

		registry.Struct<AudioVoiceSummary>("AudioVoiceSummary", "One live voice of the audio engine.")
			.Field("voice", &AudioVoiceSummary::Voice, "The voice's handle: 16 lowercase hex digits.")
			.Field("clip", &AudioVoiceSummary::Clip, "The clip it plays (the silent clip when the source's clip is missing).")
			.Field("entity", &AudioVoiceSummary::Entity, "The play scene's AudioSource entity it belongs to; empty for one-shots and previews.")
			.Field("group", &AudioVoiceSummary::Group, "The mixer group it plays in.")
			.Field("volume", &AudioVoiceSummary::Volume, "Its linear volume.")
			.Field("pitch", &AudioVoiceSummary::Pitch, "Its playback speed and pitch multiplier.")
			.Field("loop", &AudioVoiceSummary::Loop, "Whether it restarts when the clip ends.")
			.Field("spatial", &AudioVoiceSummary::Spatial, "Whether it is positioned and attenuated with distance.")
			.Field("position", &AudioVoiceSummary::Position, "Its world position [x, y, z] in metres; empty for a non-spatial voice.")
			.Field("paused", &AudioVoiceSummary::Paused, "Whether it is paused (a paused play session, or a paused source).")
			.Field("streamed", &AudioVoiceSummary::Streamed, "Whether its clip streams instead of being decoded once.")
			.Field("priority", &AudioVoiceSummary::Priority, "Its stealing priority: 0 effects, 1 music, 2 previews; lower is stolen first.")
			.Field("cursorFrames", &AudioVoiceSummary::CursorFrames, "The clip's frames played so far, at the clip's own sample rate.")
			.Field("lengthFrames", &AudioVoiceSummary::LengthFrames, "The clip's length in its own frames; 0 while not known yet.")
			.Field("fadeVolume", &AudioVoiceSummary::FadeVolume,
				"Its level now, a multiplier of its volume that a fade moves (the session's music fades in, out and between tracks); 1 "
				"for a voice that never faded.")
			.Field("next", &AudioVoiceSummary::Next,
				"The clip that follows the voice's clip, sample-accurately (the loop of the session's music after its intro); empty for a "
				"voice without one.")
			.Field("inNext", &AudioVoiceSummary::InNext, "Whether the voice plays the clip that follows (its cursor and length are then that clip's).");

		registry.Struct<AudioMusicSummary>("AudioMusicSummary", "The play session's music (Audio.PlayMusic).")
			.Field("clip", &AudioMusicSummary::Clip, "The clip of the current request; empty without music and outside a Play session.")
			.Field("intro", &AudioMusicSummary::Intro, "The intro of the current request, played once before its clip; empty for none.")
			.Field("volume", &AudioMusicSummary::Volume, "The current request's linear volume; 0 without music.")
			.Field("loop", &AudioMusicSummary::Loop, "Whether the current request loops; false without music.")
			.Field("voice", &AudioMusicSummary::Voice,
				"The current request's voice (a voice of voices); empty without music, and when its clip played once has ended or its "
				"voice could not start.")
			.Field("fadingVoices", &AudioMusicSummary::FadingVoices, "The music's voices fading out to their end, oldest first.");

		registry.Struct<AudioGroupVolume>("AudioGroupVolume", "One mixer group's volume.")
			.Field("group", &AudioGroupVolume::Group, "The group.")
			.Field("volume", &AudioGroupVolume::Volume, "Its linear volume.");

		registry.Struct<AudioStatsResult>("AudioStatsResult", "The state of the host's audio engine.")
			.Field("deviceState", &AudioStatsResult::DeviceState, "The playback device's state.")
			.Field("deviceName", &AudioStatsResult::DeviceName, "The playback device's name; empty without a device.")
			.Field("timeSource", &AudioStatsResult::TimeSource, "Who advances the voices.")
			.Field("sampleRate", &AudioStatsResult::SampleRate, "The mixing rate in Hz (48000).")
			.Field("masterVolume", &AudioStatsResult::MasterVolume, "The master volume over every group.")
			.Field("groups", &AudioStatsResult::Groups, "The volumes of the Music, Sfx and Ui groups, in that order.")
			.Field("voiceCount", &AudioStatsResult::VoiceCount, "The live voices.")
			.Field("voiceCapacity", &AudioStatsResult::VoiceCapacity, "The most voices that play at once (64).")
			.Field("stolenVoices", &AudioStatsResult::StolenVoices, "Voices stopped to make room for a new one since the engine started.")
			.Field("pulledFrames", &AudioStatsResult::PulledFrames, "Frames the engine mixed for simulation time or without a device.")
			.Field("deviceReadFrames", &AudioStatsResult::DeviceReadFrames, "Frames the playback device read.")
			.Field("voices", &AudioStatsResult::Voices, "Every live voice, oldest first.")
			.Field("music", &AudioStatsResult::Music, "The play session's music: its request and its voices.");
	}

	void RegisterAudioMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "audio.stats",
				.Description = "Reports the host's audio engine: its device and time source, the master and group volumes, every voice with "
							   "its clip, owning entity, group, volume, level, position and cursor (the play session's voices, and the asset "
							   "browser's preview in the editor), and the play session's music.",
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the voices of a lockstep session.", .Params = Json::object() } },
			},
			&Automation::AudioStats);
	}

}
