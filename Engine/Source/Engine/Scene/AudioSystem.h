#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Components to voices (Architecture §10.2): the AudioSystem of one play session, the listener rule and the audio
// validation codes. Frozen by the M12 contract (Docs/Decisions/0015-m12-decisions.md); PlaySession owns one AudioSystem per
// Play session (none in Simulate mode, §5.6, and none without an AudioEngine) and calls it at the points the contract wired
// (Session/PlaySession.cpp).

namespace Engine {

	class AssetManager;
	class ConstEntity;
	class Entity;
	class Scene;

	// The voice priorities AudioSystem starts voices with (AudioVoiceDescription::Priority; the stealing rule of
	// AudioEngine.h): Sfx and Ui voices are stolen before music, so a burst of effects never silences the soundtrack.
	inline constexpr int32_t AudioSystemEffectPriority = 0; // Sfx and Ui sources, and every one-shot
	inline constexpr int32_t AudioSystemMusicPriority = 1;  // sources in the Music group

	// The validation codes of the audio system (§13.7), reported by EditorCore's ProjectValidator through
	// FindAudioSceneIssues.
	// Warning, not fixable: spatial AudioSources in a scene without a primary AudioListener and without a primary camera
	// (§10.2); they are heard from the origin.
	inline constexpr std::string_view AudioNoListenerCode = "AUDIO_NO_LISTENER";
	// Warning, fixable: several active primary AudioListeners; the first in canonical order is used (§5.3, §10.2). The fix
	// clears Primary on every later one.
	inline constexpr std::string_view AudioMultiplePrimaryListenersCode = "AUDIO_MULTIPLE_PRIMARY_LISTENERS";

	// Where the scene is heard from (SelectAudioListener).
	enum class AudioListenerSource : uint8_t
	{
		None,     // no listener and no primary camera: the origin, looking down -Z
		Listener, // the first active primary AudioListener in canonical order
		Camera    // the primary camera (the first active CameraComponent with Primary in canonical order)
	};

	struct AudioListenerSelection
	{
		AudioListenerSource Source = AudioListenerSource::None;
		UUID Entity{}; // the listener's or the camera's entity; invalid for None
		// Active AudioListeners with Primary (more than one raises AUDIO_MULTIPLE_PRIMARY_LISTENERS).
		uint32_t PrimaryListenerCount = 0;

		bool operator==(const AudioListenerSelection&) const = default;
	};

	// The listener rule of §10.2 over the scene's effectively enabled entities (HierarchyDisabledTag and DisabledTag
	// excluded) in canonical order. Pure; main thread (it reads the scene).
	[[nodiscard]] AudioListenerSelection SelectAudioListener(const Scene& scene);

	// One audio validation finding.
	struct AudioSceneIssue
	{
		std::string Code{}; // AudioNoListenerCode or AudioMultiplePrimaryListenersCode
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{}; // AUDIO_MULTIPLE_PRIMARY_LISTENERS: one issue per extra primary listener (each fixable); AUDIO_NO_LISTENER:
					   // the first spatial AudioSource in canonical order
		std::string Message{};
		std::string Hint{};
		bool AutoFixable = false;
	};

	// The audio checks of §13.7 over `scene` (enabled entities, canonical order). Pure; main thread.
	[[nodiscard]] std::vector<AudioSceneIssue> FindAudioSceneIssues(const Scene& scene);

	// The AudioEngine registration of a loaded clip (§10.1): Name is `handle` as 16 lowercase hex digits, Version `version`,
	// Format Pcm16 for AudioClipEncoding::Pcm16 (with its rate and channels) and Encoded otherwise, Stream as cooked, and
	// Bytes an aliasing pointer to clip->Bytes that keeps the AudioClipData alive. Asserts a non-null clip and a valid
	// handle. Pure.
	[[nodiscard]] AudioClipSource MakeAudioClipSource(AssetHandle handle, uint64_t version, const AssetRef<AudioClipData>& clip);

	struct AudioSystemSpecification
	{
		// The context's audio engine (a documented back-reference that outlives the system); never null (asserted).
		AudioEngine* Audio = nullptr;
		// The context's asset manager, through which clips load (GetOrPlaceholder<AudioClipData>, so a missing or failed clip
		// plays the silent clip and records its diagnostic once, §7.2, §10.4). Null: every clip is missing (silent), with one
		// warning per clip.
		AssetManager* Assets = nullptr;
	};

	// The audio of one play scene (§10.2). Not copyable or movable; main thread only (§4.11).
	//
	// Sources. Every AudioSourceComponent of an effectively enabled entity may own one voice (a private runtime component,
	// AudioSourceRuntime of §5.3, defined in Scene/Private): created by Play, by PlayOnStart (at Start for the scene's sources,
	// at the first Update after the component exists for sources created later, through the scene registry's construct
	// signal) and released by Stop, by the voice reaching the end of a non-looping clip, by the component or entity being
	// destroyed (the destroy flush, §5.7 step 8, through the destroy signal) and by the entity becoming effectively
	// disabled (a PlayOnStart source starts again when it is enabled again). Every Update applies the component's fields to
	// its voice (Volume, Pitch, Loop, Spatial, MinDistance, MaxDistance, Attenuation, Rolloff, DopplerFactor, Group; a
	// changed Clip restarts a playing voice with the new clip) and, for spatial sources, the entity's world position, its
	// forward axis (-Z, §5.2) as direction and its velocity from the world-position delta over the frame's delta (0 for a
	// zero delta and on the first frame). Clips load through the asset manager and are registered with the engine once per
	// (handle, version) (MakeAudioClipSource); they are unregistered when the system is destroyed.
	//
	// Listener. Every Update sets the engine's listener from SelectAudioListener: the entity's world position, forward (-Z)
	// and up (+Y) axes and velocity, or the origin pose for AudioListenerSource::None.
	//
	// One-shots (§11.5 Audio.PlayOneShot). Non-spatial without a position, spatial at the position (default
	// AudioSpatialization) otherwise; group Sfx unless one is given; priority AudioSystemEffectPriority; released when they end.
	//
	// Pause and stop (§10.2). SetPaused(true) pauses every voice of the system (sources and one-shots; new voices start
	// paused) and SetPaused(false) resumes them where they were; both are idempotent. Destroying the system is Stop: every
	// voice it started is released, its clips unregistered and the engine's group volumes restored (see "Mixer").
	//
	// Mixer (§11.5 Audio.SetGroupVolume and GetGroupVolume, bound to scripts in M13). The group volumes a game sets belong to
	// its session: Start records the engine's Music, Sfx and Ui volumes and sets all three to 1, so every session (and every
	// FeatureTest capture) starts from the same mix whatever the editor or an earlier session left; SetGroupVolume sets the
	// engine's group; destroying the system restores the recorded volumes (nothing when Start never ran), so Stop leaves
	// the editor's mixer as it was. The Master volume is the host's (AudioEngine::SetMasterVolume) and never the session's.
	class AudioSystem
	{
	public:
		// Connects to `scene`'s construct and destroy signals of AudioSourceComponent; starts nothing (Start does). `scene`
		// is a documented back-reference that outlives the system (the session destroys the system first).
		AudioSystem(Scene& scene, const AudioSystemSpecification& specification);
		~AudioSystem();

		AudioSystem(const AudioSystem&) = delete;
		AudioSystem& operator=(const AudioSystem&) = delete;

		// Session setup (§5.6: "PlayOnStart audio starts"): records and resets the group volumes (see "Mixer"), plays every
		// PlayOnStart source of an effectively enabled entity, in canonical order, and sets the listener. Called once, at the
		// end of PlaySession::Create.
		void Start();

		// §5.7 frame phase step 3 (PlaySessionPhase::AudioUpdate), after the frame's TransformSystem::Update: listener, source
		// fields, positions, directions and velocities, pending PlayOnStart sources, released finished voices.
		// `deltaSeconds` is the frame's (scaled) delta, finite and >= 0 (asserted).
		void Update(double deltaSeconds);

		// Play-mode pause (§10.2; PlaySession::SetPaused).
		void SetPaused(bool paused);
		[[nodiscard]] bool IsPaused() const;

		// The AudioSource methods of §11.5 (bound to scripts in M13), for an effectively enabled entity of this system's scene
		// with an AudioSourceComponent. Play restarts a playing source from the start; Stop releases its voice; Pause and
		// Resume keep its cursor (a paused source stays paused across SetPaused(false)). Errors: InvalidArgument for an entity
		// without an AudioSourceComponent or of another scene; InvalidState for a disabled entity (Play, Resume); the engine's
		// errors (Play; a missing clip plays the silent clip, §7.2). Stop, Pause and Resume of a source that does not play
		// succeed and change nothing.
		[[nodiscard]] Status Play(Entity entity);
		[[nodiscard]] Status Stop(Entity entity);
		[[nodiscard]] Status Pause(Entity entity);
		[[nodiscard]] Status Resume(Entity entity);
		// True while the source's voice exists and is not paused by Pause (SetPaused does not change it).
		[[nodiscard]] bool IsPlaying(ConstEntity entity) const;

		// §11.5 Audio.PlayOneShot(clip, position?, volume?, group?) (see "One-shots"). Errors: InvalidArgument for a null clip
		// handle, a volume that is not finite or below 0, or a non-finite position; the engine's errors (a full pool of
		// higher-priority voices). A missing clip, or an asset of another type, plays the silent clip and records its
		// diagnostic once (§7.2).
		[[nodiscard]] Result<AudioVoiceHandle> PlayOneShot(AssetHandle clip, const std::optional<glm::vec3>& position, float volume = 1.0f,
			AudioGroup group = AudioGroup::Sfx);

		// §11.5 Audio.SetGroupVolume(group, volume) (see "Mixer"): the engine's linear volume of `group` for the rest of the
		// session. Errors: InvalidArgument for a volume that is not finite or below 0.
		[[nodiscard]] Status SetGroupVolume(AudioGroup group, float volume);
		// §11.5 Audio.GetGroupVolume(group): the engine's current volume of `group` (1 for each group after Start).
		[[nodiscard]] float GetGroupVolume(AudioGroup group) const;

		// The listener the last Start or Update used.
		[[nodiscard]] const AudioListenerSelection& GetListener() const;
	private:
		// The scene and engine back-references, the signal connections, the registered clips by (handle, version), the
		// one-shot voices, the pause state, the group volumes Start recorded and the last listener (AudioSystem.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// "None", "Listener", "Camera"; "Unknown" outside the enum (asserted).
	[[nodiscard]] std::string_view AudioListenerSourceToString(AudioListenerSource source);

}
