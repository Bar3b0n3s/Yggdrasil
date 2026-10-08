#include "EnginePCH.h"
#include "Engine/Audio/AudioEngine.h"

#include "Engine/Audio/AudioDecoder.h"
#include "Engine/Audio/AudioVfs.h"
#include "Engine/Audio/Private/MiniaudioSupport.h"
#include "Engine/Audio/Private/MiniaudioVfs.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"

#include <glm/geometric.hpp>
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// The longest stretch one pull reads before it processes the resource manager's jobs again (10 ms), so a stream
		// always has its next page decoded before it needs it, however many frames a caller pulls at once (a stream page is
		// one second of the clip, MA_RESOURCE_MANAGER_PAGE_SIZE_IN_MILLISECONDS).
		constexpr uint32_t PullChunkFrames = 480;
		// The sample rates of a Pcm16 clip (AudioClipSource).
		constexpr uint32_t MinClipSampleRate = 8000;
		constexpr uint32_t MaxClipSampleRate = 192000;
		// Two directions closer than this sine of their angle are parallel (SetListener).
		constexpr double ParallelSine = 1.0e-6;
		// The largest pitch a voice plays at; a larger AudioVoiceSettings::Pitch is accepted, reported as given and played at
		// this one. miniaudio turns a voice's resampling ratio (the clip's rate over 48 kHz, times the pitch, times the Doppler
		// pitch) into a 32-bit fixed-point rate: an unbounded ratio overflows that conversion (undefined behaviour) or is
		// refused, keeping the voice's previous rate. With this bound and the Doppler bound below, the ratio stays at most
		// 4 x 16 x 3 = 192 (a 192 kHz clip).
		constexpr float MaxEffectivePitch = 16.0f;
		// The Doppler bound: a voice's effective Doppler factor keeps the factor times the speed of the source, and times the
		// speed of the listener, at or below this fraction of the speed of sound. miniaudio's Doppler pitch
		// (c - f vListener) / (c - f vSource) then stays within [1/3, 3], where an unbounded one divides by zero when a
		// source approaches at c / f (a teleport, or a fast source with a large factor).
		constexpr double MaxDopplerSpeedFraction = 0.5;

		// The data miniaudio reads for one registration: the shared bytes of a Pcm16 clip or a stream, or the PCM an Encoded
		// clip decoded to at registration. A registration and every voice of it hold it, so it outlives miniaudio's use.
		struct AudioClipPayload
		{
			Ref<const Buffer> Bytes{}; // Pcm16 and streamed clips
			std::vector<float> Pcm{};  // Encoded clips decoded at registration (interleaved f32)
			ma_format Format = ma_format_unknown;
			uint32_t ChannelCount = 0;
			uint32_t SampleRate = 0;
			uint64_t FrameCount = 0;

			[[nodiscard]] const void* GetData() const
			{
				return Pcm.empty() ? static_cast<const void*>(Bytes->data()) : static_cast<const void*>(Pcm.data());
			}
		};

		// An initialized ma_sound (a voice) or ma_sound_group (ma_sound_group is an ma_sound), uninitialized on destruction.
		// The miniaudio object lives on the heap, so its address (which the node graph keeps) survives moves of the owner
		// (the voice pool's slots move when it grows). Movable, not copyable.
		class SoundNode
		{
		public:
			SoundNode() = default;
			~SoundNode() { Reset(); }

			SoundNode(SoundNode&& other) noexcept = default;
			SoundNode& operator=(SoundNode&& other) noexcept
			{
				if (this != &other)
				{
					Reset();
					m_Sound = std::move(other.m_Sound);
				}
				return *this;
			}
			SoundNode(const SoundNode&) = delete;
			SoundNode& operator=(const SoundNode&) = delete;

			// A voice of the registered resource `name` in `group`.
			[[nodiscard]] static Result<SoundNode> CreateSound(ma_engine& engine, const std::string& name, ma_uint32 flags, ma_sound_group& group)
			{
				Scope<ma_sound> sound = CreateScope<ma_sound>();
				if (const ma_result result = ma_sound_init_from_file(&engine, name.c_str(), flags, &group, nullptr, sound.get()); result != MA_SUCCESS)
					return std::unexpected(MakeMiniaudioError(result, std::format("cannot start a voice of the audio clip '{}'", name)));
				SoundNode node;
				node.m_Sound = std::move(sound);
				return node;
			}

			// A group under `parent` (the engine's endpoint when null): never spatialized or pitched, since the voices are.
			[[nodiscard]] static Result<SoundNode> CreateGroup(ma_engine& engine, ma_sound_group* parent, std::string_view name)
			{
				Scope<ma_sound_group> group = CreateScope<ma_sound_group>();
				const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
				if (const ma_result result = ma_sound_group_init(&engine, flags, parent, group.get()); result != MA_SUCCESS)
					return std::unexpected(MakeMiniaudioError(result, std::format("cannot create the audio group '{}'", name)));
				SoundNode node;
				node.m_Sound = std::move(group);
				return node;
			}

			void Reset()
			{
				if (m_Sound != nullptr)
					ma_sound_uninit(m_Sound.get());
				m_Sound.reset();
			}

			[[nodiscard]] ma_sound* Get() const { return m_Sound.get(); }
		private:
			Scope<ma_sound> m_Sound;
		};

		// A device notification waiting for Update.
		struct QueuedNotification
		{
			AudioDeviceNotification Type = AudioDeviceNotification::Started;
			bool Injected = false;
		};

		// What the device's threads share with the main thread. The data callback reads the engine only while it holds
		// ReadMutex (try_lock: it never waits) and simulation time is not owned; the main thread holds ReadMutex for its own
		// pulls and takes it once in BeginSimulationTime to wait for a device read in progress.
		struct DeviceShared
		{
			ma_engine* Mixer = nullptr;
			std::mutex ReadMutex;
			std::atomic<bool> SimulationOwned{ false };
			// Set while the engine itself stops the device, so the Stopped notification that causes is not unexpected.
			std::atomic<bool> ExpectingStop{ false };
			std::atomic<uint64_t> ReadFrames{ 0 };
			std::atomic<uint64_t> SilentFrames{ 0 };
			std::mutex QueueMutex; // guards Queue
			std::vector<QueuedNotification> Queue;
		};

	}

	namespace Utils {

		// Replaces every sample that is not finite with silence. The decoder refuses files with such samples
		// (AudioDecoder.h), but bytes that never went through an import (a hand-made pak or cooked cache) and volumes whose
		// product overflows can still make the mixer produce them; neither the device nor a capture may receive them.
		static void ReplaceNonFiniteSamples(std::span<float> samples)
		{
			for (float& sample : samples)
			{
				if (!std::isfinite(sample))
					sample = 0.0f;
			}
		}

		// ma_device_data_proc: reads the engine in real time (time source Device), else outputs silence.
		static void OnDeviceData(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount)
		{
			auto& shared = *static_cast<DeviceShared*>(device->pUserData);
			auto* frames = static_cast<float*>(output);
			std::unique_lock lock(shared.ReadMutex, std::try_to_lock);
			if (lock.owns_lock() && !shared.SimulationOwned.load(std::memory_order_acquire))
			{
				// The node graph silences whatever it does not fill; a failed read is played as silence.
				if (ma_engine_read_pcm_frames(shared.Mixer, frames, frameCount, nullptr) == MA_SUCCESS)
				{
					ReplaceNonFiniteSamples(std::span<float>(frames, static_cast<size_t>(frameCount) * AudioChannelCount));
					shared.ReadFrames.fetch_add(frameCount, std::memory_order_relaxed);
					return;
				}
			}
			std::fill_n(frames, static_cast<size_t>(frameCount) * AudioChannelCount, 0.0f);
			shared.SilentFrames.fetch_add(frameCount, std::memory_order_relaxed);
		}

		static std::optional<AudioDeviceNotification> ToDeviceNotification(ma_device_notification_type type)
		{
			switch (type)
			{
				case ma_device_notification_type_started:            return AudioDeviceNotification::Started;
				case ma_device_notification_type_stopped:            return AudioDeviceNotification::Stopped;
				case ma_device_notification_type_rerouted:           return AudioDeviceNotification::Rerouted;
				case ma_device_notification_type_interruption_began: return AudioDeviceNotification::InterruptionBegan;
				case ma_device_notification_type_interruption_ended: return AudioDeviceNotification::InterruptionEnded;
				case ma_device_notification_type_unlocked:           return AudioDeviceNotification::Unlocked;
			}
			return std::nullopt;
		}

		// ma_device_notification_proc (the device's thread): only queues the notification for Update (§10.1).
		static void OnDeviceNotification(const ma_device_notification* notification)
		{
			auto& shared = *static_cast<DeviceShared*>(notification->pDevice->pUserData);
			const std::optional<AudioDeviceNotification> type = ToDeviceNotification(notification->type);
			if (!type.has_value())
				return;
			if (*type == AudioDeviceNotification::Stopped && shared.ExpectingStop.load())
				return;
			const std::lock_guard lock(shared.QueueMutex);
			shared.Queue.push_back({ .Type = *type, .Injected = false });
		}

		// The miniaudio model of `model`. None is the inverse model (ApplySettings gives it a rolloff of 0): miniaudio's own
		// "none" model switches spatialization off altogether (AudioTypes.h).
		static ma_attenuation_model ToMiniaudioAttenuation(Attenuation model)
		{
			switch (model)
			{
				case Attenuation::None:        return ma_attenuation_model_inverse;
				case Attenuation::Inverse:     return ma_attenuation_model_inverse;
				case Attenuation::Linear:      return ma_attenuation_model_linear;
				case Attenuation::Exponential: return ma_attenuation_model_exponential;
			}

			ENGINE_CORE_ASSERT(false, "Unknown Attenuation {}", std::to_underlying(model));
			return ma_attenuation_model_inverse;
		}

		// The Doppler factor a voice plays with: `factor`, reduced where needed so that it times the speed of the source and
		// times the speed of the listener stays at or below MaxDopplerSpeedFraction of the speed of sound.
		static float GetEffectiveDopplerFactor(float factor, const glm::vec3& sourceVelocity, const glm::vec3& listenerVelocity, float speedOfSound)
		{
			const double speed = std::max(glm::length(glm::dvec3(sourceVelocity)), glm::length(glm::dvec3(listenerVelocity)));
			const double limit = MaxDopplerSpeedFraction * static_cast<double>(speedOfSound);
			if (static_cast<double>(factor) * speed <= limit)
				return factor;
			return static_cast<float>(limit / speed);
		}

		static bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static Status ValidateVolume(float volume, std::string_view what)
		{
			if (!std::isfinite(volume) || volume < 0.0f)
				return MakeError(ErrorCode::InvalidArgument, "the {} must be finite and >= 0 (got {})", what, volume);
			return {};
		}

		static Status ValidateSettings(const AudioVoiceSettings& settings)
		{
			if (std::to_underlying(settings.Group) >= AudioGroupCount)
				return MakeError(ErrorCode::InvalidArgument, "unknown audio group {}", std::to_underlying(settings.Group));
			ENGINE_TRY(Utils::ValidateVolume(settings.Volume, "voice volume"));
			if (!std::isfinite(settings.Pitch) || settings.Pitch < MinAudioPitch)
			{
				return MakeError(ErrorCode::InvalidArgument, "the voice pitch must be finite and >= {} (got {})", MinAudioPitch, settings.Pitch);
			}

			const AudioSpatialization& spatialization = settings.Spatialization;
			if (std::to_underlying(spatialization.Model) > std::to_underlying(Attenuation::Exponential))
				return MakeError(ErrorCode::InvalidArgument, "unknown attenuation model {}", std::to_underlying(spatialization.Model));
			const bool finite = std::isfinite(spatialization.MinDistance) && std::isfinite(spatialization.MaxDistance)
				&& std::isfinite(spatialization.Rolloff) && std::isfinite(spatialization.DopplerFactor);
			if (!finite)
				return MakeError(ErrorCode::InvalidArgument, "the voice's spatialization values must be finite");
			if (spatialization.MinDistance < MinAudioDistance || spatialization.MaxDistance < spatialization.MinDistance)
			{
				return MakeError(ErrorCode::InvalidArgument, "the voice's distances must satisfy {} <= MinDistance <= MaxDistance (got {} and {})",
					MinAudioDistance, spatialization.MinDistance, spatialization.MaxDistance);
			}
			if (spatialization.Rolloff < 0.0f || spatialization.DopplerFactor < 0.0f)
			{
				return MakeError(ErrorCode::InvalidArgument, "the voice's rolloff and Doppler factor must be >= 0 (got {} and {})",
					spatialization.Rolloff, spatialization.DopplerFactor);
			}
			return {};
		}

		static Status ValidateTransform(const AudioVoiceTransform& transform)
		{
			if (!IsFinite(transform.Position) || !IsFinite(transform.Direction) || !IsFinite(transform.Velocity))
				return MakeError(ErrorCode::InvalidArgument, "the voice's position, direction and velocity must be finite");
			return {};
		}

		static Status ValidateListener(const AudioListenerPose& pose)
		{
			if (!IsFinite(pose.Position) || !IsFinite(pose.Forward) || !IsFinite(pose.Up) || !IsFinite(pose.Velocity))
				return MakeError(ErrorCode::InvalidArgument, "the listener's position, forward, up and velocity must be finite");
			const glm::dvec3 forward(pose.Forward);
			const glm::dvec3 up(pose.Up);
			const double forwardLength = glm::length(forward);
			const double upLength = glm::length(up);
			if (forwardLength == 0.0 || upLength == 0.0)
				return MakeError(ErrorCode::InvalidArgument, "the listener's forward and up directions must not be zero");
			if (glm::length(glm::cross(forward, up)) <= ParallelSine * forwardLength * upLength)
				return MakeError(ErrorCode::InvalidArgument, "the listener's forward and up directions must not be parallel");
			return {};
		}

		static Status ValidateClipSource(const AudioClipSource& source)
		{
			if (source.Name.empty())
				return MakeError(ErrorCode::InvalidArgument, "an audio clip needs a name");
			if (source.Name.contains("://") || source.Name.contains('@'))
			{
				return MakeError(ErrorCode::InvalidArgument, "'{}' cannot name an audio clip: a clip name contains neither \"://\" nor '@'", source.Name);
			}
			if (source.Bytes == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "the audio clip '{}' has no bytes", source.Name);
			if (source.Format == AudioClipFormat::Pcm16)
			{
				if (source.Stream)
					return MakeError(ErrorCode::InvalidArgument, "the audio clip '{}' is PCM and cannot stream", source.Name);
				if (source.SampleRate < MinClipSampleRate || source.SampleRate > MaxClipSampleRate)
				{
					return MakeError(ErrorCode::InvalidArgument, "the audio clip '{}' has a sample rate of {} Hz, outside {} to {} Hz", source.Name,
						source.SampleRate, MinClipSampleRate, MaxClipSampleRate);
				}
				if (source.ChannelCount != 1 && source.ChannelCount != 2)
				{
					return MakeError(ErrorCode::InvalidArgument, "the audio clip '{}' has {} channels; PCM clips are mono or stereo", source.Name,
						source.ChannelCount);
				}
				if (source.Bytes->size() % (static_cast<size_t>(source.ChannelCount) * sizeof(int16_t)) != 0)
				{
					return MakeError(ErrorCode::InvalidArgument, "the audio clip '{}' holds {} bytes, not a whole number of 16-bit frames", source.Name,
						source.Bytes->size());
				}
			}
			else if (source.Format != AudioClipFormat::Encoded)
			{
				return MakeError(ErrorCode::InvalidArgument, "the audio clip '{}' has an unknown format {}", source.Name, std::to_underlying(source.Format));
			}
			return {};
		}

	}

	struct AudioClipRecord
	{
		std::string Name{};
		uint64_t Version = 0;
		std::string ResourceName{};
		bool Streamed = false;
		uint32_t References = 0;
		Ref<const AudioClipPayload> Payload{};
	};

	struct AudioVoice
	{
		// Declared before Sound, so the sound is uninitialized (and stops reading) before its data can go.
		Ref<const AudioClipPayload> Payload{};
		SoundNode Sound{};
		AudioClipHandle Clip{};
		std::string ClipName{};
		uint64_t ClipVersion = 0;
		AudioVoiceSettings Settings{};
		AudioVoiceTransform Transform{};
		int32_t Priority = 0;
		uint64_t Owner = 0;
		uint64_t StartSequence = 0;
		bool Paused = false;
		bool Streamed = false;
	};

	struct AudioEngine::State
	{
		AudioEngineSpecification Specification{};

		// The miniaudio objects in creation order; ~State tears them down in reverse.
		MiniaudioLog MiniaudioLogger;
		Scope<AudioVfs> ClipVfs;
		Scope<MiniaudioVfs> VfsBridge;
		ma_resource_manager Resources{};
		bool ResourcesInitialized = false;
		ma_engine Mixer{};
		bool MixerInitialized = false;
		SoundNode MasterGroup;
		std::array<SoundNode, AudioGroupCount> Groups;

		// The device (AudioDeviceKind System or Null).
		DeviceShared Shared;
		ma_context Backend{};
		bool BackendInitialized = false;
		ma_device PlaybackDevice{};
		bool DeviceInitialized = false;
		AudioDeviceState DeviceState = AudioDeviceState::None;
		std::string DeviceName{};
		// A Stopped notification not acted on yet (HandleNotifications): whether it was injected, and when a real one is
		// checked against the device's state.
		bool StopPending = false;
		bool StopInjected = false;
		double StopCheckTime = 0.0;
		double NextDeviceAttempt = 0.0;
		uint32_t FailedAttempts = 0;
		uint32_t InjectedFailures = 0;
		uint32_t DeviceRecreations = 0;
		uint32_t FailedDeviceCreations = 0;

		// Clips and voices.
		HandlePool<AudioClipRecord> Clips;
		std::map<std::string, AudioClipHandle, std::less<>> ClipsByResource;
		// The data of every decoded-data name miniaudio may still hold (its registration or a voice), so registering a name
		// again while a voice of the previous registration plays shares the data miniaudio already has for it.
		std::map<std::string, std::weak_ptr<const AudioClipPayload>, std::less<>> ResidentPayloads;
		HandlePool<AudioVoice> Voices{ MaxAudioVoices };
		uint64_t NextStartSequence = 1;
		uint64_t StartedVoices = 0;
		uint64_t StolenVoices = 0;
		AudioListenerPose Listener{};
		std::array<float, AudioGroupCount> GroupVolumes{ 1.0f, 1.0f, 1.0f };
		float MasterVolume = 1.0f;

		// Time.
		uint32_t FixedHz = 0;
		uint64_t SimulationTicks = 0;
		uint64_t SimulationFrames = 0;
		double HostSeconds = 0.0;
		uint64_t HostFrames = 0;
		uint64_t PulledFrames = 0;
		std::vector<float> Scratch;
		bool ReadFailureLogged = false;

		// Capture.
		bool Capturing = false;
		AudioCapture Capture{};

		State() = default;
		~State();

		State(const State&) = delete;
		State& operator=(const State&) = delete;

		[[nodiscard]] Status Initialize(const VirtualFileSystem& vfs);
		[[nodiscard]] Status CreateDevice();
		void DestroyDevice();
		void HandleNotifications(double nowSeconds);
		void HandlePendingStop(double nowSeconds);
		void HandleUnexpectedStop(double nowSeconds, double recreationDelaySeconds);
		void HandleRecreation(double nowSeconds);
		void ProcessJobs();
		void Pull(uint64_t frames, std::span<float> destination);
		void AppendCapture(std::span<const float> samples);
		void ReleaseFinishedVoices();
		[[nodiscard]] Ref<const AudioClipPayload> FindResidentPayload(std::string_view resourceName);
		[[nodiscard]] Status ApplyGroup(ma_sound& sound, AudioGroup group);
		static void ApplySettings(ma_sound& sound, const AudioVoiceSettings& settings);
		static void ApplyTransform(ma_sound& sound, const AudioVoiceTransform& transform);
		// The voice's effective Doppler factor (GetEffectiveDopplerFactor) for its settings, its velocity and the listener's.
		void ApplyDoppler(ma_sound& sound, const AudioVoiceSettings& settings, const AudioVoiceTransform& transform);
		[[nodiscard]] std::optional<AudioVoiceHandle> FindStealVictim();
		[[nodiscard]] AudioTimeSource GetTimeSource() const;
		[[nodiscard]] uint64_t GetCursor(const AudioVoice& voice) const;
		[[nodiscard]] static uint64_t GetLength(const AudioVoice& voice);
	};

	AudioEngine::State::~State()
	{
		// The device first, so nothing reads the engine any more; then the voices, the registrations, the groups, the
		// engine and the resource manager (which closes the last files of the VFS bridge), the VFS and the backend.
		DestroyDevice();
		Voices.Clear();
		Clips.ForEach([this](AudioClipHandle /*handle*/, AudioClipRecord& record)
		{
			if (record.Streamed)
			{
				if (const Status removed = ClipVfs->RemoveMemoryFile(record.ResourceName); !removed.has_value())
					ENGINE_CORE_ERROR("Cannot remove the audio clip '{}': {}", record.ResourceName, removed.error().ToString());
			}
			else if (const ma_result result = ma_resource_manager_unregister_data(&Resources, record.ResourceName.c_str()); result != MA_SUCCESS)
			{
				ENGINE_CORE_ERROR("Cannot unregister the audio clip '{}': {}", record.ResourceName, DescribeMiniaudioResult(result));
			}
		});
		Clips.Clear();
		ResidentPayloads.clear();
		for (SoundNode& group : Groups)
			group.Reset();
		MasterGroup.Reset();
		if (MixerInitialized)
			ma_engine_uninit(&Mixer);
		if (ResourcesInitialized)
			ma_resource_manager_uninit(&Resources);
		VfsBridge.reset();
		ClipVfs.reset();
		if (BackendInitialized)
			ma_context_uninit(&Backend);
	}

	Status AudioEngine::State::Initialize(const VirtualFileSystem& vfs)
	{
		ENGINE_TRY(MiniaudioLogger.Initialize());
		ClipVfs = CreateScope<AudioVfs>(vfs);
		VfsBridge = CreateScope<MiniaudioVfs>(*ClipVfs);

		// Decoded data and streams stay at the clip's own rate and channel count, in f32 (AudioEngine.h "Clips").
		ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
		resourceConfig.pLog = MiniaudioLogger.Get();
		resourceConfig.decodedFormat = ma_format_f32;
		resourceConfig.decodedChannels = 0;
		resourceConfig.decodedSampleRate = 0;
		resourceConfig.pVFS = VfsBridge->Get();
		if (Specification.Decoding == AudioDecoding::Deterministic)
		{
			resourceConfig.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING | MA_RESOURCE_MANAGER_FLAG_NON_BLOCKING;
			resourceConfig.jobThreadCount = 0;
		}
		if (const ma_result result = ma_resource_manager_init(&resourceConfig, &Resources); result != MA_SUCCESS)
			return std::unexpected(MakeMiniaudioError(result, "cannot create the audio resource manager"));
		ResourcesInitialized = true;

		ma_engine_config engineConfig = ma_engine_config_init();
		engineConfig.pResourceManager = &Resources;
		engineConfig.pLog = MiniaudioLogger.Get();
		engineConfig.noDevice = MA_TRUE;
		engineConfig.channels = AudioChannelCount;
		engineConfig.sampleRate = AudioSampleRate;
		engineConfig.listenerCount = 1;
		if (const ma_result result = ma_engine_init(&engineConfig, &Mixer); result != MA_SUCCESS)
			return std::unexpected(MakeMiniaudioError(result, "cannot create the audio engine"));
		MixerInitialized = true;
		Shared.Mixer = &Mixer;

		ENGINE_TRY_ASSIGN(MasterGroup, SoundNode::CreateGroup(Mixer, nullptr, "Master"));
		for (uint32_t index = 0; index < AudioGroupCount; ++index)
		{
			const auto group = static_cast<AudioGroup>(index);
			ENGINE_TRY_ASSIGN(Groups[index], SoundNode::CreateGroup(Mixer, MasterGroup.Get(), AudioGroupToString(group)));
		}

		ma_engine_listener_set_position(&Mixer, 0, Listener.Position.x, Listener.Position.y, Listener.Position.z);
		ma_engine_listener_set_direction(&Mixer, 0, Listener.Forward.x, Listener.Forward.y, Listener.Forward.z);
		ma_engine_listener_set_world_up(&Mixer, 0, Listener.Up.x, Listener.Up.y, Listener.Up.z);
		ma_engine_listener_set_velocity(&Mixer, 0, Listener.Velocity.x, Listener.Velocity.y, Listener.Velocity.z);
		return {};
	}

	Status AudioEngine::State::CreateDevice()
	{
		if (InjectedFailures > 0)
		{
			--InjectedFailures;
			return MakeError(ErrorCode::Io, "the backend refused to create the device (injected by the test instrumentation)");
		}

		if (!BackendInitialized)
		{
			ma_context_config contextConfig = ma_context_config_init();
			contextConfig.pLog = MiniaudioLogger.Get();
			const ma_backend nullBackend = ma_backend_null;
			const bool useNull = Specification.Device == AudioDeviceKind::Null;
			const ma_result result = ma_context_init(useNull ? &nullBackend : nullptr, useNull ? 1 : 0, &contextConfig, &Backend);
			if (result != MA_SUCCESS)
				return std::unexpected(MakeMiniaudioError(result, "cannot initialize an audio backend"));
			BackendInitialized = true;
		}

		ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
		deviceConfig.playback.format = ma_format_f32;
		deviceConfig.playback.channels = AudioChannelCount;
		deviceConfig.sampleRate = AudioSampleRate;
		deviceConfig.dataCallback = &Utils::OnDeviceData;
		deviceConfig.notificationCallback = &Utils::OnDeviceNotification;
		deviceConfig.pUserData = &Shared;
		if (const ma_result result = ma_device_init(&Backend, &deviceConfig, &PlaybackDevice); result != MA_SUCCESS)
			return std::unexpected(MakeMiniaudioError(result, "cannot create the playback device"));
		DeviceInitialized = true;
		if (const ma_result result = ma_device_start(&PlaybackDevice); result != MA_SUCCESS)
		{
			DestroyDevice();
			return std::unexpected(MakeMiniaudioError(result, "cannot start the playback device"));
		}
		DeviceName = PlaybackDevice.playback.name;
		return {};
	}

	void AudioEngine::State::DestroyDevice()
	{
		if (!DeviceInitialized)
			return;
		Shared.ExpectingStop.store(true);
		ma_device_uninit(&PlaybackDevice);
		Shared.ExpectingStop.store(false);
		DeviceInitialized = false;
		DeviceName.clear();
		StopPending = false;
	}

	void AudioEngine::State::HandleNotifications(double nowSeconds)
	{
		std::vector<QueuedNotification> queue;
		{
			const std::lock_guard lock(Shared.QueueMutex);
			queue.swap(Shared.Queue);
		}

		// A Stopped is not acted on when it arrives: some backends stop the device to reroute it and start it again (WASAPI
		// does when the default device changes: Stopped, Rerouted, Started). A Rerouted or Started after it cancels it.
		for (const QueuedNotification& notification : queue)
		{
			switch (notification.Type)
			{
				case AudioDeviceNotification::Stopped:
				{
					if (DeviceState != AudioDeviceState::Running)
					{
						ENGINE_CORE_TRACE("Ignoring a stop notification of the audio device in state {}", AudioDeviceStateToString(DeviceState));
						break;
					}
					StopPending = true;
					StopInjected = notification.Injected;
					StopCheckTime = nowSeconds + AudioEngine::DeviceRecreationDelaySeconds;
					ENGINE_CORE_TRACE("The audio device '{}' stopped{}", DeviceName, notification.Injected ? " (injected)" : "");
					break;
				}
				case AudioDeviceNotification::Rerouted:
				{
					ENGINE_CORE_INFO("The audio device '{}' was rerouted: the default output device changed", DeviceName);
					StopPending = false;
					break;
				}
				case AudioDeviceNotification::Started:
				{
					ENGINE_CORE_TRACE("Audio device notification: {}", AudioDeviceNotificationToString(notification.Type));
					StopPending = false;
					break;
				}
				case AudioDeviceNotification::InterruptionBegan:
				case AudioDeviceNotification::InterruptionEnded:
				case AudioDeviceNotification::Unlocked:
				{
					ENGINE_CORE_TRACE("Audio device notification: {}", AudioDeviceNotificationToString(notification.Type));
					break;
				}
			}
		}

		// An injected Stopped is unexpected by definition (AudioEngine.h): acted on now, unless its batch rerouted or
		// restarted the device after it.
		if (StopPending && StopInjected)
			HandleUnexpectedStop(nowSeconds, AudioEngine::DeviceRecreationDelaySeconds);
	}

	void AudioEngine::State::HandlePendingStop(double nowSeconds)
	{
		if (!StopPending || nowSeconds < StopCheckTime)
			return;
		// A real Stopped with no Rerouted or Started after it for DeviceRecreationDelaySeconds: a device miniaudio restarted on
		// its own (a reroute whose notifications were lost) keeps running; a device still stopped was lost, and the delay
		// before its re-creation has passed already. Waiting also keeps ma_device_uninit away from a reroute in progress.
		if (DeviceInitialized && ma_device_get_state(&PlaybackDevice) == ma_device_state_started)
		{
			ENGINE_CORE_TRACE("The audio device '{}' runs again after a stop notification", DeviceName);
			StopPending = false;
			return;
		}
		HandleUnexpectedStop(nowSeconds, 0.0);
	}

	void AudioEngine::State::HandleUnexpectedStop(double nowSeconds, double recreationDelaySeconds)
	{
		ENGINE_CORE_INFO("The audio device '{}' stopped unexpectedly{}; re-creating it in {} s", DeviceName, StopInjected ? " (injected)" : "",
			recreationDelaySeconds);
		DestroyDevice();
		DeviceState = AudioDeviceState::Recreating;
		FailedAttempts = 0;
		NextDeviceAttempt = nowSeconds + recreationDelaySeconds;
	}

	void AudioEngine::State::HandleRecreation(double nowSeconds)
	{
		if (DeviceState != AudioDeviceState::Recreating || nowSeconds < NextDeviceAttempt)
			return;

		const Status created = CreateDevice();
		if (created.has_value())
		{
			DeviceState = AudioDeviceState::Running;
			++DeviceRecreations;
			FailedAttempts = 0;
			ENGINE_CORE_INFO("Re-created the audio device '{}'", DeviceName);
			return;
		}

		++FailedDeviceCreations;
		++FailedAttempts;
		if (FailedAttempts >= AudioEngine::MaxDeviceRecreationAttempts)
		{
			DeviceState = AudioDeviceState::Failed;
			ENGINE_CORE_WARN("The audio device could not be re-created after {} attempts ({}); audio continues without a device",
				FailedAttempts, created.error().ToString());
			return;
		}
		ENGINE_CORE_INFO("Re-creating the audio device failed (attempt {} of {}): {}; retrying in {} s", FailedAttempts,
			AudioEngine::MaxDeviceRecreationAttempts, created.error().ToString(), AudioEngine::DeviceRecreationDelaySeconds);
		NextDeviceAttempt = nowSeconds + AudioEngine::DeviceRecreationDelaySeconds;
	}

	void AudioEngine::State::ProcessJobs()
	{
		for (;;)
		{
			const ma_result result = ma_resource_manager_process_next_job(&Resources);
			if (result == MA_NO_DATA_AVAILABLE || result == MA_CANCELLED)
				return;
			// A failed job leaves its data source failed (the voice plays silence) and its error in the voice's state.
			if (result != MA_SUCCESS)
				ENGINE_CORE_TRACE("An audio resource job failed: {}", DescribeMiniaudioResult(result));
		}
	}

	void AudioEngine::State::Pull(uint64_t frames, std::span<float> destination)
	{
		ENGINE_CORE_ASSERT(destination.empty() || destination.size() == frames * AudioChannelCount, "Pull destination of the wrong size");
		{
			const std::lock_guard lock(Shared.ReadMutex);
			uint64_t done = 0;
			while (done < frames)
			{
				const auto chunk = static_cast<uint32_t>(std::min<uint64_t>(frames - done, PullChunkFrames));
				if (Specification.Decoding == AudioDecoding::Deterministic)
					ProcessJobs();

				std::span<float> output;
				if (destination.empty())
				{
					Scratch.resize(static_cast<size_t>(PullChunkFrames) * AudioChannelCount);
					output = std::span<float>(Scratch).first(static_cast<size_t>(chunk) * AudioChannelCount);
				}
				else
				{
					output = destination.subspan(static_cast<size_t>(done) * AudioChannelCount, static_cast<size_t>(chunk) * AudioChannelCount);
				}

				// The node graph silences the frames it does not fill (no voice playing); a failed read is silence too.
				if (const ma_result result = ma_engine_read_pcm_frames(&Mixer, output.data(), chunk, nullptr); result != MA_SUCCESS)
				{
					std::ranges::fill(output, 0.0f);
					if (!ReadFailureLogged)
					{
						ENGINE_CORE_ERROR("Reading the audio engine failed: {}; the frames are silent", DescribeMiniaudioResult(result));
						ReadFailureLogged = true;
					}
				}
				Utils::ReplaceNonFiniteSamples(output);
				if (Capturing)
					AppendCapture(output);
				PulledFrames += chunk;
				done += chunk;
			}
		}
		ReleaseFinishedVoices();
	}

	void AudioEngine::State::AppendCapture(std::span<const float> samples)
	{
		const uint64_t captured = Capture.GetFrameCount();
		const uint64_t room = AudioEngine::MaxCaptureFrames - std::min(captured, AudioEngine::MaxCaptureFrames);
		const uint64_t frames = samples.size() / AudioChannelCount;
		const uint64_t kept = std::min(frames, room);
		Capture.Samples.insert(Capture.Samples.end(), samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(kept * AudioChannelCount));
		if (kept < frames)
			Capture.Truncated = true;
	}

	void AudioEngine::State::ReleaseFinishedVoices()
	{
		std::vector<AudioVoiceHandle> finished;
		Voices.ForEach([&finished](AudioVoiceHandle handle, AudioVoice& voice)
		{
			if (ma_sound_at_end(voice.Sound.Get()))
				finished.push_back(handle);
		});
		for (const AudioVoiceHandle handle : finished)
		{
			[[maybe_unused]] const Status released = Voices.Destroy(handle);
			ENGINE_CORE_ASSERT(released.has_value(), "A finished audio voice was not alive");
		}
	}

	Ref<const AudioClipPayload> AudioEngine::State::FindResidentPayload(std::string_view resourceName)
	{
		// Forget the names whose registration and voices are all gone (miniaudio released their data with them), so the
		// table does not grow with every hot-reloaded version.
		std::erase_if(ResidentPayloads, [](const auto& entry)
		{
			return entry.second.expired();
		});
		const auto found = ResidentPayloads.find(resourceName);
		return found != ResidentPayloads.end() ? found->second.lock() : nullptr;
	}

	Status AudioEngine::State::ApplyGroup(ma_sound& sound, AudioGroup group)
	{
		ma_sound* target = Groups[std::to_underlying(group)].Get();
		if (const ma_result result = ma_node_attach_output_bus(&sound, 0, target, 0); result != MA_SUCCESS)
			return std::unexpected(MakeMiniaudioError(result, std::format("cannot move a voice to the audio group '{}'", AudioGroupToString(group))));
		return {};
	}

	void AudioEngine::State::ApplySettings(ma_sound& sound, const AudioVoiceSettings& settings)
	{
		const AudioSpatialization& spatialization = settings.Spatialization;
		ma_sound_set_volume(&sound, settings.Volume);
		ma_sound_set_pitch(&sound, std::min(settings.Pitch, MaxEffectivePitch));
		ma_sound_set_looping(&sound, settings.Loop ? MA_TRUE : MA_FALSE);
		ma_sound_set_spatialization_enabled(&sound, settings.Spatial ? MA_TRUE : MA_FALSE);
		// Attenuation::None keeps a spatial voice positioned (panned and Doppler-shifted) without a distance falloff: the
		// inverse model (ToMiniaudioAttenuation) with no rolloff, whose gain is 1 at every distance.
		ma_sound_set_attenuation_model(&sound, Utils::ToMiniaudioAttenuation(spatialization.Model));
		ma_sound_set_rolloff(&sound, spatialization.Model != Attenuation::None ? spatialization.Rolloff : 0.0f);
		ma_sound_set_min_distance(&sound, spatialization.MinDistance);
		ma_sound_set_max_distance(&sound, spatialization.MaxDistance);
	}

	void AudioEngine::State::ApplyTransform(ma_sound& sound, const AudioVoiceTransform& transform)
	{
		ma_sound_set_position(&sound, transform.Position.x, transform.Position.y, transform.Position.z);
		ma_sound_set_direction(&sound, transform.Direction.x, transform.Direction.y, transform.Direction.z);
		ma_sound_set_velocity(&sound, transform.Velocity.x, transform.Velocity.y, transform.Velocity.z);
	}

	void AudioEngine::State::ApplyDoppler(ma_sound& sound, const AudioVoiceSettings& settings, const AudioVoiceTransform& transform)
	{
		const float speedOfSound = ma_spatializer_listener_get_speed_of_sound(&Mixer.listeners[0]);
		ma_sound_set_doppler_factor(&sound,
			Utils::GetEffectiveDopplerFactor(settings.Spatialization.DopplerFactor, transform.Velocity, Listener.Velocity, speedOfSound));
	}

	std::optional<AudioVoiceHandle> AudioEngine::State::FindStealVictim()
	{
		// The lowest priority, then the farthest from the listener (a non-spatial voice is at distance 0), then the oldest.
		std::optional<AudioVoiceHandle> victim;
		int32_t victimPriority = 0;
		float victimDistance = 0.0f;
		uint64_t victimSequence = 0;
		const glm::vec3 listener = Listener.Position;
		Voices.ForEach([&](AudioVoiceHandle handle, const AudioVoice& voice)
		{
			const glm::vec3 offset = voice.Transform.Position - listener;
			const float distance = voice.Settings.Spatial ? glm::dot(offset, offset) : 0.0f;
			const bool better = !victim.has_value() || voice.Priority < victimPriority
				|| (voice.Priority == victimPriority
					&& (distance > victimDistance || (distance == victimDistance && voice.StartSequence < victimSequence)));
			if (!better)
				return;
			victim = handle;
			victimPriority = voice.Priority;
			victimDistance = distance;
			victimSequence = voice.StartSequence;
		});
		return victim;
	}

	AudioTimeSource AudioEngine::State::GetTimeSource() const
	{
		if (Shared.SimulationOwned.load())
			return AudioTimeSource::Simulation;
		if (DeviceState == AudioDeviceState::Running || DeviceState == AudioDeviceState::Recreating)
			return AudioTimeSource::Device;
		return AudioTimeSource::Host;
	}

	uint64_t AudioEngine::State::GetCursor(const AudioVoice& voice) const
	{
		// miniaudio's cursor counts the frames read from the clip, which runs ahead of the frames played by the voice's
		// processing cache (the frames read but not yet mixed); the voice's position is the cursor less those, modulo the
		// clip's length when the cache holds frames from before a loop's wrap. The cache count is a plain field the mixing
		// thread writes, so it is read only while nothing else mixes: the engine pulls its own frames, or there is no
		// device. While the device may read the engine, the position is miniaudio's own cursor snapshot.
		const ma_sound* sound = voice.Sound.Get();
		ma_uint64 cursor = 0;
		if (ma_sound_get_cursor_in_pcm_frames(sound, &cursor) != MA_SUCCESS)
			return 0;
		const bool deviceMayRead = DeviceInitialized && !Shared.SimulationOwned.load();
		if (deviceMayRead)
			return cursor;
		const uint64_t cached = sound->processingCacheFramesRemaining;
		if (cached <= cursor)
			return cursor - cached;
		const uint64_t length = GetLength(voice);
		if (length == 0)
			return 0;
		return (cursor + length - cached % length) % length;
	}

	uint64_t AudioEngine::State::GetLength(const AudioVoice& voice)
	{
		ma_uint64 length = 0;
		if (ma_sound_get_length_in_pcm_frames(voice.Sound.Get(), &length) != MA_SUCCESS)
			return 0;
		return length;
	}

	AudioEngine::AudioEngine(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	AudioEngine::~AudioEngine()
	{
		ENGINE_CORE_ASSERT(m_State->Voices.GetSize() == 0, "The audio engine is destroyed with {} live voices; their owners release them first",
			m_State->Voices.GetSize());
	}

	Result<Scope<AudioEngine>> AudioEngine::Create(const AudioEngineSpecification& specification, const VirtualFileSystem& vfs)
	{
		if (std::to_underlying(specification.Device) > std::to_underlying(AudioDeviceKind::Null))
			return MakeError(ErrorCode::InvalidArgument, "unknown audio device kind {}", std::to_underlying(specification.Device));
		if (std::to_underlying(specification.Decoding) > std::to_underlying(AudioDecoding::Threaded))
			return MakeError(ErrorCode::InvalidArgument, "unknown audio decoding {}", std::to_underlying(specification.Decoding));

		Scope<AudioEngine> engine = CreateScope<AudioEngine>(ConstructionKey());
		State& state = *engine->m_State;
		state.Specification = specification;
		state.InjectedFailures = specification.InjectedDeviceCreationFailures;
		ENGINE_TRY(state.Initialize(vfs));

		if (specification.Device != AudioDeviceKind::None)
		{
			const Status created = state.CreateDevice();
			if (created.has_value())
			{
				state.DeviceState = AudioDeviceState::Running;
				ENGINE_CORE_INFO("Audio engine: device '{}' ({}), {} decoding", state.DeviceName, AudioDeviceKindToString(specification.Device),
					AudioDecodingToString(specification.Decoding));
			}
			else
			{
				state.DeviceState = AudioDeviceState::Failed;
				++state.FailedDeviceCreations;
				ENGINE_CORE_WARN("No audio device could be created ({}); audio runs without a device", created.error().ToString());
			}
		}
		else
		{
			ENGINE_CORE_INFO("Audio engine: no device, {} decoding", AudioDecodingToString(specification.Decoding));
		}
		return engine;
	}

	const AudioEngineSpecification& AudioEngine::GetSpecification() const
	{
		return m_State->Specification;
	}

	Result<AudioClipHandle> AudioEngine::RegisterClip(const AudioClipSource& source)
	{
		State& state = *m_State;
		ENGINE_TRY(Utils::ValidateClipSource(source));
		std::string resourceName = MakeAudioResourceName(source.Name, source.Version);
		if (const auto found = state.ClipsByResource.find(resourceName); found != state.ClipsByResource.end())
		{
			++state.Clips.Get(found->second).References;
			return found->second;
		}

		const bool streamed = source.Format == AudioClipFormat::Encoded && source.Stream;
		Ref<const AudioClipPayload> payload;
		if (streamed)
		{
			ENGINE_TRY(WithContext(state.ClipVfs->AddMemoryFile(resourceName, source.Bytes), std::format("while registering the audio clip '{}'", resourceName)));
			payload = CreateRef<const AudioClipPayload>(AudioClipPayload{ .Bytes = source.Bytes });
		}
		else
		{
			payload = state.FindResidentPayload(resourceName);
			if (payload == nullptr && source.Format == AudioClipFormat::Pcm16)
			{
				const uint64_t frames = source.Bytes->size() / (static_cast<size_t>(source.ChannelCount) * sizeof(int16_t));
				payload = CreateRef<const AudioClipPayload>(AudioClipPayload{ .Bytes = source.Bytes,
					.Format = ma_format_s16,
					.ChannelCount = source.ChannelCount,
					.SampleRate = source.SampleRate,
					.FrameCount = frames });
			}
			else if (payload == nullptr)
			{
				ENGINE_TRY_ASSIGN(DecodedAudio decoded,
					WithContext(DecodeEncodedAudio(*source.Bytes), std::format("while decoding the audio clip '{}'", resourceName)));
				payload = CreateRef<const AudioClipPayload>(AudioClipPayload{ .Pcm = std::move(decoded.Samples),
					.Format = ma_format_f32,
					.ChannelCount = decoded.Info.ChannelCount,
					.SampleRate = decoded.Info.SampleRate,
					.FrameCount = decoded.Info.FrameCount });
			}

			const ma_result result = ma_resource_manager_register_decoded_data(&state.Resources, resourceName.c_str(), payload->GetData(),
				payload->FrameCount, payload->Format, payload->ChannelCount, payload->SampleRate);
			if (result != MA_SUCCESS)
				return std::unexpected(MakeMiniaudioError(result, std::format("cannot register the audio clip '{}'", resourceName)));
			state.ResidentPayloads.insert_or_assign(resourceName, payload);
		}

		Result<AudioClipHandle> handle = state.Clips.Create(AudioClipRecord{
			.Name = source.Name, .Version = source.Version, .ResourceName = resourceName, .Streamed = streamed, .References = 1, .Payload = payload });
		ENGINE_CORE_VERIFY(handle.has_value(), "The audio clip pool has no capacity limit");
		state.ClipsByResource.emplace(std::move(resourceName), *handle);
		return *handle;
	}

	Status AudioEngine::UnregisterClip(AudioClipHandle clip)
	{
		State& state = *m_State;
		AudioClipRecord* record = state.Clips.TryGet(clip);
		if (record == nullptr)
			return MakeError(ErrorCode::NotFound, "the audio clip {:#x} is not registered", clip.GetValue());
		if (--record->References > 0)
			return {};

		// The last reference: voices of the clip keep their data (they hold its payload and miniaudio's node) until they end.
		if (record->Streamed)
		{
			ENGINE_TRY(state.ClipVfs->RemoveMemoryFile(record->ResourceName));
		}
		else if (const ma_result result = ma_resource_manager_unregister_data(&state.Resources, record->ResourceName.c_str()); result != MA_SUCCESS)
		{
			return std::unexpected(MakeMiniaudioError(result, std::format("cannot unregister the audio clip '{}'", record->ResourceName)));
		}
		state.ClipsByResource.erase(record->ResourceName);
		return state.Clips.Destroy(clip);
	}

	bool AudioEngine::IsClipRegistered(AudioClipHandle clip) const
	{
		return m_State->Clips.IsAlive(clip);
	}

	Result<AudioVoiceHandle> AudioEngine::PlayVoice(const AudioVoiceDescription& description)
	{
		State& state = *m_State;
		const AudioClipRecord* clip = state.Clips.TryGet(description.Clip);
		if (clip == nullptr)
			return MakeError(ErrorCode::NotFound, "the audio clip {:#x} is not registered", description.Clip.GetValue());
		ENGINE_TRY(Utils::ValidateSettings(description.Settings));
		ENGINE_TRY(Utils::ValidateTransform(description.Transform));

		std::optional<AudioVoiceHandle> victim;
		if (state.Voices.GetSize() >= MaxAudioVoices)
		{
			victim = state.FindStealVictim();
			ENGINE_CORE_VERIFY(victim.has_value(), "A full voice pool has a voice to steal");
			const int32_t lowest = state.Voices.Get(*victim).Priority;
			if (lowest > description.Priority)
				return MakeError(ErrorCode::InvalidState, "all {} voices have a higher priority than {} (the lowest is {})", state.Voices.GetSize(),
					description.Priority, lowest);
		}

		ma_uint32 flags = clip->Streamed ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
		if (!description.Settings.Spatial)
			flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
		ma_sound_group& group = *state.Groups[std::to_underlying(description.Settings.Group)].Get();
		ENGINE_TRY_ASSIGN(SoundNode sound, SoundNode::CreateSound(state.Mixer, clip->ResourceName, flags, group));
		State::ApplySettings(*sound.Get(), description.Settings);
		State::ApplyTransform(*sound.Get(), description.Transform);
		state.ApplyDoppler(*sound.Get(), description.Settings, description.Transform);
		if (!description.StartPaused)
		{
			if (const ma_result result = ma_sound_start(sound.Get()); result != MA_SUCCESS)
				return std::unexpected(MakeMiniaudioError(result, std::format("cannot start a voice of the audio clip '{}'", clip->ResourceName)));
		}

		if (victim.has_value())
		{
			ENGINE_TRY(state.Voices.Destroy(*victim));
			++state.StolenVoices;
		}

		AudioVoice voice{ .Payload = clip->Payload,
			.Sound = std::move(sound),
			.Clip = description.Clip,
			.ClipName = clip->Name,
			.ClipVersion = clip->Version,
			.Settings = description.Settings,
			.Transform = description.Transform,
			.Priority = description.Priority,
			.Owner = description.Owner,
			.StartSequence = state.NextStartSequence++,
			.Paused = description.StartPaused,
			.Streamed = clip->Streamed };
		ENGINE_TRY_ASSIGN(const AudioVoiceHandle handle, state.Voices.Create(std::move(voice)));
		++state.StartedVoices;
		return handle;
	}

	Status AudioEngine::StopVoice(AudioVoiceHandle voice)
	{
		if (!m_State->Voices.IsAlive(voice))
			return MakeError(ErrorCode::NotFound, "the audio voice {:#x} is not alive", voice.GetValue());
		return m_State->Voices.Destroy(voice);
	}

	Status AudioEngine::SetVoicePaused(AudioVoiceHandle voice, bool paused)
	{
		AudioVoice* target = m_State->Voices.TryGet(voice);
		if (target == nullptr)
			return MakeError(ErrorCode::NotFound, "the audio voice {:#x} is not alive", voice.GetValue());
		if (target->Paused == paused)
			return {};
		const ma_result result = paused ? ma_sound_stop(target->Sound.Get()) : ma_sound_start(target->Sound.Get());
		if (result != MA_SUCCESS)
			return std::unexpected(MakeMiniaudioError(result, paused ? "cannot pause an audio voice" : "cannot resume an audio voice"));
		target->Paused = paused;
		return {};
	}

	Status AudioEngine::SetVoiceSettings(AudioVoiceHandle voice, const AudioVoiceSettings& settings)
	{
		AudioVoice* target = m_State->Voices.TryGet(voice);
		if (target == nullptr)
			return MakeError(ErrorCode::NotFound, "the audio voice {:#x} is not alive", voice.GetValue());
		ENGINE_TRY(Utils::ValidateSettings(settings));
		if (settings.Group != target->Settings.Group)
			ENGINE_TRY(m_State->ApplyGroup(*target->Sound.Get(), settings.Group));
		State::ApplySettings(*target->Sound.Get(), settings);
		m_State->ApplyDoppler(*target->Sound.Get(), settings, target->Transform);
		target->Settings = settings;
		return {};
	}

	Status AudioEngine::SetVoiceTransform(AudioVoiceHandle voice, const AudioVoiceTransform& transform)
	{
		AudioVoice* target = m_State->Voices.TryGet(voice);
		if (target == nullptr)
			return MakeError(ErrorCode::NotFound, "the audio voice {:#x} is not alive", voice.GetValue());
		ENGINE_TRY(Utils::ValidateTransform(transform));
		// Applied to non-spatial voices too (miniaudio ignores it there), so turning Spatial on later starts at the pose.
		State::ApplyTransform(*target->Sound.Get(), transform);
		m_State->ApplyDoppler(*target->Sound.Get(), target->Settings, transform);
		target->Transform = transform;
		return {};
	}

	bool AudioEngine::IsVoiceAlive(AudioVoiceHandle voice) const
	{
		return m_State->Voices.IsAlive(voice);
	}

	Result<AudioVoiceInfo> AudioEngine::GetVoiceInfo(AudioVoiceHandle voice) const
	{
		const AudioVoice* target = m_State->Voices.TryGet(voice);
		if (target == nullptr)
			return MakeError(ErrorCode::NotFound, "the audio voice {:#x} is not alive", voice.GetValue());
		return AudioVoiceInfo{ .Voice = voice,
			.Clip = target->Clip,
			.ClipName = target->ClipName,
			.ClipVersion = target->ClipVersion,
			.Owner = target->Owner,
			.Priority = target->Priority,
			.Settings = target->Settings,
			.Transform = target->Transform,
			.Paused = target->Paused,
			.Streamed = target->Streamed,
			.CursorFrames = m_State->GetCursor(*target),
			.LengthFrames = State::GetLength(*target),
			.StartSequence = target->StartSequence };
	}

	std::vector<AudioVoiceInfo> AudioEngine::GetVoices() const
	{
		std::vector<AudioVoiceHandle> handles;
		m_State->Voices.ForEach([&handles](AudioVoiceHandle handle, AudioVoice& /*voice*/)
		{
			handles.push_back(handle);
		});
		std::vector<AudioVoiceInfo> voices;
		voices.reserve(handles.size());
		for (const AudioVoiceHandle handle : handles)
		{
			Result<AudioVoiceInfo> info = GetVoiceInfo(handle);
			ENGINE_CORE_VERIFY(info.has_value(), "A listed audio voice is alive");
			voices.push_back(std::move(*info));
		}
		std::ranges::sort(voices, {}, &AudioVoiceInfo::StartSequence);
		return voices;
	}

	Status AudioEngine::SetListener(const AudioListenerPose& pose)
	{
		ENGINE_TRY(Utils::ValidateListener(pose));
		ma_engine& mixer = m_State->Mixer;
		ma_engine_listener_set_position(&mixer, 0, pose.Position.x, pose.Position.y, pose.Position.z);
		ma_engine_listener_set_direction(&mixer, 0, pose.Forward.x, pose.Forward.y, pose.Forward.z);
		ma_engine_listener_set_world_up(&mixer, 0, pose.Up.x, pose.Up.y, pose.Up.z);
		ma_engine_listener_set_velocity(&mixer, 0, pose.Velocity.x, pose.Velocity.y, pose.Velocity.z);
		m_State->Listener = pose;
		// Every voice's Doppler bound depends on the listener's speed.
		State& state = *m_State;
		state.Voices.ForEach([&state](AudioVoiceHandle /*handle*/, AudioVoice& voice)
		{
			state.ApplyDoppler(*voice.Sound.Get(), voice.Settings, voice.Transform);
		});
		return {};
	}

	AudioListenerPose AudioEngine::GetListener() const
	{
		return m_State->Listener;
	}

	Status AudioEngine::SetGroupVolume(AudioGroup group, float volume)
	{
		if (std::to_underlying(group) >= AudioGroupCount)
			return MakeError(ErrorCode::InvalidArgument, "unknown audio group {}", std::to_underlying(group));
		ENGINE_TRY(Utils::ValidateVolume(volume, std::format("volume of the audio group '{}'", AudioGroupToString(group))));
		ma_sound_group_set_volume(m_State->Groups[std::to_underlying(group)].Get(), volume);
		m_State->GroupVolumes[std::to_underlying(group)] = volume;
		return {};
	}

	float AudioEngine::GetGroupVolume(AudioGroup group) const
	{
		ENGINE_CORE_ASSERT(std::to_underlying(group) < AudioGroupCount, "Unknown AudioGroup {}", std::to_underlying(group));
		if (std::to_underlying(group) >= AudioGroupCount)
			return 0.0f;
		return m_State->GroupVolumes[std::to_underlying(group)];
	}

	Status AudioEngine::SetMasterVolume(float volume)
	{
		ENGINE_TRY(Utils::ValidateVolume(volume, "master volume"));
		ma_sound_group_set_volume(m_State->MasterGroup.Get(), volume);
		m_State->MasterVolume = volume;
		return {};
	}

	float AudioEngine::GetMasterVolume() const
	{
		return m_State->MasterVolume;
	}

	void AudioEngine::Update(double nowSeconds, double deltaSeconds)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(std::isfinite(deltaSeconds) && deltaSeconds >= 0.0, "AudioEngine::Update needs a finite delta >= 0 (got {})", deltaSeconds);
		state.HandleNotifications(nowSeconds);
		state.HandlePendingStop(nowSeconds);
		state.HandleRecreation(nowSeconds);
		// Streams the device reads still decode on this thread (deterministic decoding has no job thread).
		if (state.Specification.Decoding == AudioDecoding::Deterministic)
			state.ProcessJobs();

		if (state.GetTimeSource() == AudioTimeSource::Host && std::isfinite(deltaSeconds) && deltaSeconds > 0.0)
		{
			// The frames of the time pulled so far, rounded half up, less those already pulled: the accumulator keeps the
			// rounding of every frame, so the total never drifts from the elapsed time.
			state.HostSeconds += deltaSeconds;
			const auto target = static_cast<uint64_t>(std::floor(state.HostSeconds * static_cast<double>(AudioSampleRate) + 0.5));
			const uint64_t frames = target > state.HostFrames ? target - state.HostFrames : 0;
			state.HostFrames = std::max(state.HostFrames, target);
			if (frames > 0)
				state.Pull(frames, {});
		}
		state.ReleaseFinishedVoices();
	}

	void AudioEngine::BeginSimulationTime(uint32_t fixedHz)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(fixedHz >= 1, "BeginSimulationTime needs at least 1 tick per second");
		ENGINE_CORE_ASSERT(!state.Shared.SimulationOwned.load(), "The audio engine's simulation time is already owned");
		state.FixedHz = std::max(fixedHz, 1u);
		state.SimulationTicks = 0;
		state.SimulationFrames = 0;
		state.Shared.SimulationOwned.store(true);
		// Wait for a device read in progress: from here on the callback outputs silence.
		const std::lock_guard lock(state.Shared.ReadMutex);
	}

	void AudioEngine::EndSimulationTime()
	{
		m_State->Shared.SimulationOwned.store(false);
	}

	bool AudioEngine::IsSimulationTimeOwned() const
	{
		return m_State->Shared.SimulationOwned.load();
	}

	uint32_t AudioEngine::AdvanceSimulationTick()
	{
		State& state = *m_State;
		if (!state.Shared.SimulationOwned.load())
			return 0;
		// After n ticks at F Hz exactly round(n x 48000 / F) frames, half up, in integers.
		++state.SimulationTicks;
		const uint64_t hz = state.FixedHz;
		const uint64_t total = (state.SimulationTicks * AudioSampleRate * 2 + hz) / (2 * hz);
		const auto frames = static_cast<uint32_t>(total - state.SimulationFrames);
		state.SimulationFrames = total;
		if (frames > 0)
			state.Pull(frames, {});
		return frames;
	}

	Status AudioEngine::ReadFrames(std::span<float> interleavedStereo)
	{
		if (interleavedStereo.size() % AudioChannelCount != 0)
			return MakeError(ErrorCode::InvalidArgument, "ReadFrames needs whole stereo frames (got {} samples)", interleavedStereo.size());
		if (m_State->GetTimeSource() == AudioTimeSource::Device)
		{
			return MakeError(ErrorCode::InvalidState, "the audio device reads the engine (time source Device); frames are pulled only without a device "
													  "or while simulation time is owned");
		}
		if (!interleavedStereo.empty())
			m_State->Pull(interleavedStereo.size() / AudioChannelCount, interleavedStereo);
		return {};
	}

	AudioTimeSource AudioEngine::GetTimeSource() const
	{
		return m_State->GetTimeSource();
	}

	void AudioEngine::StartCapture()
	{
		m_State->Capture = AudioCapture{};
		m_State->Capturing = true;
	}

	AudioCapture AudioEngine::StopCapture()
	{
		if (!m_State->Capturing)
			return {};
		m_State->Capturing = false;
		return std::exchange(m_State->Capture, AudioCapture{});
	}

	bool AudioEngine::IsCapturing() const
	{
		return m_State->Capturing;
	}

	AudioDeviceState AudioEngine::GetDeviceState() const
	{
		return m_State->DeviceState;
	}

	AudioEngineStats AudioEngine::GetStats() const
	{
		const State& state = *m_State;
		return AudioEngineStats{ .DeviceKind = state.Specification.Device,
			.DeviceState = state.DeviceState,
			.TimeSource = state.GetTimeSource(),
			.Decoding = state.Specification.Decoding,
			.DeviceName = state.DeviceName,
			.LiveVoices = state.Voices.GetSize(),
			.VoiceCapacity = MaxAudioVoices,
			.RegisteredClips = state.Clips.GetSize(),
			.StartedVoices = state.StartedVoices,
			.StolenVoices = state.StolenVoices,
			.PulledFrames = state.PulledFrames,
			.DeviceReadFrames = state.Shared.ReadFrames.load(std::memory_order_relaxed),
			.DeviceSilentFrames = state.Shared.SilentFrames.load(std::memory_order_relaxed),
			.DeviceRecreations = state.DeviceRecreations,
			.FailedDeviceCreations = state.FailedDeviceCreations };
	}

	void AudioEngine::InjectDeviceNotification(AudioDeviceNotification notification)
	{
		const std::lock_guard lock(m_State->Shared.QueueMutex);
		m_State->Shared.Queue.push_back({ .Type = notification, .Injected = true });
	}

	void AudioEngine::InjectDeviceCreationFailures(uint32_t count)
	{
		m_State->InjectedFailures += count;
	}

	std::string MakeAudioResourceName(std::string_view name, uint64_t version)
	{
		return std::format("{}@{}", name, version);
	}

	AudioLevels MeasureAudioLevels(std::span<const float> interleavedStereo)
	{
		const size_t frames = interleavedStereo.size() / AudioChannelCount;
		if (frames == 0)
			return {};
		double left = 0.0;
		double right = 0.0;
		float peak = 0.0f;
		for (size_t frame = 0; frame < frames; ++frame)
		{
			const float leftSample = interleavedStereo[frame * AudioChannelCount];
			const float rightSample = interleavedStereo[frame * AudioChannelCount + 1];
			left += static_cast<double>(leftSample) * leftSample;
			right += static_cast<double>(rightSample) * rightSample;
			peak = std::max({ peak, std::abs(leftSample), std::abs(rightSample) });
		}
		return AudioLevels{ .RmsLeft = static_cast<float>(std::sqrt(left / static_cast<double>(frames))),
			.RmsRight = static_cast<float>(std::sqrt(right / static_cast<double>(frames))),
			.Peak = peak };
	}

	std::string_view AudioDeviceKindToString(AudioDeviceKind kind)
	{
		switch (kind)
		{
			case AudioDeviceKind::None:   return "None";
			case AudioDeviceKind::System: return "System";
			case AudioDeviceKind::Null:   return "Null";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDeviceKind {}", std::to_underlying(kind));
		return "Unknown";
	}

	std::string_view AudioDecodingToString(AudioDecoding decoding)
	{
		switch (decoding)
		{
			case AudioDecoding::Deterministic: return "Deterministic";
			case AudioDecoding::Threaded:      return "Threaded";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDecoding {}", std::to_underlying(decoding));
		return "Unknown";
	}

	std::string_view AudioDeviceStateToString(AudioDeviceState state)
	{
		switch (state)
		{
			case AudioDeviceState::None:       return "None";
			case AudioDeviceState::Running:    return "Running";
			case AudioDeviceState::Recreating: return "Recreating";
			case AudioDeviceState::Failed:     return "Failed";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDeviceState {}", std::to_underlying(state));
		return "Unknown";
	}

	std::string_view AudioTimeSourceToString(AudioTimeSource source)
	{
		switch (source)
		{
			case AudioTimeSource::Device:     return "Device";
			case AudioTimeSource::Simulation: return "Simulation";
			case AudioTimeSource::Host:       return "Host";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioTimeSource {}", std::to_underlying(source));
		return "Unknown";
	}

	std::string_view AudioDeviceNotificationToString(AudioDeviceNotification notification)
	{
		switch (notification)
		{
			case AudioDeviceNotification::Started:           return "Started";
			case AudioDeviceNotification::Stopped:           return "Stopped";
			case AudioDeviceNotification::Rerouted:          return "Rerouted";
			case AudioDeviceNotification::InterruptionBegan: return "InterruptionBegan";
			case AudioDeviceNotification::InterruptionEnded: return "InterruptionEnded";
			case AudioDeviceNotification::Unlocked:          return "Unlocked";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioDeviceNotification {}", std::to_underlying(notification));
		return "Unknown";
	}

}
