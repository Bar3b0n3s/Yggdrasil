#pragma once

#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Handle.h"
#include "Engine/Core/Result.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The audio engine (Architecture §10.1), one per EngineContext (§4.1: the AudioEngine step after Graphics). ECS-agnostic:
// it plays registered clips on voices; Scene/AudioSystem maps components to voices, EditorCore's AudioPreview plays
// asset-browser previews. Frozen by the M12 contract (Docs/Decisions/0015-m12-decisions.md); no miniaudio type appears in
// this header (§3 rule 3): the miniaudio objects (ma_engine, ma_resource_manager, ma_device, ma_sound, ma_sound_group) live
// behind the opaque State.
//
// The engine never owns the device (§10.1). ma_engine is always created with noDevice = MA_TRUE, 2 channels at 48 kHz, in
// every mode, over a resource manager whose VFS is an AudioVfs over the context's VirtualFileSystem. With
// AudioDeviceKind::System (windowed runs) or Null (device tests) the engine adds its own ma_device (playback, f32 stereo,
// 48 kHz) whose data callback only calls ma_engine_read_pcm_frames, or outputs silence while simulation time is owned (see
// "Time"). Device loss or a reroute therefore re-initializes only the ma_device: every sound, group and voice keeps its
// state and playback position.
//
// Device notifications (§10.1). The device's notificationCallback (miniaudio's device thread) only queues the notification;
// Update handles the queue on the main thread, in order:
//   - Rerouted (the default device changed) is logged at Info and changes nothing else;
//   - a Stopped the engine did not ask for is not acted on at once, because some backends stop the device to reroute it
//     and start it again (WASAPI posts Stopped, Rerouted, Started when the default device changes): a Rerouted or Started
//     after it cancels it. A real one still pending DeviceRecreationDelaySeconds later (by Update's clock) is unexpected
//     when the device has not started again by then; an injected one (InjectDeviceNotification) is unexpected at the end
//     of the Update that handles it, unless a Rerouted or Started follows it in that Update;
//   - an unexpected stop uninitializes the ma_device (AudioDeviceState::Recreating) and re-creates it on the current
//     default device DeviceRecreationDelaySeconds after the stop (an injected stop: after that Update); a failed attempt
//     schedules the next one as long; after MaxDeviceRecreationAttempts failed attempts the engine gives up with exactly
//     one warning and keeps running without a device (AudioDeviceState::Failed, time source Host). While Recreating
//     nothing reads the engine, so every voice holds its cursor;
//   - Started, InterruptionBegan, InterruptionEnded and Unlocked are logged at Trace.
// A device that cannot be created at startup leaves the engine device-less with one warning (AudioDeviceState::Failed);
// Create still succeeds.
//
// Time (§10.1 "Time ownership"). Exactly one party reads the engine at a time, named by GetTimeSource():
//   - Device: the device callback reads in real time (a device exists, simulation time is not owned). The main thread
//     never pulls then (ReadFrames is InvalidState).
//   - Simulation: lockstep or a test run owns time (BeginSimulationTime). The device callback outputs silence and does not
//     read the engine; each AdvanceSimulationTick pulls the frames of one tick: after n ticks at F Hz exactly
//     round(n x 48000 / F) frames (half up, an exact integer accumulator), so 800 per tick at 60 Hz, 960 at 50 Hz, and
//     333, 333, 334, ... at 144 Hz. Voices advance with simulation time (play.step {ticks: 600} plays no burst).
//   - Host: there is no device (AudioDeviceKind::None, or the device failed). Update pulls the frames of each frame's delta
//     (round half up with a fractional accumulator), so voices advance in headless runs; tests pull with ReadFrames.
// Every pull of the engine's own (AdvanceSimulationTick, Update's host pull, ReadFrames) first processes the pending
// resource-manager jobs inline when decoding is Deterministic, then reads ma_engine_read_pcm_frames, appends the frames to
// the capture buffer while capturing and discards them otherwise, and finally releases the voices that reached the end of
// a non-looping clip. A pull longer than 10 ms (480 frames) is read in chunks of at most 480 frames with the pending jobs
// processed before each chunk, so a stream's next page is decoded before the mixer needs it whatever the pull's length.
//
// Deterministic decoding (§10.1). AudioDecoding::Deterministic (tests and headless processes) creates the resource manager
// with MA_RESOURCE_MANAGER_FLAG_NO_THREADING (non-blocking) and jobThreadCount 0, and processes its jobs inline before every
// pull and every chunk of one (ma_resource_manager_process_next_job until empty), so streamed and asynchronously loaded
// clips decode at the same simulation point in every run: captured PCM of a fixed scene is bit-identical across runs, and
// streamed and decoded playback of one clip are sample-identical. Update also processes them, so a device that reads the
// engine (time source Device) finds the pages of its streams decoded. AudioDecoding::Threaded (windowed runs) keeps
// miniaudio's job thread.
//
// Clips (§10.1). A registration is known to miniaudio by its resource name, MakeAudioResourceName(Name, Version)
// ("<16 hex digits>@<version>"), so a hot-reloaded clip's new version is a separate resource while voices of the old one
// keep playing their own data (miniaudio never replaces the data of a name that is already registered). How the bytes are
// registered:
//   - Pcm16 (synthesized sound effects): ma_resource_manager_register_decoded_data over the shared bytes (ma_format_s16).
//   - Encoded without Stream (clips up to MaxDecodedClipSeconds by default): decoded once, here, with DecodeEncodedAudio
//     (Audio/AudioDecoder.h) to interleaved f32 at the clip's own rate and channel count, and that PCM registered with
//     ma_resource_manager_register_decoded_data (ma_format_f32). Every voice of the clip shares the decoded data (each
//     voice is an ma_sound_init_from_file on the registration's resource name, which acquires the same data buffer node,
//     as ma_sound_init_copy does); none decodes on the fly. ma_resource_manager_register_encoded_data is not used:
//     registered encoded data stays encoded and every voice would run its own decoder over it.
//   - Encoded with Stream: an AudioVfs memory file under the resource name, played with MA_SOUND_FLAG_STREAM. The resource
//     manager decodes streams to f32 at the clip's own rate and channels (decodedFormat ma_format_f32, decodedSampleRate
//     and decodedChannels 0), the format DecodeEncodedAudio produces, so streamed and decoded playback of one clip are
//     sample-identical.
// The engine keeps a registration's data (the shared Bytes, or the PCM it decoded) until the registration is removed and
// its last voice has ended.
//
// Pitch and Doppler bounds. miniaudio converts each voice's resampling ratio to a 32-bit fixed-point rate, so the engine
// bounds what reaches it: a voice plays at a pitch of at most 16 (a larger AudioVoiceSettings::Pitch is accepted and
// reported as given), and its Doppler factor is reduced where needed so that the factor times the speed of the voice, and
// times the speed of the listener, stays at or below half the speed of sound, which keeps the Doppler pitch within
// [1/3, 3]. Every output sample that is not finite is replaced with silence before the device or a capture gets it.
//
// Voices (§10.1). A HandlePool of MaxAudioVoices voices, each one ma_sound in its group (Master -> Music, Sfx, Ui). When the
// pool is full, PlayVoice steals the voice with the lowest priority, among those the one farthest from the listener (a
// non-spatial voice counts as distance 0), among those the oldest (the smallest start sequence); a voice is stolen only
// when its priority is not above the new voice's, else PlayVoice fails. A stolen voice's handle becomes stale.
//
// Teardown (§4.1): destroying the engine with voices alive is a programmer error (asserted); their owners (play sessions,
// the editor's preview) release them first. Registered clips are released with the engine.
//
// Thread safety: main thread only, except InjectDeviceNotification (any thread); the device callback runs on miniaudio's
// device thread and only reads the engine (§4.11). Not copyable or movable.

namespace Engine {

	class VirtualFileSystem;

	// The objects behind the engine's handles, defined in AudioEngine.cpp (Handle<T> needs only the name).
	struct AudioClipRecord;
	struct AudioVoice;

	// A registered clip (RegisterClip), and a playing voice (PlayVoice). Generational: a released clip or voice leaves its
	// handles stale, never aliasing a later object (Core/Handle.h).
	using AudioClipHandle = Handle<AudioClipRecord>;
	using AudioVoiceHandle = Handle<AudioVoice>;

	// Whether and how the engine adds its own playback device (§10.1).
	enum class AudioDeviceKind : uint8_t
	{
		None,   // no device: the host pulls frames (headless processes, tests); time source Host
		System, // the OS's default playback device (windowed runs)
		Null    // miniaudio's Null backend as a real device thread (the device-handling tests of §10.4)
	};

	// How the resource manager decodes (§10.1 "Deterministic decoding").
	enum class AudioDecoding : uint8_t
	{
		Deterministic, // MA_RESOURCE_MANAGER_FLAG_NO_THREADING, jobs processed inline before each pull (tests, headless)
		Threaded       // miniaudio's job thread (windowed runs)
	};

	struct AudioEngineSpecification
	{
		AudioDeviceKind Device = AudioDeviceKind::None;
		AudioDecoding Decoding = AudioDecoding::Deterministic;
		// Test instrumentation (§10.4): the first this many device creations fail as if the backend had refused them, the one
		// at Create included, so the startup failure is testable with the Null backend. Applications leave it 0.
		uint32_t InjectedDeviceCreationFailures = 0;
	};

	// The state of the engine's device (audio.stats "deviceState").
	enum class AudioDeviceState : uint8_t
	{
		None,       // AudioDeviceKind::None: device-less by configuration
		Running,    // the device exists and runs
		Recreating, // an unexpected stop: the device is gone and a re-creation is scheduled
		Failed      // no device could be created at startup, or every re-creation failed: device-less
	};

	// Who reads the engine (see "Time" above; audio.stats "timeSource").
	enum class AudioTimeSource : uint8_t
	{
		Device,     // the device callback, in real time
		Simulation, // AdvanceSimulationTick, per tick (lockstep or a test run)
		Host        // Update per frame's delta, or ReadFrames (no device)
	};

	// miniaudio's device notifications (ma_device_notification_type), queued by the device thread for Update.
	enum class AudioDeviceNotification : uint8_t
	{
		Started,
		Stopped,
		Rerouted,
		InterruptionBegan,
		InterruptionEnded,
		Unlocked
	};

	// How a registered clip's bytes are encoded.
	enum class AudioClipFormat : uint8_t
	{
		Encoded, // a WAV, FLAC, MP3 or Ogg Vorbis file, decoded by miniaudio (the container is detected from the bytes)
		Pcm16    // interleaved little-endian signed 16-bit PCM (synthesized sound effects: 48 kHz mono, §6.6)
	};

	// One clip to register (§10.1 "Clips"). Scene/AudioSystem.h's MakeAudioClipSource builds it from a loaded AudioClipData.
	struct AudioClipSource
	{
		// The clip's name: its AssetHandle as 16 lowercase hex digits (§10.1). Non-empty, without "://" or '@'.
		std::string Name{};
		// The asset's version (AssetManager::GetVersion): a hot-reloaded clip registers again under the same Name with a new
		// version, a separate registration (MakeAudioResourceName); voices of the old version keep playing its data.
		uint64_t Version = 1;
		AudioClipFormat Format = AudioClipFormat::Encoded;
		// The clip's bytes; never null. Shared, never copied: the engine keeps them while the clip is registered or a voice
		// plays it (AudioSystem passes an aliasing pointer that keeps the loaded AudioClipData alive).
		Ref<const Buffer> Bytes{};
		// Pcm16 only: the sample rate (8,000 to 192,000 Hz) and the channel count (1 or 2); Bytes holds a whole number of
		// frames. Ignored for Encoded clips, whose header says.
		uint32_t SampleRate = AudioSampleRate;
		uint32_t ChannelCount = 1;
		// Encoded only: stream through the AudioVfs (MA_SOUND_FLAG_STREAM) instead of decoding the whole clip once at
		// registration (see "Clips"). AudioImporter decides it (§7.4: by default, clips longer than MaxDecodedClipSeconds
		// stream).
		bool Stream = false;
	};

	// What may change while a voice plays (SetVoiceSettings); also part of the voice's description.
	struct AudioVoiceSettings
	{
		AudioGroup Group = AudioGroup::Sfx; // a change re-attaches the voice to the new group
		float Volume = 1.0f;                // linear, finite, >= 0
		float Pitch = 1.0f;                 // a multiplier of the clip's speed, finite, >= MinAudioPitch
		bool Loop = false;
		// Spatial voices are positioned (SetVoiceTransform) and attenuated; non-spatial ones use
		// MA_SOUND_FLAG_NO_SPATIALIZATION (§10.2).
		bool Spatial = false;
		AudioSpatialization Spatialization{};

		bool operator==(const AudioVoiceSettings&) const = default;
	};

	// A spatial voice's pose in world space (§10.2: position, direction and velocity, metres and metres per second); every
	// component finite. Ignored by non-spatial voices.
	struct AudioVoiceTransform
	{
		glm::vec3 Position{ 0.0f };
		glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
		glm::vec3 Velocity{ 0.0f };

		bool operator==(const AudioVoiceTransform&) const = default;
	};

	// One voice to start (PlayVoice).
	struct AudioVoiceDescription
	{
		AudioClipHandle Clip{};
		AudioVoiceSettings Settings{};
		AudioVoiceTransform Transform{};
		// Stealing order (see "Voices"): a lower priority is stolen first. AudioSystem uses AudioSystemEffectPriority and
		// AudioSystemMusicPriority (Scene/AudioSystem.h), the editor's preview AudioPreviewPriority
		// (EditorCore/Audio/AudioPreview.h).
		int32_t Priority = 0;
		// Starts paused at the clip's beginning (a paused play session's new voices, AudioSystem).
		bool StartPaused = false;
		// An opaque number the engine only reports (AudioVoiceInfo::Owner): AudioSystem stores the AudioSource entity's UUID
		// value, so audio.stats can name the entity; 0 for one-shots and previews.
		uint64_t Owner = 0;
	};

	// A snapshot of one live voice (GetVoiceInfo, GetVoices; audio.stats).
	struct AudioVoiceInfo
	{
		AudioVoiceHandle Voice{};
		AudioClipHandle Clip{};
		std::string ClipName{}; // AudioClipSource::Name of its clip
		uint64_t ClipVersion = 0;
		uint64_t Owner = 0;
		int32_t Priority = 0;
		AudioVoiceSettings Settings{};
		AudioVoiceTransform Transform{};
		bool Paused = false;
		bool Streamed = false;
		// The voice's playback position and its clip's length in the clip's own frames; LengthFrames is 0 when the decoder
		// cannot tell yet (a stream before its first page). CursorFrames is the frames the engine has mixed of the voice:
		// ma_sound_get_cursor_in_pcm_frames minus the frames the voice has read ahead but the mixer has not output yet
		// (modulo the length for a looping voice). It is exact while the engine pulls its own frames (time sources
		// Simulation and Host) or has no device; while the device may read the engine it is miniaudio's cursor alone, a
		// snapshot that can run ahead of the output by the voice's read-ahead.
		uint64_t CursorFrames = 0;
		uint64_t LengthFrames = 0;
		// The order voices were started in (1 for the engine's first voice): the "oldest" of the stealing rule.
		uint64_t StartSequence = 0;
	};

	// Counters and state for audio.stats and the tests (GetStats).
	struct AudioEngineStats
	{
		AudioDeviceKind DeviceKind = AudioDeviceKind::None;
		AudioDeviceState DeviceState = AudioDeviceState::None;
		AudioTimeSource TimeSource = AudioTimeSource::Host;
		AudioDecoding Decoding = AudioDecoding::Deterministic;
		std::string DeviceName{}; // the device's name; empty without a device
		uint32_t LiveVoices = 0;
		uint32_t VoiceCapacity = MaxAudioVoices;
		uint32_t RegisteredClips = 0;
		uint64_t StartedVoices = 0;
		uint64_t StolenVoices = 0;
		// Frames the engine pulled itself (AdvanceSimulationTick, Update's host pull, ReadFrames).
		uint64_t PulledFrames = 0;
		// Frames the device callback read from the engine (time source Device).
		uint64_t DeviceReadFrames = 0;
		// Frames the device callback filled with silence without reading the engine (time source Simulation).
		uint64_t DeviceSilentFrames = 0;
		uint32_t DeviceRecreations = 0;     // successful re-creations after an unexpected stop
		uint32_t FailedDeviceCreations = 0; // failed creations, at startup and in re-creations
	};

	// The frames pulled while capturing (StopCapture): interleaved stereo f32 at 48 kHz, two samples per frame.
	struct AudioCapture
	{
		std::vector<float> Samples{};
		// True when the capture reached AudioEngine::MaxCaptureFrames and later frames were dropped.
		bool Truncated = false;

		[[nodiscard]] uint64_t GetFrameCount() const { return Samples.size() / AudioChannelCount; }
	};

	// Levels of interleaved stereo frames (Test.CaptureAudio(ticks) -> {RmsLeft, RmsRight, Peak}, M13; the §10.4 tests).
	struct AudioLevels
	{
		float RmsLeft = 0.0f;
		float RmsRight = 0.0f;
		float Peak = 0.0f; // the largest absolute sample of either channel
	};

	class AudioEngine
	{
	public:
		// §10.1 device re-creation: the delay after an unexpected stop and between attempts, and the attempts before giving up.
		static constexpr double DeviceRecreationDelaySeconds = 1.0;
		static constexpr uint32_t MaxDeviceRecreationAttempts = 3;
		// The longest capture: 10 minutes of frames (about 230 MB of samples); later frames are dropped (AudioCapture::Truncated).
		static constexpr uint64_t MaxCaptureFrames = static_cast<uint64_t>(AudioSampleRate) * 600;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class AudioEngine;
		};

		// Use Create.
		explicit AudioEngine(ConstructionKey key);
		~AudioEngine();

		AudioEngine(const AudioEngine&) = delete;
		AudioEngine& operator=(const AudioEngine&) = delete;

		// The engine of `specification` over `vfs` (a documented back-reference that outlives the engine: the EngineContext's
		// VFS, which the AudioVfs reads paths through): ma_engine (device-less, 2 channels, 48 kHz), the resource manager with
		// the AudioVfs and the specification's decoding, the groups, and the device for System and Null (a device that cannot
		// be created is the warning of "Device notifications", not an error). Errors: Unsupported or Io when miniaudio cannot
		// create the engine, the resource manager or a group (with miniaudio's result description).
		[[nodiscard]] static Result<Scope<AudioEngine>> Create(const AudioEngineSpecification& specification, const VirtualFileSystem& vfs);

		[[nodiscard]] const AudioEngineSpecification& GetSpecification() const;

		// --- Clips (§10.1) ---------------------------------------------------------------------------------------------

		// Registers `source` under MakeAudioResourceName(Name, Version) (see "Clips"), or adds a reference to the registration
		// of the same Name and Version; each successful call is balanced by one UnregisterClip. An Encoded clip without Stream
		// is decoded here, on the calling thread, in every decoding mode. Errors: InvalidArgument for an empty name or one
		// containing "://" or '@', null bytes, a Pcm16 clip with a sample rate outside 8,000 to 192,000 Hz, a channel count
		// other than 1 or 2 or a partial frame, or Stream on a Pcm16 clip; DecodeEncodedAudio's errors (Parse, Validation)
		// for an Encoded clip without Stream that does not decode; Io or Unsupported when miniaudio refuses the registration
		// (with its result description); the AudioVfs's errors.
		[[nodiscard]] Result<AudioClipHandle> RegisterClip(const AudioClipSource& source);

		// Drops one reference of RegisterClip; the last one removes the registration (the resource name, the memory file of a
		// stream). Voices playing the clip keep its data until they end. Errors: NotFound for a stale or null handle.
		[[nodiscard]] Status UnregisterClip(AudioClipHandle clip);

		[[nodiscard]] bool IsClipRegistered(AudioClipHandle clip) const;

		// --- Voices (§10.1, §10.2) ---------------------------------------------------------------------------------------

		// Starts a voice (see "Voices" for a full pool). Errors: NotFound for a stale clip handle; InvalidArgument for
		// settings or a transform outside their documented ranges; InvalidState "all 64 voices have a higher priority"
		// (with the counts) when nothing may be stolen; Io or Unsupported when miniaudio cannot start the sound (a stream
		// whose data cannot be decoded).
		[[nodiscard]] Result<AudioVoiceHandle> PlayVoice(const AudioVoiceDescription& description);

		// Stops and releases the voice; its handle becomes stale. Errors: NotFound for a stale or null handle.
		[[nodiscard]] Status StopVoice(AudioVoiceHandle voice);
		// Pauses (ma_sound_stop, which keeps the cursor) or resumes the voice. Errors: NotFound.
		[[nodiscard]] Status SetVoicePaused(AudioVoiceHandle voice, bool paused);
		// Applies `settings` to a playing voice. Errors: NotFound; InvalidArgument as PlayVoice.
		[[nodiscard]] Status SetVoiceSettings(AudioVoiceHandle voice, const AudioVoiceSettings& settings);
		// Moves a spatial voice (ignored, yet validated, for a non-spatial one). Errors: NotFound; InvalidArgument for a
		// non-finite component.
		[[nodiscard]] Status SetVoiceTransform(AudioVoiceHandle voice, const AudioVoiceTransform& transform);

		// False for a null or stale handle: the voice was stopped, stolen, or released after reaching the end of a
		// non-looping clip (at the end of the pull that played its last frame, or at the next Update).
		[[nodiscard]] bool IsVoiceAlive(AudioVoiceHandle voice) const;
		// Errors: NotFound for a stale or null handle.
		[[nodiscard]] Result<AudioVoiceInfo> GetVoiceInfo(AudioVoiceHandle voice) const;
		// Every live voice, by StartSequence.
		[[nodiscard]] std::vector<AudioVoiceInfo> GetVoices() const;

		// --- Listener and groups (§10.2, §11.5 Audio.SetGroupVolume) -------------------------------------------------------

		// Listener 0's pose. Errors: InvalidArgument for a non-finite component, a zero Forward or Up, or parallel ones.
		[[nodiscard]] Status SetListener(const AudioListenerPose& pose);
		[[nodiscard]] AudioListenerPose GetListener() const;
		// A group's linear volume (ma_sound_group_set_volume). Errors: InvalidArgument for a volume that is not finite or below 0.
		[[nodiscard]] Status SetGroupVolume(AudioGroup group, float volume);
		[[nodiscard]] float GetGroupVolume(AudioGroup group) const;
		// The Master group's linear volume, over every group. Errors: as SetGroupVolume.
		[[nodiscard]] Status SetMasterVolume(float volume);
		[[nodiscard]] float GetMasterVolume() const;

		// --- Time (§10.1) --------------------------------------------------------------------------------------------------

		// Once per frame on the main thread (Application, after OnUpdate), `nowSeconds` a monotonic time (the frame clock's
		// accumulated unscaled time, so headless runs on a ManualClock are deterministic; tests pass chosen times) and
		// `deltaSeconds` the frame's unscaled delta (finite, >= 0): handles the queued device notifications and a due
		// re-creation ("Device notifications"), releases voices that reached their end, and with time source Host pulls
		// round(deltaSeconds x 48000) frames (fractional accumulator).
		void Update(double nowSeconds, double deltaSeconds);

		// Lockstep or a test run takes time (time source Simulation) at `fixedHz` ticks per second (>= 1, asserted; the
		// session validated it against [1, FrameLoopConfig::MaxFixedHz], and above 48,000 Hz some ticks pull no frame): from
		// now on the device callback outputs silence without reading the engine, waiting for a read in progress to finish
		// first, and only AdvanceSimulationTick advances voices. The tick accumulator starts at 0. Asserts that simulation
		// time is not owned already (one owner: the play session, PlaySession::SetLockstep).
		void BeginSimulationTime(uint32_t fixedHz);
		// Gives time back: Device with a running device, else Host. Does nothing when simulation time is not owned.
		void EndSimulationTime();
		[[nodiscard]] bool IsSimulationTimeOwned() const;
		// Pulls the frames of one tick (see "Time") and returns how many; 0 when simulation time is not owned.
		uint32_t AdvanceSimulationTick();

		// Pulls interleavedStereo.size() / 2 frames into `interleavedStereo` (and the capture buffer while capturing): what
		// the tests read from a device-less engine. Errors: InvalidArgument for an odd sample count; InvalidState while the
		// device reads the engine (time source Device).
		[[nodiscard]] Status ReadFrames(std::span<float> interleavedStereo);

		[[nodiscard]] AudioTimeSource GetTimeSource() const;

		// --- Capture (Test.CaptureAudio, M13) ------------------------------------------------------------------------------

		// Starts capturing every frame the engine pulls (never the device's reads), dropping an earlier capture.
		void StartCapture();
		// Stops capturing and returns the frames pulled since StartCapture (empty when not capturing).
		[[nodiscard]] AudioCapture StopCapture();
		[[nodiscard]] bool IsCapturing() const;

		// --- Device and statistics ---------------------------------------------------------------------------------------

		[[nodiscard]] AudioDeviceState GetDeviceState() const;
		[[nodiscard]] AudioEngineStats GetStats() const;

		// --- Test instrumentation (§10.4: "a fake notification source") ---------------------------------------------------

		// Queues `notification` as if the device's notification callback had posted it (Update handles it). A Stopped
		// injected this way counts as unexpected, whether or not the device really stopped, unless a Rerouted or Started
		// follows it in the same Update (see "Device notifications"). Thread-safe.
		void InjectDeviceNotification(AudioDeviceNotification notification);
		// Makes the next `count` device re-creations fail as if the backend had refused them (startup failures:
		// AudioEngineSpecification::InjectedDeviceCreationFailures).
		void InjectDeviceCreationFailures(uint32_t count);
	private:
		// The miniaudio objects, the AudioVfs, the clip and voice pools, the listener, the time state, the capture buffer and
		// the notification queue (AudioEngine.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// The resource name of a clip registration (see "Clips"): `name`, '@', then `version` in decimal ("00000000000000a1@2").
	// Pure.
	[[nodiscard]] std::string MakeAudioResourceName(std::string_view name, uint64_t version);

	// The levels of interleaved stereo frames (an odd trailing sample is ignored); zeros for no frames. Pure.
	[[nodiscard]] AudioLevels MeasureAudioLevels(std::span<const float> interleavedStereo);

	// Enumerator names ("System", "Recreating", "Simulation", "Rerouted", ...); "Unknown" outside the enums (asserted).
	[[nodiscard]] std::string_view AudioDeviceKindToString(AudioDeviceKind kind);
	[[nodiscard]] std::string_view AudioDecodingToString(AudioDecoding decoding);
	[[nodiscard]] std::string_view AudioDeviceStateToString(AudioDeviceState state);
	[[nodiscard]] std::string_view AudioTimeSourceToString(AudioTimeSource source);
	[[nodiscard]] std::string_view AudioDeviceNotificationToString(AudioDeviceNotification notification);

}
