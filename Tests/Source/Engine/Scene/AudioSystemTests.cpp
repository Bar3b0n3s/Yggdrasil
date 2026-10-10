#include "TestsPCH.h"

#include "Engine/Scene/AudioSystem.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/AudioListenerComponent.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AudioTestData.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Components to voices (Architecture §10.2, §10.4; Docs/Decisions/0015-m12-decisions.md decision 12). The listener rule,
// the validation checks and MakeAudioClipSource need no engine. The tests that play run a device-less AudioEngine with
// deterministic decoding over clips served from memory.

namespace Engine {

	namespace {

		// An asset manager over in-memory clips; procedural built-ins (the silent clip) come from the base.
		class ClipAssetManager final : public AssetManager
		{
		public:
			ClipAssetManager()
				: m_Jobs(0, m_MainThreadQueue)
			{
			}

			void Publish(AssetHandle handle, AssetRef<Asset> asset)
			{
				m_Assets.insert_or_assign(handle, std::move(asset));
				BumpVersion(handle);
			}

			Result<AssetRef<Asset>> Load(AssetHandle handle) override
			{
				if (IsBuiltinAssetHandle(handle))
					return GetProceduralBuiltin(handle);
				const auto found = m_Assets.find(handle);
				if (found == m_Assets.end())
					return MakeError(ErrorCode::NotFound, "no asset {}", handle);
				return found->second;
			}

			JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override
			{
				return m_Jobs.Submit([result = Load(handle)]()
				{
					return result;
				});
			}

			AssetState GetState(AssetHandle handle) const override { return m_Assets.contains(handle) ? AssetState::Loaded : AssetState::Unloaded; }
			const AssetMetadata* GetMetadata(AssetHandle /*handle*/) const override { return nullptr; }

			AssetType GetAssetType(AssetHandle handle) const override
			{
				const auto found = m_Assets.find(handle);
				return found != m_Assets.end() ? found->second->GetAssetType() : AssetType::None;
			}

			std::optional<AssetHandle> Resolve(std::string_view /*reference*/) const override { return std::nullopt; }
			std::string GetReferencePath(AssetHandle handle) const override { return std::format("Assets/Audio/{}.wav", handle); }
			void WaitIdle() override {}
		private:
			MainThreadQueue m_MainThreadQueue;
			JobSystem m_Jobs;
			std::map<AssetHandle, AssetRef<Asset>> m_Assets;
		};

		// One test's runtime scene, audio engine and clips. Not copyable or movable.
		class AudioSceneFixture
		{
		public:
			AudioSceneFixture()
				: m_Scene(1, true)
			{
				Result<Scope<AudioEngine>> engine = AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, m_Vfs);
				REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
				m_Engine = std::move(*engine);
			}

			AudioSceneFixture(const AudioSceneFixture&) = delete;
			AudioSceneFixture& operator=(const AudioSceneFixture&) = delete;

			[[nodiscard]] Scene& GetScene() { return m_Scene.GetScene(); }
			[[nodiscard]] AudioEngine& GetEngine() { return *m_Engine; }
			[[nodiscard]] ClipAssetManager& GetAssets() { return m_Assets; }
			[[nodiscard]] AudioSystemSpecification GetSpecification() { return { .Audio = m_Engine.get(), .Assets = &m_Assets }; }

			// Publishes `seconds` of a 440 Hz, 48 kHz mono tone as the clip `handle`.
			AssetHandle AddToneClip(uint64_t handle, double seconds = 1.0)
			{
				const Test::TestToneSpecification tone{ .SampleRate = 48000,
					.ChannelCount = 1,
					.FrameCount = static_cast<uint64_t>(seconds * 48000.0),
					.Frequency = 440.0,
					.Amplitude = 0.5 };
				const Ref<AudioClipData> clip = CreateRef<AudioClipData>();
				clip->Encoding = AudioClipEncoding::Pcm16;
				clip->SampleRate = tone.SampleRate;
				clip->ChannelCount = tone.ChannelCount;
				clip->FrameCount = tone.FrameCount;
				clip->Bytes = Test::MakeTonePcm16Bytes(tone);
				m_Assets.Publish(AssetHandle(handle), clip);
				return AssetHandle(handle);
			}

			// An entity at `position` with an AudioSource of `clip`.
			Entity AddSource(std::string_view name, AssetHandle clip, const glm::vec3& position, bool playOnStart = true, bool spatial = true)
			{
				Entity entity = GetScene().CreateEntity(name);
				entity.GetComponent<TransformComponent>().Translation = position;
				AudioSourceComponent source;
				source.Clip.SetHandle(clip);
				source.PlayOnStart = playOnStart;
				source.Spatial = spatial;
				source.Loop = true;
				source.Attenuation = Attenuation::None;
				entity.AddComponent<AudioSourceComponent>(source);
				return entity;
			}

			// A primary AudioListener at the origin, looking down -Z.
			Entity AddListener(std::string_view name = "Listener", bool primary = true)
			{
				Entity entity = GetScene().CreateEntity(name);
				entity.AddComponent<AudioListenerComponent>(AudioListenerComponent{ .Primary = primary });
				return entity;
			}

			// TransformSystem::Update, then AudioSystem::Update with 1/60 s, then one tick of frames pulled.
			std::vector<float> Frame(AudioSystem& system, uint64_t frames = 800)
			{
				TransformSystem::Update(GetScene());
				system.Update(1.0 / 60.0);
				std::vector<float> samples(static_cast<size_t>(frames) * AudioChannelCount);
				const Status read = m_Engine->ReadFrames(samples);
				REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
				return samples;
			}
		private:
			Test::SceneTestFixture m_Scene;
			VirtualFileSystem m_Vfs;
			ClipAssetManager m_Assets;
			Scope<AudioEngine> m_Engine;
		};

		// Every issue's code, in report order.
		std::vector<std::string> GetCodes(const std::vector<AudioSceneIssue>& issues)
		{
			std::vector<std::string> codes;
			for (const AudioSceneIssue& issue : issues)
				codes.push_back(issue.Code);
			return codes;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("AudioSystem: listener sources name themselves")
		{
			CHECK(AudioListenerSourceToString(AudioListenerSource::None) == "None");
			CHECK(AudioListenerSourceToString(AudioListenerSource::Listener) == "Listener");
			CHECK(AudioListenerSourceToString(AudioListenerSource::Camera) == "Camera");
			CHECK(AudioSystemEffectPriority < AudioSystemMusicPriority);
			CHECK(AudioNoListenerCode == "AUDIO_NO_LISTENER");
			CHECK(AudioMultiplePrimaryListenersCode == "AUDIO_MULTIPLE_PRIMARY_LISTENERS");
		}

		TEST_CASE("AudioSystem: PlayOnStart sources start with the session and others wait for Play")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity autoplay = fixture.AddSource("Autoplay", clip, glm::vec3(0.0f, 0.0f, -2.0f), true);
			const Entity manual = fixture.AddSource("Manual", clip, glm::vec3(0.0f, 0.0f, -2.0f), false);
			TransformSystem::Update(fixture.GetScene());
			{
				AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
				system.Start();
				CHECK(system.IsPlaying(autoplay));
				CHECK_FALSE(system.IsPlaying(manual));
				CHECK(fixture.GetEngine().GetStats().LiveVoices == 1);
				REQUIRE(system.Play(manual).has_value());
				CHECK(system.IsPlaying(manual));
				CHECK(fixture.GetEngine().GetStats().LiveVoices == 2);
				const std::vector<float> frames = fixture.Frame(system);
				CHECK(MeasureAudioLevels(frames).Peak > 0.0f);
				REQUIRE(system.Stop(autoplay).has_value());
				CHECK_FALSE(system.IsPlaying(autoplay));
				// Stop of a source that does not play changes nothing.
				CHECK(system.Stop(autoplay).has_value());
			}
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);
		}

		TEST_CASE("AudioSystem: a spatial source to the listener's right is louder on the right")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			fixture.AddSource("Right", clip, glm::vec3(10.0f, 0.0f, 0.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			fixture.Frame(system);
			const AudioLevels levels = MeasureAudioLevels(fixture.Frame(system, 4800));
			CHECK(levels.RmsRight > levels.RmsLeft);
			CHECK(system.GetListener().Source == AudioListenerSource::Listener);
		}

		TEST_CASE("AudioSystem: captured PCM of a fixed scene is bit-identical across runs")
		{
			const auto captureOnce = []()
			{
				AudioSceneFixture fixture;
				const AssetHandle music = fixture.AddToneClip(0xa001, 3.0);
				const AssetHandle effect = fixture.AddToneClip(0xa002, 0.25);
				fixture.AddListener();
				fixture.AddSource("Music", music, glm::vec3(0.0f), true, false);
				const Entity moving = fixture.AddSource("Moving", effect, glm::vec3(-5.0f, 0.0f, -3.0f));
				TransformSystem::Update(fixture.GetScene());
				AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
				system.Start();
				fixture.GetEngine().StartCapture();
				for (int frame = 0; frame < 120; ++frame)
				{
					moving.GetComponent<TransformComponent>().Translation.x += 0.1f;
					fixture.Frame(system);
					if (frame == 60)
						CHECK(system.PlayOneShot(effect, glm::vec3(2.0f, 0.0f, 0.0f), 0.5f).has_value());
				}
				return fixture.GetEngine().StopCapture();
			};
			const AudioCapture first = captureOnce();
			const AudioCapture second = captureOnce();
			CHECK(first.GetFrameCount() == 120 * 800);
			CHECK(MeasureAudioLevels(first.Samples).Peak > 0.0f);
			CHECK(first.Samples == second.Samples);
		}

		TEST_CASE("AudioSystem: a missing clip yields a diagnostic and silence")
		{
			AudioSceneFixture fixture;
			fixture.AddListener();
			const AssetHandle missing(0xdead);
			const Entity source = fixture.AddSource("Missing", missing, glm::vec3(0.0f, 0.0f, -1.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			// GetOrPlaceholder logs the missing clip once, at Error (§7.2).
			const Test::ExpectLog logged(LogLevel::Error, missing.ToString());
			system.Start();
			// The silent clip plays in its place (§7.2), so the source counts as playing.
			CHECK(system.IsPlaying(source));
			const std::vector<float> frames = fixture.Frame(system, 4800);
			CHECK(MeasureAudioLevels(frames).Peak == 0.0f);
			const std::span<const AssetDiagnostic> diagnostics = fixture.GetAssets().GetDiagnostics();
			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics.front().Code == AssetMissingCode);
			CHECK(diagnostics.front().Asset == missing);
			const std::vector<AudioVoiceInfo> voices = fixture.GetEngine().GetVoices();
			REQUIRE(voices.size() == 1);
			CHECK(voices.front().ClipName == BuiltinAssetHandles::SilentClip.ToString());
		}

		TEST_CASE("AudioSystem: the listener is the first primary AudioListener, else the primary camera")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			CHECK(SelectAudioListener(scene) == AudioListenerSelection{});

			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			AudioListenerSelection selection = SelectAudioListener(scene);
			CHECK(selection.Source == AudioListenerSource::Camera);
			CHECK(selection.Entity == camera.GetUUID());

			Entity secondary = scene.CreateEntity("Secondary");
			secondary.AddComponent<AudioListenerComponent>(AudioListenerComponent{ .Primary = false });
			CHECK(SelectAudioListener(scene).Source == AudioListenerSource::Camera);

			Entity first = scene.CreateEntity("First");
			first.AddComponent<AudioListenerComponent>();
			Entity second = scene.CreateEntity("Second");
			second.AddComponent<AudioListenerComponent>();
			selection = SelectAudioListener(scene);
			CHECK(selection.Source == AudioListenerSource::Listener);
			CHECK(selection.Entity == first.GetUUID());
			CHECK(selection.PrimaryListenerCount == 2);

			// A disabled listener does not count.
			first.SetActive(false);
			TransformSystem::Update(scene);
			selection = SelectAudioListener(scene);
			CHECK(selection.Entity == second.GetUUID());
			CHECK(selection.PrimaryListenerCount == 1);
		}

		TEST_CASE("AudioSystem: several primary listeners raise AUDIO_MULTIPLE_PRIMARY_LISTENERS")
		{
			Test::SceneTestFixture fixture(1, false);
			Scene& scene = fixture.GetScene();
			Entity first = scene.CreateEntity("First");
			first.AddComponent<AudioListenerComponent>();
			CHECK(FindAudioSceneIssues(scene).empty());
			Entity second = scene.CreateEntity("Second");
			second.AddComponent<AudioListenerComponent>();
			Entity third = scene.CreateEntity("Third");
			third.AddComponent<AudioListenerComponent>();
			const std::vector<AudioSceneIssue> issues = FindAudioSceneIssues(scene);
			CHECK(GetCodes(issues) == std::vector<std::string>{ std::string(AudioMultiplePrimaryListenersCode), std::string(AudioMultiplePrimaryListenersCode) });
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Entity == second.GetUUID());
			CHECK(issues[1].Entity == third.GetUUID());
			CHECK(issues[0].Severity == DiagnosticSeverity::Warning);
			CHECK(issues[0].AutoFixable);
		}

		TEST_CASE("AudioSystem: group volumes belong to the session and destroying the system restores the engine's")
		{
			AudioSceneFixture fixture;
			AudioEngine& engine = fixture.GetEngine();
			// What the editor (or an earlier session) left on the engine.
			REQUIRE(engine.SetGroupVolume(AudioGroup::Music, 0.25f).has_value());
			REQUIRE(engine.SetGroupVolume(AudioGroup::Ui, 0.5f).has_value());
			REQUIRE(engine.SetMasterVolume(0.75f).has_value());
			{
				AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
				system.Start();
				// Every session starts from the same mix; the Master volume stays the host's.
				for (const AudioGroup group : { AudioGroup::Music, AudioGroup::Sfx, AudioGroup::Ui })
				{
					CAPTURE(std::string(AudioGroupToString(group)));
					CHECK(system.GetGroupVolume(group) == 1.0f);
					CHECK(engine.GetGroupVolume(group) == 1.0f);
				}
				CHECK(engine.GetMasterVolume() == 0.75f);
				REQUIRE(system.SetGroupVolume(AudioGroup::Sfx, 0.5f).has_value());
				CHECK(system.GetGroupVolume(AudioGroup::Sfx) == 0.5f);
				CHECK(engine.GetGroupVolume(AudioGroup::Sfx) == 0.5f);
				const Status refused = system.SetGroupVolume(AudioGroup::Music, -1.0f);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			// Stop leaves the editor's mixer as it was.
			CHECK(engine.GetGroupVolume(AudioGroup::Music) == 0.25f);
			CHECK(engine.GetGroupVolume(AudioGroup::Sfx) == 1.0f);
			CHECK(engine.GetGroupVolume(AudioGroup::Ui) == 0.5f);
			CHECK(engine.GetMasterVolume() == 0.75f);
		}

		TEST_CASE("AudioSystem: mixer initialization is idempotent and restores without starting voices")
		{
			AudioSceneFixture fixture;
			AudioEngine& engine = fixture.GetEngine();
			REQUIRE(engine.SetGroupVolume(AudioGroup::Music, 0.75f));
			{
				AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
				system.InitializeMix();
				CHECK(system.GetGroupVolume(AudioGroup::Music) == 1.0f);
				REQUIRE(system.SetGroupVolume(AudioGroup::Music, 0.25f));
				system.InitializeMix();
				CHECK(system.GetGroupVolume(AudioGroup::Music) == 0.25f);
			}
			CHECK(engine.GetGroupVolume(AudioGroup::Music) == 0.75f);
		}

		TEST_CASE("AudioSystem: spatial sources without a listener or a camera raise AUDIO_NO_LISTENER")
		{
			Test::SceneTestFixture fixture(1, false);
			Scene& scene = fixture.GetScene();
			Entity flat = scene.CreateEntity("Flat");
			AudioSourceComponent nonSpatial;
			nonSpatial.Spatial = false;
			flat.AddComponent<AudioSourceComponent>(nonSpatial);
			CHECK(FindAudioSceneIssues(scene).empty());

			Entity spatial = scene.CreateEntity("Spatial");
			spatial.AddComponent<AudioSourceComponent>();
			const std::vector<AudioSceneIssue> issues = FindAudioSceneIssues(scene);
			REQUIRE(GetCodes(issues) == std::vector<std::string>{ std::string(AudioNoListenerCode) });
			CHECK(issues[0].Entity == spatial.GetUUID());
			CHECK_FALSE(issues[0].AutoFixable);

			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			CHECK(FindAudioSceneIssues(scene).empty());
		}

		TEST_CASE("AudioSystem: component fields reach the voice at every update")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity source = fixture.AddSource("Source", clip, glm::vec3(0.0f, 0.0f, -4.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			AudioSourceComponent& component = source.GetComponent<AudioSourceComponent>();
			component.Volume = 0.25f;
			component.Pitch = 1.5f;
			component.Group = AudioGroup::Music;
			component.MinDistance = 2.0f;
			component.MaxDistance = 8.0f;
			component.Attenuation = Attenuation::Linear;
			component.DopplerFactor = 0.0f;
			fixture.Frame(system);
			const std::vector<AudioVoiceInfo> voices = fixture.GetEngine().GetVoices();
			REQUIRE(voices.size() == 1);
			const AudioVoiceInfo& voice = voices.front();
			CHECK(voice.Owner == source.GetUUID().GetValue());
			// The stealing priority is decided by the group when the voice starts (it began in Sfx).
			CHECK(voice.Priority == AudioSystemEffectPriority);
			CHECK(voice.Settings.Volume == 0.25f);
			CHECK(voice.Settings.Pitch == 1.5f);
			CHECK(voice.Settings.Group == AudioGroup::Music);
			const AudioSpatialization expected{
				.Model = Attenuation::Linear, .MinDistance = 2.0f, .MaxDistance = 8.0f, .Rolloff = 1.0f, .DopplerFactor = 0.0f
			};
			CHECK(voice.Settings.Spatialization == expected);
			CHECK(voice.Transform.Position == glm::vec3(0.0f, 0.0f, -4.0f));
			// A changed clip restarts the playing voice with the new clip, now with the Music group's priority.
			const AssetHandle other = fixture.AddToneClip(0xa002);
			component.Clip.SetHandle(other);
			fixture.Frame(system);
			const std::vector<AudioVoiceInfo> restarted = fixture.GetEngine().GetVoices();
			REQUIRE(restarted.size() == 1);
			CHECK(restarted.front().ClipName == other.ToString());
			CHECK(restarted.front().Priority == AudioSystemMusicPriority);
			// From the clip's start: one frame of playback, as the first voice had after its first frame.
			CHECK(restarted.front().CursorFrames <= voice.CursorFrames);
			// A null clip plays nothing ("null plays nothing", the AudioSource registration).
			component.Clip.SetHandle(AssetHandle());
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetVoices().empty());
			CHECK_FALSE(system.IsPlaying(source));
		}

		TEST_CASE("AudioSystem: destroying or disabling a source's entity releases its voice")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity destroyed = fixture.AddSource("Destroyed", clip, glm::vec3(0.0f));
			const Entity disabled = fixture.AddSource("Disabled", clip, glm::vec3(0.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			REQUIRE(fixture.GetEngine().GetStats().LiveVoices == 2);
			fixture.GetScene().DestroyEntity(destroyed);
			fixture.GetScene().FlushPendingDestroys();
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 1);
			disabled.SetActive(false);
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);
			CHECK_FALSE(system.IsPlaying(disabled));
			// Enabled again, a PlayOnStart source starts again.
			disabled.SetActive(true);
			fixture.Frame(system);
			CHECK(system.IsPlaying(disabled));
		}

		TEST_CASE("AudioSystem: pause holds every voice and resume continues it")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity source = fixture.AddSource("Source", clip, glm::vec3(0.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			REQUIRE(system.PlayOneShot(clip, std::nullopt).has_value());
			fixture.Frame(system);
			system.SetPaused(true);
			CHECK(system.IsPaused());
			const std::vector<AudioVoiceInfo> before = fixture.GetEngine().GetVoices();
			REQUIRE(before.size() == 2);
			const std::vector<float> silence = fixture.Frame(system);
			CHECK(MeasureAudioLevels(silence).Peak == 0.0f);
			const std::vector<AudioVoiceInfo> paused = fixture.GetEngine().GetVoices();
			for (size_t index = 0; index < before.size(); ++index)
			{
				CHECK(paused[index].Paused);
				CHECK(paused[index].CursorFrames == before[index].CursorFrames);
			}
			// A voice started while paused starts paused.
			REQUIRE(system.PlayOneShot(clip, std::nullopt).has_value());
			CHECK(fixture.GetEngine().GetVoices().back().Paused);
			system.SetPaused(false);
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetVoices().front().CursorFrames == before.front().CursorFrames + 800);
			// The script-level Pause survives the session's resume.
			REQUIRE(system.Pause(source).has_value());
			system.SetPaused(true);
			system.SetPaused(false);
			CHECK_FALSE(system.IsPlaying(source));
			REQUIRE(system.Resume(source).has_value());
			CHECK(system.IsPlaying(source));
		}

		TEST_CASE("AudioSystem: PlayOneShot is non-spatial without a position and plays in the Sfx group by default")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001, 0.1);
			fixture.AddListener();
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			const Result<AudioVoiceHandle> flat = system.PlayOneShot(clip, std::nullopt);
			REQUIRE_MESSAGE(flat.has_value(), flat.error().ToString());
			const Result<AudioVoiceInfo> flatInfo = fixture.GetEngine().GetVoiceInfo(*flat);
			REQUIRE(flatInfo.has_value());
			CHECK_FALSE(flatInfo->Settings.Spatial);
			CHECK(flatInfo->Settings.Group == AudioGroup::Sfx);
			CHECK(flatInfo->Settings.Volume == 1.0f);
			CHECK_FALSE(flatInfo->Settings.Loop);
			CHECK(flatInfo->Owner == 0);
			CHECK(flatInfo->Priority == AudioSystemEffectPriority);

			const Result<AudioVoiceHandle> placed = system.PlayOneShot(clip, glm::vec3(1.0f, 2.0f, 3.0f), 0.5f, AudioGroup::Ui);
			REQUIRE(placed.has_value());
			const Result<AudioVoiceInfo> placedInfo = fixture.GetEngine().GetVoiceInfo(*placed);
			REQUIRE(placedInfo.has_value());
			CHECK(placedInfo->Settings.Spatial);
			CHECK(placedInfo->Settings.Group == AudioGroup::Ui);
			CHECK(placedInfo->Settings.Volume == 0.5f);
			CHECK(placedInfo->Transform.Position == glm::vec3(1.0f, 2.0f, 3.0f));

			CHECK_FALSE(system.PlayOneShot(AssetHandle(), std::nullopt).has_value());
			CHECK_FALSE(system.PlayOneShot(clip, std::nullopt, -1.0f).has_value());
			// One-shots end on their own: 0.1 s.
			fixture.Frame(system, 4800);
			fixture.Frame(system);
			CHECK_FALSE(fixture.GetEngine().IsVoiceAlive(*flat));
		}

		TEST_CASE("AudioSystem: sources created during play with PlayOnStart start at the next update")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);
			const Entity spawned = fixture.AddSource("Spawned", clip, glm::vec3(0.0f));
			CHECK_FALSE(system.IsPlaying(spawned));
			fixture.Frame(system);
			CHECK(system.IsPlaying(spawned));
		}

		TEST_CASE("AudioSystem: a source's velocity comes from its world-position delta")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity source = fixture.AddSource("Moving", clip, glm::vec3(0.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetVoices().front().Transform.Velocity == glm::vec3(0.0f));
			source.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
			fixture.Frame(system);
			// One metre in 1/60 s.
			CHECK(fixture.GetEngine().GetVoices().front().Transform.Velocity.x == doctest::Approx(60.0f));
			// A zero delta reports no velocity.
			system.Update(0.0);
			CHECK(fixture.GetEngine().GetVoices().front().Transform.Velocity == glm::vec3(0.0f));
		}

		TEST_CASE("AudioSystem: a source or listener that jumps faster than sound has no velocity")
		{
			// A teleport, a respawn or a camera cut is a jump, not motion: its velocity would Doppler-shift every voice for a
			// frame. A position that moves at or above the speed of sound (343.3 m/s) in one frame reports no velocity.
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			const Entity listener = fixture.AddListener();
			const Entity source = fixture.AddSource("Jumping", clip, glm::vec3(0.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			fixture.Frame(system);
			// 5 m in 1/60 s is 300 m/s: motion.
			source.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetVoices().front().Transform.Velocity.x == doctest::Approx(300.0f));
			// 10 m more in 1/60 s is 600 m/s: a jump.
			source.GetComponent<TransformComponent>().Translation = glm::vec3(15.0f, 0.0f, 0.0f);
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetVoices().front().Transform.Velocity == glm::vec3(0.0f));
			// The listener is held to the same rule: 100 m in one frame.
			listener.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 100.0f);
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetListener().Position == glm::vec3(0.0f, 0.0f, 100.0f));
			CHECK(fixture.GetEngine().GetListener().Velocity == glm::vec3(0.0f));
			// The frame after a jump measures from the new position.
			listener.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 101.0f);
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetListener().Velocity.z == doctest::Approx(60.0f));
		}

		TEST_CASE("AudioSystem: a spatial source is heard at its rendered pose and that pose's velocity")
		{
			// Physics and FixedUpdate scripts move entities only inside fixed steps, of which a frame runs 0 to
			// MaxStepsPerFrame: the world pose of a moving body stands still on some frames and jumps on others. The rendered
			// pose (§5.2) moves with the frame's time, so the voice is heard where the body is drawn and at its speed
			// (Docs/Decisions/0016-m8-m11-m12-integration.md decision 8).
			AudioSceneFixture fixture;
			Scene& scene = fixture.GetScene();
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			Entity source = fixture.AddSource("Body", clip, glm::vec3(0.0f));
			TransformSystem::Update(scene);
			AudioSystem system(scene, fixture.GetSpecification());
			system.Start();

			// A fixed step moved the body from x = 0 (the interpolation snapshot's previous pose) to x = 1; the frame's alpha
			// is 0.25, so it is drawn at x = 0.25, a quarter metre on in 1/60 s.
			source.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = glm::mat4(1.0f) });
			source.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
			scene.SetInterpolationAlpha(0.25f);
			fixture.Frame(system);
			AudioVoiceInfo voice = fixture.GetEngine().GetVoices().front();
			CHECK(voice.Transform.Position.x == doctest::Approx(0.25f));
			CHECK(voice.Transform.Velocity.x == doctest::Approx(15.0f));
			// The next frame runs no step: the world pose stands still and the drawn one moves on by another quarter metre.
			scene.SetInterpolationAlpha(0.5f);
			fixture.Frame(system);
			voice = fixture.GetEngine().GetVoices().front();
			CHECK(voice.Transform.Position.x == doctest::Approx(0.5f));
			CHECK(voice.Transform.Velocity.x == doctest::Approx(15.0f));

			// Teleported (InterpolationResetTag, PlaySession::MarkTeleported): drawn and heard at its current pose, with no
			// velocity, although 2.5 m in 1/60 s is slower than sound.
			source.GetComponent<TransformComponent>().Translation = glm::vec3(3.0f, 0.0f, 0.0f);
			source.AddComponent<InterpolationResetTag>();
			fixture.Frame(system);
			voice = fixture.GetEngine().GetVoices().front();
			CHECK(voice.Transform.Position.x == doctest::Approx(3.0f));
			CHECK(voice.Transform.Velocity == glm::vec3(0.0f));
			// Written outside the fixed steps on the next frame as well (a script's OnUpdate): a pose that stays reset moves.
			source.GetComponent<TransformComponent>().Translation = glm::vec3(3.5f, 0.0f, 0.0f);
			fixture.Frame(system);
			voice = fixture.GetEngine().GetVoices().front();
			CHECK(voice.Transform.Position.x == doctest::Approx(3.5f));
			CHECK(voice.Transform.Velocity.x == doctest::Approx(30.0f));
		}

		TEST_CASE("AudioSystem: the listener is heard from its rendered pose, and its ancestor's teleport stops its velocity")
		{
			AudioSceneFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity rig = scene.CreateEntity("Rig");
			Entity ear = scene.CreateEntity("Ear", rig);
			ear.AddComponent<AudioListenerComponent>(AudioListenerComponent{ .Primary = true });
			TransformSystem::Update(scene);
			AudioSystem system(scene, fixture.GetSpecification());
			system.Start();

			// A fixed step moved the rig from z = 0 to z = 2; at alpha 0.5 the ear is drawn at z = 1, a metre on in 1/60 s.
			rig.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = glm::mat4(1.0f) });
			ear.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = glm::mat4(1.0f) });
			rig.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 2.0f);
			scene.SetInterpolationAlpha(0.5f);
			fixture.Frame(system);
			AudioListenerPose pose = fixture.GetEngine().GetListener();
			CHECK(pose.Position.z == doctest::Approx(1.0f));
			CHECK(pose.Velocity.z == doctest::Approx(60.0f));
			scene.SetInterpolationAlpha(0.75f);
			fixture.Frame(system);
			pose = fixture.GetEngine().GetListener();
			CHECK(pose.Position.z == doctest::Approx(1.5f));
			CHECK(pose.Velocity.z == doctest::Approx(30.0f));

			// The rig is teleported 2.5 m on: the ear's pose stops interpolating with its ancestor's and has no velocity.
			rig.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 4.0f);
			rig.AddComponent<InterpolationResetTag>();
			fixture.Frame(system);
			pose = fixture.GetEngine().GetListener();
			CHECK(pose.Position.z == doctest::Approx(4.0f));
			CHECK(pose.Velocity == glm::vec3(0.0f));
		}

		TEST_CASE("AudioSystem: destroying the system releases every voice and clip")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			fixture.AddSource("Source", clip, glm::vec3(0.0f));
			TransformSystem::Update(fixture.GetScene());
			{
				AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
				system.Start();
				REQUIRE(system.PlayOneShot(clip, std::nullopt).has_value());
				CHECK(fixture.GetEngine().GetStats().LiveVoices == 2);
				CHECK(fixture.GetEngine().GetStats().RegisteredClips == 1);
			}
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);
			CHECK(fixture.GetEngine().GetStats().RegisteredClips == 0);
		}

		TEST_CASE("AudioSystem: MakeAudioClipSource names the clip by its handle and keeps the data alive")
		{
			Ref<AudioClipData> pcm = CreateRef<AudioClipData>(CreateSilentAudioClip());
			const AudioClipSource source = MakeAudioClipSource(AssetHandle(0x1234), 3, pcm);
			CHECK(source.Name == "0000000000001234");
			CHECK(source.Version == 3);
			CHECK(source.Format == AudioClipFormat::Pcm16);
			CHECK(source.SampleRate == 48000);
			CHECK(source.ChannelCount == 1);
			CHECK_FALSE(source.Stream);
			REQUIRE(source.Bytes != nullptr);
			CHECK(source.Bytes.get() == &pcm->Bytes);
			pcm.reset();
			CHECK(source.Bytes->size() == 4800 * sizeof(int16_t));

			const Ref<AudioClipData> encoded = CreateRef<AudioClipData>();
			encoded->Encoding = AudioClipEncoding::Vorbis;
			encoded->Stream = true;
			encoded->Bytes = Buffer(16);
			const AudioClipSource streamed = MakeAudioClipSource(AssetHandle(0x99), 1, encoded);
			CHECK(streamed.Format == AudioClipFormat::Encoded);
			CHECK(streamed.Stream);
		}

		TEST_CASE("AudioSystem: disabled listeners, cameras and sources do not count for the audio checks")
		{
			Test::SceneTestFixture fixture(1, false);
			Scene& scene = fixture.GetScene();
			Entity spatial = scene.CreateEntity("Spatial");
			spatial.AddComponent<AudioSourceComponent>();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			CHECK(FindAudioSceneIssues(scene).empty());
			// A disabled camera does not listen.
			camera.SetActive(false);
			CHECK(GetCodes(FindAudioSceneIssues(scene)) == std::vector<std::string>{ std::string(AudioNoListenerCode) });
			// Neither does a disabled listener, nor a secondary one.
			Entity group = scene.CreateEntity("Group");
			group.SetActive(false);
			Entity disabledEar = scene.CreateEntity("DisabledEar", group);
			disabledEar.AddComponent<AudioListenerComponent>();
			Entity secondaryEar = scene.CreateEntity("SecondaryEar");
			secondaryEar.AddComponent<AudioListenerComponent>(AudioListenerComponent{ .Primary = false });
			CHECK(GetCodes(FindAudioSceneIssues(scene)) == std::vector<std::string>{ std::string(AudioNoListenerCode) });
			CHECK(SelectAudioListener(scene) == AudioListenerSelection{});
			// A disabled spatial source is not heard at all.
			spatial.SetActive(false);
			CHECK(FindAudioSceneIssues(scene).empty());
			// Enabling the group brings its listener back, as the only primary one.
			group.SetActive(true);
			const AudioListenerSelection selection = SelectAudioListener(scene);
			CHECK(selection.Source == AudioListenerSource::Listener);
			CHECK(selection.Entity == disabledEar.GetUUID());
			CHECK(selection.PrimaryListenerCount == 1);
		}

		TEST_CASE("AudioSystem: the AudioSource methods check their entity")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity source = fixture.AddSource("Source", clip, glm::vec3(0.0f), false);
			const Entity silent = fixture.GetScene().CreateEntity("Silent");
			Test::SceneTestFixture other(2, true);
			const Entity stranger = other.GetScene().CreateEntity("Stranger");
			stranger.AddComponent<AudioSourceComponent>();
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();

			for (const Entity entity : { silent, stranger, Entity() })
			{
				const Status played = system.Play(entity);
				REQUIRE_FALSE(played.has_value());
				CHECK(played.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK_FALSE(system.Stop(entity).has_value());
				CHECK_FALSE(system.IsPlaying(entity));
			}
			// Stop, Pause and Resume of a source that does not play change nothing.
			CHECK(system.Stop(source).has_value());
			CHECK(system.Pause(source).has_value());
			CHECK(system.Resume(source).has_value());
			CHECK_FALSE(system.IsPlaying(source));
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);

			// A disabled source refuses Play and Resume.
			source.SetActive(false);
			const Status disabled = system.Play(source);
			REQUIRE_FALSE(disabled.has_value());
			CHECK(disabled.error().GetCode() == ErrorCode::InvalidState);
			const Status resumed = system.Resume(source);
			REQUIRE_FALSE(resumed.has_value());
			CHECK(resumed.error().GetCode() == ErrorCode::InvalidState);
			source.SetActive(true);

			// Play restarts a playing source from the start.
			REQUIRE(system.Play(source).has_value());
			fixture.Frame(system);
			REQUIRE(system.Play(source).has_value());
			const std::vector<AudioVoiceInfo> voices = fixture.GetEngine().GetVoices();
			REQUIRE(voices.size() == 1);
			CHECK(voices.front().CursorFrames == 0);
			CHECK(voices.front().Owner == source.GetUUID().GetValue());
		}

		TEST_CASE("AudioSystem: removing the AudioSource releases its voice and adding one plays it on start")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity source = fixture.AddSource("Source", clip, glm::vec3(0.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			REQUIRE(system.IsPlaying(source));
			source.RemoveComponent<AudioSourceComponent>();
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);

			AudioSourceComponent again;
			again.Clip.SetHandle(clip);
			again.PlayOnStart = true;
			again.Spatial = false;
			source.AddComponent<AudioSourceComponent>(again);
			CHECK_FALSE(system.IsPlaying(source));
			fixture.Frame(system);
			CHECK(system.IsPlaying(source));
			const std::vector<AudioVoiceInfo> voices = fixture.GetEngine().GetVoices();
			REQUIRE(voices.size() == 1);
			CHECK_FALSE(voices.front().Settings.Spatial);
		}

		TEST_CASE("AudioSystem: a disabled PlayOnStart source starts when its entity is enabled")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			const Entity source = fixture.AddSource("Source", clip, glm::vec3(0.0f));
			source.SetActive(false);
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			CHECK_FALSE(system.IsPlaying(source));
			fixture.Frame(system);
			CHECK(fixture.GetEngine().GetStats().LiveVoices == 0);
			source.SetActive(true);
			fixture.Frame(system);
			CHECK(system.IsPlaying(source));
		}

		TEST_CASE("AudioSystem: the primary camera listens when there is no listener, with its pose and velocity")
		{
			AudioSceneFixture fixture;
			Entity camera = fixture.GetScene().CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			camera.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
			system.Start();
			CHECK(system.GetListener().Source == AudioListenerSource::Camera);
			CHECK(system.GetListener().Entity == camera.GetUUID());
			AudioListenerPose pose = fixture.GetEngine().GetListener();
			CHECK(pose.Position == glm::vec3(1.0f, 2.0f, 3.0f));
			CHECK(pose.Forward == glm::vec3(0.0f, 0.0f, -1.0f));
			CHECK(pose.Up == glm::vec3(0.0f, 1.0f, 0.0f));
			CHECK(pose.Velocity == glm::vec3(0.0f));

			// Half a metre along +X in 1/60 s; a quarter turn about +Y looks down -X.
			camera.GetComponent<TransformComponent>().Translation.x += 0.5f;
			camera.GetComponent<TransformComponent>().Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, 90.0f, 0.0f));
			fixture.Frame(system);
			pose = fixture.GetEngine().GetListener();
			CHECK(pose.Position == glm::vec3(1.5f, 2.0f, 3.0f));
			CHECK(pose.Velocity.x == doctest::Approx(30.0f));
			CHECK(pose.Forward.x == doctest::Approx(-1.0f));
			CHECK(pose.Forward.z == doctest::Approx(0.0f).epsilon(1e-6));

			// A listener takes over from the camera; its first frame has no velocity.
			const Entity listener = fixture.AddListener();
			fixture.Frame(system);
			CHECK(system.GetListener().Entity == listener.GetUUID());
			CHECK(fixture.GetEngine().GetListener().Position == glm::vec3(0.0f));
			CHECK(fixture.GetEngine().GetListener().Velocity == glm::vec3(0.0f));
		}

		TEST_CASE("AudioSystem: without an asset manager every clip plays the silent clip with one warning each")
		{
			AudioSceneFixture fixture;
			const AssetHandle clip = fixture.AddToneClip(0xa001);
			fixture.AddListener();
			fixture.AddSource("First", clip, glm::vec3(0.0f));
			fixture.AddSource("Second", clip, glm::vec3(0.0f));
			TransformSystem::Update(fixture.GetScene());
			const Test::ExpectLog warned(LogLevel::Warn, clip.ToString());
			{
				AudioSystem system(fixture.GetScene(), AudioSystemSpecification{ .Audio = &fixture.GetEngine(), .Assets = nullptr });
				system.Start();
				const std::vector<AudioVoiceInfo> voices = fixture.GetEngine().GetVoices();
				REQUIRE(voices.size() == 2);
				for (const AudioVoiceInfo& voice : voices)
					CHECK(voice.ClipName == BuiltinAssetHandles::SilentClip.ToString());
				CHECK(fixture.GetEngine().GetStats().RegisteredClips == 1);
				CHECK(MeasureAudioLevels(fixture.Frame(system)).Peak == 0.0f);
			}
			CHECK(warned.GetMatchCount() == 1);
		}
	}

}
