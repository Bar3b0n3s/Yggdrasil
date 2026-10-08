#include "EnginePCH.h"
#include "Engine/Scene/AudioSystem.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/AudioSourceRuntime.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <utility>

// Scene is a simulation-path module (§4.12): nothing here uses a CRT transcendental function. Audio is not simulated
// state, so nothing here reaches the state hash either; AudioSourceRuntime is a runtime-only component.

namespace Engine {

	namespace {

		// The clip a voice plays: the handle and version it is registered under (the placeholder's when the requested clip is
		// unavailable) and its data.
		struct ResolvedAudioClip
		{
			AssetHandle Handle{};
			uint64_t Version = 1;
			AssetRef<AudioClipData> Data{};
		};

		constexpr std::array<AudioGroup, AudioGroupCount> AllAudioGroups = { AudioGroup::Music, AudioGroup::Sfx, AudioGroup::Ui };

		// The speed of sound in air in metres per second (the engine's Doppler model uses the same): a world position that
		// moves this fast or faster in one frame jumped (ComputeVelocity).
		constexpr double MaxSourceSpeed = 343.3;

	}

	namespace Utils {

		static bool IsFiniteVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static bool IsValidAudioGroup(AudioGroup group)
		{
			return std::to_underlying(group) < AudioGroupCount;
		}

		static bool IsEffectivelyEnabled(ConstEntity entity)
		{
			return !entity.HasComponent<HierarchyDisabledTag>();
		}

		// The world matrix the frame phase computed (TransformSystem::Update runs before AudioSystem::Update, §5.7), or the
		// chain walk for an entity the last TransformSystem::Update did not see yet.
		static glm::mat4 GetWorldMatrix(ConstEntity entity)
		{
			if (const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>())
				return world->Matrix;
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		// `axis` normalized, or `fallback` for a zero or non-finite axis (a degenerate world matrix written by a runtime system).
		static glm::vec3 NormalizeOr(const glm::vec3& axis, const glm::vec3& fallback)
		{
			const float length = glm::length(axis);
			if (!std::isfinite(length) || length <= 0.0f)
				return fallback;
			return axis / length;
		}

		// The entity's forward axis (-Z, §5.2) in world space, normalized.
		static glm::vec3 GetWorldForward(const glm::mat4& world)
		{
			return NormalizeOr(-glm::vec3(world[2]), glm::vec3(0.0f, 0.0f, -1.0f));
		}

		// The velocity of a world position that moved from `previous` over `deltaSeconds`; zero without a previous position
		// or for a zero delta (§10.2: "velocity from the transform delta"), and zero for a move at or above the speed of
		// sound, which is a jump (a teleport, a respawn, a camera cut) rather than motion: its Doppler shift would chirp
		// every voice for a frame.
		static glm::vec3 ComputeVelocity(const glm::vec3& position, const glm::vec3& previous, bool hasPrevious, double deltaSeconds)
		{
			if (!hasPrevious || deltaSeconds <= 0.0)
				return glm::vec3(0.0f);
			const glm::vec3 velocity = (position - previous) / static_cast<float>(deltaSeconds);
			if (!IsFiniteVector(velocity))
				return glm::vec3(0.0f);
			const double speedSquared = glm::dot(glm::dvec3(velocity), glm::dvec3(velocity));
			if (speedSquared >= MaxSourceSpeed * MaxSourceSpeed)
				return glm::vec3(0.0f);
			return velocity;
		}

		// `value` when it is finite and at least `minimum`, else `minimum` (or `fallback` for a non-finite value).
		static float ClampFiniteAbove(float value, float minimum, float fallback)
		{
			if (!std::isfinite(value))
				return fallback;
			return std::max(value, minimum);
		}

		// The engine settings of an AudioSource (§10.2). Registry validation keeps authored values in range; values a runtime
		// system wrote directly are clamped into the engine's ranges (AudioTypes.h) instead of failing every update.
		static AudioVoiceSettings MakeVoiceSettings(const AudioSourceComponent& source)
		{
			AudioVoiceSettings settings;
			settings.Group = IsValidAudioGroup(source.Group) ? source.Group : AudioGroup::Sfx;
			settings.Volume = ClampFiniteAbove(source.Volume, 0.0f, 0.0f);
			settings.Pitch = ClampFiniteAbove(source.Pitch, MinAudioPitch, 1.0f);
			settings.Loop = source.Loop;
			settings.Spatial = source.Spatial;
			AudioSpatialization& spatialization = settings.Spatialization;
			spatialization.Model = std::to_underlying(source.Attenuation) <= std::to_underlying(Attenuation::Exponential) ? source.Attenuation : Attenuation::Inverse;
			spatialization.MinDistance = ClampFiniteAbove(source.MinDistance, MinAudioDistance, 1.0f);
			spatialization.MaxDistance = ClampFiniteAbove(source.MaxDistance, spatialization.MinDistance, std::max(50.0f, spatialization.MinDistance));
			spatialization.Rolloff = ClampFiniteAbove(source.Rolloff, 0.0f, 1.0f);
			spatialization.DopplerFactor = ClampFiniteAbove(source.DopplerFactor, 0.0f, 1.0f);
			return settings;
		}

		// The stealing priority of a source's voice, decided by its group when the voice starts (AudioSystem.h).
		static int32_t GetSourcePriority(AudioGroup group)
		{
			return group == AudioGroup::Music ? AudioSystemMusicPriority : AudioSystemEffectPriority;
		}

	}

	struct AudioSystem::State
	{
		Scene* TargetScene = nullptr;   // documented back-reference
		AudioEngine* Audio = nullptr;   // documented back-reference
		AssetManager* Assets = nullptr; // documented back-reference, may be null
		AudioListenerSelection Listener{};
		// The listener entity of the last Start or Update and its world position, for the listener's velocity.
		UUID ListenerEntity{};
		glm::vec3 ListenerPosition{ 0.0f };
		bool HasListenerPosition = false;
		// The clips this system registered with the engine, by (handle, version); each holds one registration reference,
		// dropped when the system is destroyed.
		std::map<std::pair<AssetHandle, uint64_t>, AudioClipHandle> Clips;
		// The one-shot voices started and not yet seen to end.
		std::vector<AudioVoiceHandle> OneShots;
		// SetPaused (play-mode pause).
		bool Paused = false;
		// Start ran: the group volumes it recorded are restored at destruction.
		bool Started = false;
		std::array<float, AudioGroupCount> RecordedGroupVolumes{ 1.0f, 1.0f, 1.0f };
		// Without an asset manager: the silent clip every source plays, and the clips already warned about (one warning each).
		AssetRef<AudioClipData> Silence{};
		std::set<AssetHandle> WarnedClips;

		// The registry signals of AudioSystem.h (connected by the constructor, disconnected by the destructor). A source
		// created after Start gets a runtime that the next Update starts when it is PlayOnStart (AudioSourceRuntime::Enabled);
		// removing the component or destroying the entity (the destroy flush) removes the runtime, and removing the runtime
		// releases its voice, whichever of the two components EnTT destroys first.
		void OnSourceConstructed(entt::registry& registry, entt::entity entity);
		void OnSourceDestroyed(entt::registry& registry, entt::entity entity);
		void OnRuntimeDestroyed(entt::registry& registry, entt::entity entity);

		// Stops `voice` when it is alive and nulls it.
		void ReleaseVoice(AudioVoiceHandle& voice);
		// The clip `handle` as the voice plays it (see AudioSystemSpecification::Assets).
		[[nodiscard]] ResolvedAudioClip ResolveClip(AssetHandle handle);
		// The engine registration of `handle`'s current version, registered on first use.
		[[nodiscard]] Result<AudioClipHandle> AcquireClip(AssetHandle handle);
		// The runtime of `entity`'s source, added when missing.
		[[nodiscard]] AudioSourceRuntime& GetRuntime(Entity entity);
		// Starts the voice of the source on `entity` from the beginning of its clip, at `transform`; nothing for a null Clip
		// ("null plays nothing"). Errors: those of AcquireClip and AudioEngine::PlayVoice.
		[[nodiscard]] Status StartSourceVoice(Entity entity, const AudioSourceComponent& source, AudioSourceRuntime& runtime,
			const AudioVoiceTransform& transform);
		// The pose of the source on `entity` with the given world matrix and velocity.
		[[nodiscard]] static AudioVoiceTransform MakeSourceTransform(const glm::mat4& world, const glm::vec3& velocity);
		// The validated source of an AudioSource method's entity. Errors: InvalidArgument for an invalid entity, one of another
		// scene or one without an AudioSourceComponent.
		[[nodiscard]] Result<AudioSourceComponent*> FindSource(Entity entity) const;
		// Sets the engine's listener from SelectAudioListener (see "Listener" in AudioSystem.h).
		void UpdateListener(double deltaSeconds);
		// One source's part of Update.
		void UpdateSource(Entity entity, AudioSourceComponent& source, double deltaSeconds);
	};

	void AudioSystem::State::OnSourceConstructed(entt::registry& registry, entt::entity entity)
	{
		if (AudioSourceRuntime* runtime = registry.try_get<AudioSourceRuntime>(entity))
		{
			ReleaseVoice(runtime->Voice);
			*runtime = AudioSourceRuntime{};
			return;
		}
		registry.emplace<AudioSourceRuntime>(entity);
	}

	void AudioSystem::State::OnSourceDestroyed(entt::registry& registry, entt::entity entity)
	{
		registry.remove<AudioSourceRuntime>(entity);
	}

	void AudioSystem::State::OnRuntimeDestroyed(entt::registry& registry, entt::entity entity)
	{
		ReleaseVoice(registry.get<AudioSourceRuntime>(entity).Voice);
	}

	void AudioSystem::State::ReleaseVoice(AudioVoiceHandle& voice)
	{
		if (Audio->IsVoiceAlive(voice))
		{
			if (const Status stopped = Audio->StopVoice(voice); !stopped)
				ENGINE_CORE_WARN("AudioSystem: stopping a voice failed: {}", stopped.error().ToString());
		}
		voice = AudioVoiceHandle();
	}

	ResolvedAudioClip AudioSystem::State::ResolveClip(AssetHandle handle)
	{
		const AssetHandle placeholder = GetPlaceholderHandle(AssetType::AudioClip);
		if (Assets == nullptr)
		{
			if (handle != placeholder && WarnedClips.insert(handle).second)
				ENGINE_CORE_WARN("AudioSystem: the play session has no asset manager, so audio clip {} plays silence", handle.ToString());
			if (Silence == nullptr)
				Silence = CreateRef<AudioClipData>(CreateSilentAudioClip());
			return ResolvedAudioClip{ .Handle = placeholder, .Version = 1, .Data = Silence };
		}

		// GetOrPlaceholder serves the silent clip for a missing, failed or mistyped clip and records its diagnostic once
		// (§7.2, §10.4); the voice is then registered as the placeholder, which is what plays.
		AssetRef<AudioClipData> data = Assets->GetOrPlaceholder<AudioClipData>(handle);
		AssetHandle effective = handle;
		if (handle != placeholder && static_cast<const Asset*>(data.get()) == Assets->GetPlaceholder(AssetType::AudioClip).get())
			effective = placeholder;
		return ResolvedAudioClip{ .Handle = effective, .Version = std::max<uint64_t>(1, Assets->GetVersion(effective)), .Data = std::move(data) };
	}

	Result<AudioClipHandle> AudioSystem::State::AcquireClip(AssetHandle handle)
	{
		const ResolvedAudioClip clip = ResolveClip(handle);
		const std::pair<AssetHandle, uint64_t> key{ clip.Handle, clip.Version };
		if (const auto found = Clips.find(key); found != Clips.end())
			return found->second;
		ENGINE_TRY_ASSIGN(const AudioClipHandle registered, WithContext(Audio->RegisterClip(MakeAudioClipSource(clip.Handle, clip.Version, clip.Data)), std::format("registering audio clip {}", clip.Handle.ToString())));
		Clips.emplace(key, registered);
		return registered;
	}

	AudioSourceRuntime& AudioSystem::State::GetRuntime(Entity entity)
	{
		return TargetScene->GetRegistry().get_or_emplace<AudioSourceRuntime>(entity.GetHandle());
	}

	AudioVoiceTransform AudioSystem::State::MakeSourceTransform(const glm::mat4& world, const glm::vec3& velocity)
	{
		return AudioVoiceTransform{ .Position = glm::vec3(world[3]), .Direction = Utils::GetWorldForward(world), .Velocity = velocity };
	}

	Status AudioSystem::State::StartSourceVoice(Entity entity, const AudioSourceComponent& source, AudioSourceRuntime& runtime,
		const AudioVoiceTransform& transform)
	{
		ReleaseVoice(runtime.Voice);
		runtime.Clip = source.Clip.GetHandle();
		if (!runtime.Clip.IsValid())
			return {};

		ENGINE_TRY_ASSIGN(const AudioClipHandle clip, AcquireClip(runtime.Clip));
		AudioVoiceDescription description;
		description.Clip = clip;
		description.Settings = Utils::MakeVoiceSettings(source);
		if (Utils::IsFiniteVector(transform.Position) && Utils::IsFiniteVector(transform.Velocity))
			description.Transform = transform;
		description.Priority = Utils::GetSourcePriority(description.Settings.Group);
		description.StartPaused = Paused || runtime.Paused;
		description.Owner = entity.GetUUID().GetValue();
		ENGINE_TRY_ASSIGN(runtime.Voice, Audio->PlayVoice(description));
		runtime.Settings = description.Settings;
		return {};
	}

	Result<AudioSourceComponent*> AudioSystem::State::FindSource(Entity entity) const
	{
		if (!entity.IsValid() || entity.GetScene() != TargetScene)
			return MakeError(ErrorCode::InvalidArgument, "the entity is not in this play session's scene");
		AudioSourceComponent* source = entity.TryGetComponent<AudioSourceComponent>();
		if (source == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "'{}' has no AudioSource", TargetScene->GetEntityPath(entity));
		return source;
	}

	void AudioSystem::State::UpdateListener(double deltaSeconds)
	{
		Listener = SelectAudioListener(*TargetScene);
		AudioListenerPose pose;
		if (Listener.Source == AudioListenerSource::None)
		{
			ListenerEntity = UUID();
			HasListenerPosition = false;
		}
		else
		{
			const glm::mat4 world = Utils::GetWorldMatrix(TargetScene->FindEntityByID(Listener.Entity));
			pose.Position = glm::vec3(world[3]);
			pose.Forward = Utils::GetWorldForward(world);
			pose.Up = Utils::NormalizeOr(glm::vec3(world[1]), glm::vec3(0.0f, 1.0f, 0.0f));
			pose.Velocity = Utils::ComputeVelocity(pose.Position, ListenerPosition, HasListenerPosition && ListenerEntity == Listener.Entity, deltaSeconds);
			ListenerEntity = Listener.Entity;
			ListenerPosition = pose.Position;
			HasListenerPosition = Utils::IsFiniteVector(pose.Position);
		}
		if (const Status set = Audio->SetListener(pose); !set)
		{
			// A degenerate pose (a non-finite or collapsed world matrix written by a runtime system): heard from the origin.
			ENGINE_CORE_WARN("AudioSystem: the listener's pose is unusable, hearing from the origin: {}", set.error().ToString());
			if (const Status origin = Audio->SetListener(AudioListenerPose{}); !origin)
				ENGINE_CORE_WARN("AudioSystem: resetting the listener failed: {}", origin.error().ToString());
		}
	}

	void AudioSystem::State::UpdateSource(Entity entity, AudioSourceComponent& source, double deltaSeconds)
	{
		AudioSourceRuntime& runtime = GetRuntime(entity);
		// A voice that reached the end of a non-looping clip was released by the engine.
		if (!runtime.Voice.IsNull() && !Audio->IsVoiceAlive(runtime.Voice))
		{
			runtime.Voice = AudioVoiceHandle();
			runtime.Paused = false;
		}

		if (!Utils::IsEffectivelyEnabled(entity))
		{
			ReleaseVoice(runtime.Voice);
			runtime.Paused = false;
			runtime.Enabled = false;
			runtime.HasPosition = false;
			return;
		}

		const glm::mat4 world = Utils::GetWorldMatrix(entity);
		const glm::vec3 position(world[3]);
		const bool finitePosition = Utils::IsFiniteVector(position);
		const glm::vec3 velocity = finitePosition ? Utils::ComputeVelocity(position, runtime.Position, runtime.HasPosition, deltaSeconds) : glm::vec3(0.0f);
		runtime.Position = finitePosition ? position : runtime.Position;
		runtime.HasPosition = runtime.HasPosition || finitePosition;
		const AudioVoiceTransform transform = MakeSourceTransform(world, velocity);

		// A new source, or one whose entity was enabled again: PlayOnStart starts it. A changed Clip restarts a playing voice.
		const bool start = (!runtime.Enabled && source.PlayOnStart) || (!runtime.Voice.IsNull() && source.Clip.GetHandle() != runtime.Clip);
		runtime.Enabled = true;
		if (start)
		{
			if (const Status started = StartSourceVoice(entity, source, runtime, transform); !started)
			{
				ENGINE_CORE_WARN("AudioSystem: the AudioSource of '{}' cannot play: {}", TargetScene->GetEntityPath(entity), started.error().ToString());
			}
			return;
		}
		if (runtime.Voice.IsNull())
			return;

		const AudioVoiceSettings settings = Utils::MakeVoiceSettings(source);
		if (settings != runtime.Settings)
		{
			if (const Status applied = Audio->SetVoiceSettings(runtime.Voice, settings); !applied)
				ENGINE_CORE_WARN("AudioSystem: updating the AudioSource of '{}' failed: {}", TargetScene->GetEntityPath(entity), applied.error().ToString());
			else
				runtime.Settings = settings;
		}
		if (settings.Spatial && finitePosition)
		{
			if (const Status moved = Audio->SetVoiceTransform(runtime.Voice, transform); !moved)
				ENGINE_CORE_WARN("AudioSystem: moving the AudioSource of '{}' failed: {}", TargetScene->GetEntityPath(entity), moved.error().ToString());
		}
	}

	AudioListenerSelection SelectAudioListener(const Scene& scene)
	{
		AudioListenerSelection selection;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !Utils::IsEffectivelyEnabled(entity))
				continue;
			const AudioListenerComponent* listener = entity.TryGetComponent<AudioListenerComponent>();
			if (listener == nullptr || !listener->Primary)
				continue;
			if (selection.PrimaryListenerCount == 0)
			{
				selection.Source = AudioListenerSource::Listener;
				selection.Entity = id;
			}
			++selection.PrimaryListenerCount;
		}
		if (selection.PrimaryListenerCount == 0)
		{
			if (const ConstEntity camera = FindPrimaryCamera(scene); camera.IsValid())
			{
				selection.Source = AudioListenerSource::Camera;
				selection.Entity = camera.GetUUID();
			}
		}
		return selection;
	}

	std::vector<AudioSceneIssue> FindAudioSceneIssues(const Scene& scene)
	{
		std::vector<AudioSceneIssue> issues;
		std::vector<ConstEntity> primaries;
		ConstEntity firstSpatial;
		uint32_t spatialCount = 0;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid() || !Utils::IsEffectivelyEnabled(entity))
				continue;
			if (const AudioListenerComponent* listener = entity.TryGetComponent<AudioListenerComponent>(); listener != nullptr && listener->Primary)
				primaries.push_back(entity);
			if (const AudioSourceComponent* source = entity.TryGetComponent<AudioSourceComponent>(); source != nullptr && source->Spatial)
			{
				if (spatialCount == 0)
					firstSpatial = entity;
				++spatialCount;
			}
		}

		for (size_t index = 1; index < primaries.size(); ++index)
		{
			AudioSceneIssue issue;
			issue.Code = std::string(AudioMultiplePrimaryListenersCode);
			issue.Severity = DiagnosticSeverity::Warning;
			issue.Entity = primaries[index].GetUUID();
			issue.Message = std::format("'{}' is a primary AudioListener after '{}'; the scene is heard from the first in scene order",
				scene.GetEntityPath(primaries[index]), scene.GetEntityPath(primaries.front()));
			issue.Hint = std::format("clear Primary on '{}', or fix it to keep only the first listener primary", scene.GetEntityPath(primaries[index]));
			issue.AutoFixable = true;
			issues.push_back(std::move(issue));
		}

		if (primaries.empty() && spatialCount > 0 && !FindPrimaryCamera(scene).IsValid())
		{
			AudioSceneIssue issue;
			issue.Code = std::string(AudioNoListenerCode);
			issue.Severity = DiagnosticSeverity::Warning;
			issue.Entity = firstSpatial.GetUUID();
			issue.Message = std::format("{} spatial AudioSource(s), starting with '{}', but no primary AudioListener and no primary camera: "
										"they are heard from the origin",
				spatialCount, scene.GetEntityPath(firstSpatial));
			issue.Hint = "add an entity with an AudioListener (entity.create {components: {AudioListener: {}}}) or a primary camera";
			issue.AutoFixable = false;
			issues.push_back(std::move(issue));
		}
		return issues;
	}

	AudioClipSource MakeAudioClipSource(AssetHandle handle, uint64_t version, const AssetRef<AudioClipData>& clip)
	{
		ENGINE_CORE_ASSERT(clip != nullptr, "MakeAudioClipSource needs a loaded clip");
		ENGINE_CORE_ASSERT(handle.IsValid(), "MakeAudioClipSource needs a valid handle");
		AudioClipSource source;
		source.Name = handle.ToString();
		source.Version = version;
		source.Format = clip->Encoding == AudioClipEncoding::Pcm16 ? AudioClipFormat::Pcm16 : AudioClipFormat::Encoded;
		// The aliasing constructor: the engine holds the clip's bytes, and through them the whole AudioClipData, never a copy.
		source.Bytes = Ref<const Buffer>(clip, &clip->Bytes);
		if (source.Format == AudioClipFormat::Pcm16)
		{
			source.SampleRate = clip->SampleRate;
			source.ChannelCount = clip->ChannelCount;
		}
		source.Stream = clip->Stream;
		return source;
	}

	AudioSystem::AudioSystem(Scene& scene, const AudioSystemSpecification& specification)
		: m_State(CreateScope<State>())
	{
		ENGINE_CORE_ASSERT(specification.Audio != nullptr, "AudioSystem needs the context's audio engine");
		State& state = *m_State;
		state.TargetScene = &scene;
		state.Audio = specification.Audio;
		state.Assets = specification.Assets;
		entt::registry& registry = scene.GetRegistry();
		// Connecting the runtime's destroy signal also creates its pool, so the AudioSource handlers never create a pool
		// while EnTT iterates the pools of an entity it destroys.
		registry.on_destroy<AudioSourceRuntime>().connect<&State::OnRuntimeDestroyed>(state);
		registry.on_construct<AudioSourceComponent>().connect<&State::OnSourceConstructed>(state);
		registry.on_destroy<AudioSourceComponent>().connect<&State::OnSourceDestroyed>(state);
	}

	AudioSystem::~AudioSystem()
	{
		State& state = *m_State;
		entt::registry& registry = state.TargetScene->GetRegistry();
		registry.on_construct<AudioSourceComponent>().disconnect(&state);
		registry.on_destroy<AudioSourceComponent>().disconnect(&state);
		registry.on_destroy<AudioSourceRuntime>().disconnect(&state);

		// Stop (§10.2): every voice of the session is released, then its clips, then the session's mix is undone.
		for (auto [entity, runtime] : registry.view<AudioSourceRuntime>().each())
			state.ReleaseVoice(runtime.Voice);
		registry.clear<AudioSourceRuntime>();
		for (AudioVoiceHandle& voice : state.OneShots)
			state.ReleaseVoice(voice);
		for (const auto& [key, clip] : state.Clips)
		{
			if (const Status unregistered = state.Audio->UnregisterClip(clip); !unregistered)
				ENGINE_CORE_WARN("AudioSystem: unregistering audio clip {} failed: {}", key.first.ToString(), unregistered.error().ToString());
		}
		if (state.Started)
		{
			for (const AudioGroup group : AllAudioGroups)
			{
				if (const Status restored = state.Audio->SetGroupVolume(group, state.RecordedGroupVolumes[std::to_underlying(group)]); !restored)
					ENGINE_CORE_WARN("AudioSystem: restoring the {} volume failed: {}", AudioGroupToString(group), restored.error().ToString());
			}
		}
	}

	void AudioSystem::Start()
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.Started, "AudioSystem::Start runs once per session");
		// The session's mix (see "Mixer"): the engine's group volumes are recorded and every session starts from 1.
		for (const AudioGroup group : AllAudioGroups)
		{
			state.RecordedGroupVolumes[std::to_underlying(group)] = state.Audio->GetGroupVolume(group);
			if (const Status reset = state.Audio->SetGroupVolume(group, 1.0f); !reset)
				ENGINE_CORE_WARN("AudioSystem: resetting the {} volume failed: {}", AudioGroupToString(group), reset.error().ToString());
		}
		state.Started = true;
		state.UpdateListener(0.0);

		// §5.6 "PlayOnStart audio starts", in canonical order.
		state.TargetScene->ForEachCanonical([&state](Entity entity)
		{
			const AudioSourceComponent* source = entity.TryGetComponent<AudioSourceComponent>();
			if (source == nullptr)
				return;
			AudioSourceRuntime& runtime = state.GetRuntime(entity);
			runtime.Enabled = Utils::IsEffectivelyEnabled(entity);
			if (!runtime.Enabled)
				return;
			const glm::mat4 world = Utils::GetWorldMatrix(entity);
			runtime.Position = glm::vec3(world[3]);
			runtime.HasPosition = Utils::IsFiniteVector(runtime.Position);
			if (!source->PlayOnStart)
				return;
			if (const Status started = state.StartSourceVoice(entity, *source, runtime, State::MakeSourceTransform(world, glm::vec3(0.0f))); !started)
			{
				ENGINE_CORE_WARN("AudioSystem: the AudioSource of '{}' cannot play: {}", state.TargetScene->GetEntityPath(entity),
					started.error().ToString());
			}
		});
	}

	void AudioSystem::Update(double deltaSeconds)
	{
		ENGINE_CORE_ASSERT(std::isfinite(deltaSeconds) && deltaSeconds >= 0.0, "AudioSystem::Update needs a finite delta >= 0, got {}", deltaSeconds);
		State& state = *m_State;
		state.UpdateListener(deltaSeconds);
		std::erase_if(state.OneShots, [&state](AudioVoiceHandle voice)
		{
			return !state.Audio->IsVoiceAlive(voice);
		});
		state.TargetScene->ForEachCanonical([&state, deltaSeconds](Entity entity)
		{
			if (AudioSourceComponent* source = entity.TryGetComponent<AudioSourceComponent>())
				state.UpdateSource(entity, *source, deltaSeconds);
		});
	}

	void AudioSystem::SetPaused(bool paused)
	{
		State& state = *m_State;
		if (state.Paused == paused)
			return;
		state.Paused = paused;
		const auto apply = [&state](AudioVoiceHandle voice, bool voicePaused)
		{
			if (!state.Audio->IsVoiceAlive(voice))
				return;
			if (const Status set = state.Audio->SetVoicePaused(voice, voicePaused); !set)
				ENGINE_CORE_WARN("AudioSystem: pausing a voice failed: {}", set.error().ToString());
		};
		for (auto [entity, runtime] : state.TargetScene->GetRegistry().view<AudioSourceRuntime>().each())
			apply(runtime.Voice, paused || runtime.Paused);
		for (const AudioVoiceHandle voice : state.OneShots)
			apply(voice, paused);
	}

	bool AudioSystem::IsPaused() const
	{
		return m_State->Paused;
	}

	Status AudioSystem::Play(Entity entity)
	{
		State& state = *m_State;
		ENGINE_TRY_ASSIGN(const AudioSourceComponent* source, state.FindSource(entity));
		if (!Utils::IsEffectivelyEnabled(entity))
			return MakeError(ErrorCode::InvalidState, "'{}' is disabled; enable it before playing its AudioSource", state.TargetScene->GetEntityPath(entity));
		AudioSourceRuntime& runtime = state.GetRuntime(entity);
		runtime.Paused = false;
		runtime.Enabled = true;
		const glm::mat4 world = Utils::GetWorldMatrix(entity);
		if (!runtime.HasPosition)
		{
			runtime.Position = glm::vec3(world[3]);
			runtime.HasPosition = Utils::IsFiniteVector(runtime.Position);
		}
		return state.StartSourceVoice(entity, *source, runtime, State::MakeSourceTransform(world, glm::vec3(0.0f)));
	}

	Status AudioSystem::Stop(Entity entity)
	{
		State& state = *m_State;
		ENGINE_TRY(state.FindSource(entity));
		AudioSourceRuntime& runtime = state.GetRuntime(entity);
		state.ReleaseVoice(runtime.Voice);
		runtime.Paused = false;
		return {};
	}

	Status AudioSystem::Pause(Entity entity)
	{
		State& state = *m_State;
		ENGINE_TRY(state.FindSource(entity));
		AudioSourceRuntime& runtime = state.GetRuntime(entity);
		if (runtime.Paused || !state.Audio->IsVoiceAlive(runtime.Voice))
			return {};
		ENGINE_TRY(state.Audio->SetVoicePaused(runtime.Voice, true));
		runtime.Paused = true;
		return {};
	}

	Status AudioSystem::Resume(Entity entity)
	{
		State& state = *m_State;
		ENGINE_TRY(state.FindSource(entity));
		if (!Utils::IsEffectivelyEnabled(entity))
			return MakeError(ErrorCode::InvalidState, "'{}' is disabled; enable it before resuming its AudioSource", state.TargetScene->GetEntityPath(entity));
		AudioSourceRuntime& runtime = state.GetRuntime(entity);
		if (!runtime.Paused || !state.Audio->IsVoiceAlive(runtime.Voice))
			return {};
		ENGINE_TRY(state.Audio->SetVoicePaused(runtime.Voice, state.Paused));
		runtime.Paused = false;
		return {};
	}

	bool AudioSystem::IsPlaying(ConstEntity entity) const
	{
		const State& state = *m_State;
		if (!entity.IsValid() || entity.GetScene() != state.TargetScene)
			return false;
		const AudioSourceRuntime* runtime = std::as_const(*state.TargetScene).GetRegistry().try_get<AudioSourceRuntime>(entity.GetHandle());
		return runtime != nullptr && !runtime->Paused && state.Audio->IsVoiceAlive(runtime->Voice);
	}

	Result<AudioVoiceHandle> AudioSystem::PlayOneShot(AssetHandle clip, const std::optional<glm::vec3>& position, float volume, AudioGroup group)
	{
		State& state = *m_State;
		if (!clip.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "PlayOneShot needs an audio clip");
		if (!std::isfinite(volume) || volume < 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "the one-shot's volume must be finite and at least 0, got {}", volume);
		if (position.has_value() && !Utils::IsFiniteVector(*position))
			return MakeError(ErrorCode::InvalidArgument, "the one-shot's position must be finite");
		if (!Utils::IsValidAudioGroup(group))
			return MakeError(ErrorCode::InvalidArgument, "unknown audio group {}", std::to_underlying(group));

		ENGINE_TRY_ASSIGN(const AudioClipHandle registered, state.AcquireClip(clip));
		AudioVoiceDescription description;
		description.Clip = registered;
		description.Settings.Group = group;
		description.Settings.Volume = volume;
		description.Settings.Spatial = position.has_value();
		if (position.has_value())
			description.Transform.Position = *position;
		description.Priority = AudioSystemEffectPriority;
		description.StartPaused = state.Paused;
		ENGINE_TRY_ASSIGN(const AudioVoiceHandle voice, state.Audio->PlayVoice(description));
		state.OneShots.push_back(voice);
		return voice;
	}

	Status AudioSystem::SetGroupVolume(AudioGroup group, float volume)
	{
		if (!Utils::IsValidAudioGroup(group))
			return MakeError(ErrorCode::InvalidArgument, "unknown audio group {}", std::to_underlying(group));
		if (!std::isfinite(volume) || volume < 0.0f)
			return MakeError(ErrorCode::InvalidArgument, "the {} volume must be finite and at least 0, got {}", AudioGroupToString(group), volume);
		return m_State->Audio->SetGroupVolume(group, volume);
	}

	float AudioSystem::GetGroupVolume(AudioGroup group) const
	{
		return m_State->Audio->GetGroupVolume(group);
	}

	const AudioListenerSelection& AudioSystem::GetListener() const
	{
		return m_State->Listener;
	}

	std::string_view AudioListenerSourceToString(AudioListenerSource source)
	{
		switch (source)
		{
			case AudioListenerSource::None:     return "None";
			case AudioListenerSource::Listener: return "Listener";
			case AudioListenerSource::Camera:   return "Camera";
		}

		ENGINE_CORE_ASSERT(false, "Unknown AudioListenerSource {}", std::to_underlying(source));
		return "Unknown";
	}

}
