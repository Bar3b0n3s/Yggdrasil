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
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AudioTestData.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Components to voices (Architecture §10.2, §10.4). The listener-source names are implemented by the M12 contract; the
// other tests are skipped skeletons (Docs/Decisions/0015-m12-decisions.md): stream C implements AudioSystem and the
// validation checks and removes the skips. They run a device-less AudioEngine with deterministic decoding (stream A) over
// clips served from memory.

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

		TEST_CASE("AudioSystem: PlayOnStart sources start with the session and others wait for Play" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: a spatial source to the listener's right is louder on the right" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: captured PCM of a fixed scene is bit-identical across runs" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: a missing clip yields a diagnostic and silence" * doctest::skip(true))
		{
			AudioSceneFixture fixture;
			fixture.AddListener();
			const AssetHandle missing(0xdead);
			const Entity source = fixture.AddSource("Missing", missing, glm::vec3(0.0f, 0.0f, -1.0f));
			TransformSystem::Update(fixture.GetScene());
			AudioSystem system(fixture.GetScene(), fixture.GetSpecification());
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

		TEST_CASE("AudioSystem: the listener is the first primary AudioListener, else the primary camera" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: several primary listeners raise AUDIO_MULTIPLE_PRIMARY_LISTENERS" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: group volumes belong to the session and destroying the system restores the engine's" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: spatial sources without a listener or a camera raise AUDIO_NO_LISTENER" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: component fields reach the voice at every update" * doctest::skip(true))
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
			CHECK(voice.Priority == AudioSystemMusicPriority);
			CHECK(voice.Settings.Volume == 0.25f);
			CHECK(voice.Settings.Pitch == 1.5f);
			CHECK(voice.Settings.Group == AudioGroup::Music);
			const AudioSpatialization expected{
				.Model = Attenuation::Linear, .MinDistance = 2.0f, .MaxDistance = 8.0f, .Rolloff = 1.0f, .DopplerFactor = 0.0f
			};
			CHECK(voice.Settings.Spatialization == expected);
			CHECK(voice.Transform.Position == glm::vec3(0.0f, 0.0f, -4.0f));
			// A changed clip restarts the playing voice with the new clip.
			const AssetHandle other = fixture.AddToneClip(0xa002);
			component.Clip.SetHandle(other);
			fixture.Frame(system);
			REQUIRE(fixture.GetEngine().GetVoices().size() == 1);
			CHECK(fixture.GetEngine().GetVoices().front().ClipName == other.ToString());
		}

		TEST_CASE("AudioSystem: destroying or disabling a source's entity releases its voice" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: pause holds every voice and resume continues it" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: PlayOneShot is non-spatial without a position and plays in the Sfx group by default" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: sources created during play with PlayOnStart start at the next update" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: a source's velocity comes from its world-position delta" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: destroying the system releases every voice and clip" * doctest::skip(true))
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

		TEST_CASE("AudioSystem: MakeAudioClipSource names the clip by its handle and keeps the data alive" * doctest::skip(true))
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
	}

}
