#include "TestsPCH.h"

#include "EditorCore/Lighting/LightingBake.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/ProbeCapture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>

// lighting.bake's work (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A): BakeLighting's
// refusals without a renderer, and, on the GPU, the acceptance "two probe bakes are byte-identical" (the first bake writes,
// imports and assigns every probe's data in one undo step; the second writes the same bytes, changes no component and records
// no step), the device allocations a bake leaves whatever its probe count (World Tree's brief 21), the options and disabled
// probes, and the folders of scenes that share a name. The method's round trip is Automation/LightingMethodsTests.cpp.

namespace Engine {

	namespace {

		// The ids of the room's probes (MakeRoom), fixed so that two rooms share them as a scene copied with its ids does.
		constexpr UUID RoomVolumeId{ 0x7a1c3e5f20b4d608 };
		constexpr UUID RoomMirrorId{ 0x51d0a2e4c9b37f16 };

		// A room named `name` on `editor`'s registry: a floor, lit from straight above when `sunlit`, a 2 x 2 x 2 irradiance
		// volume around it (RoomVolumeId) and a 32-texel reflection probe above it (RoomMirrorId).
		Scope<Scene> MakeRoom(EditorContext& editor, std::string name, bool sunlit)
		{
			Scope<Scene> scene = editor.CreateScene(std::move(name));
			const Entity floor = scene->CreateEntity("Floor");
			floor.GetComponent<TransformComponent>().Scale = glm::vec3(4.0f);
			floor.AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::PlaneMesh);
			if (sunlit)
			{
				const Entity sun = scene->CreateEntity("Sun");
				sun.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
				sun.AddComponent<DirectionalLightComponent>();
			}
			const Entity volume = scene->CreateEntityWithID(RoomVolumeId, "Volume");
			IrradianceVolumeComponent& grid = volume.AddComponent<IrradianceVolumeComponent>();
			grid.Size = glm::vec3(4.0f, 2.0f, 4.0f);
			grid.ProbeCountX = 2;
			grid.ProbeCountY = 2;
			grid.ProbeCountZ = 2;
			const Entity mirror = scene->CreateEntityWithID(RoomMirrorId, "Mirror");
			mirror.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
			mirror.AddComponent<ReflectionProbeComponent>().Resolution = 32;
			return scene;
		}

		// Writes `scene` to the project path `path` and returns that path.
		VfsPath SaveScene(EditorContext& editor, const Scene& scene, std::string_view path)
		{
			const Result<VfsPath> scenePath = VfsPath::Create("project", path);
			REQUIRE(scenePath.has_value());
			const Result<std::string> text = SceneSerializer::SaveToString(scene);
			REQUIRE(text.has_value());
			REQUIRE(editor.CreateProjectDirectory(scenePath->GetParent()).has_value());
			REQUIRE(editor.WriteProjectFile(*scenePath, AsBytes(*text)).has_value());
			return *scenePath;
		}

		// An editor whose engine context mounts the repository's Resources (as a development editor does) with `baker` as its
		// environment baker, so the bake's .hdr imports as an Environment, and a project whose scene Assets/Scenes/Room.scene
		// (MakeRoom, sunlit) is saved and open.
		class BakeEditor
		{
		public:
			explicit BakeEditor(IEnvironmentBaker& baker)
				: m_Directory("LightingBake")
			{
				REQUIRE(FileSystem::CreateDirectories(m_Directory / "UserData").has_value());
				Result<Scope<EngineContext>> engine = EngineContext::Create({
					.WorkerCount = 0,
					.UserDataDirectory = m_Directory / "UserData",
					.EngineResourcesDirectory = Test::GetRepositoryRoot() / "Resources",
					.EngineCacheDirectory = m_Directory / "EngineCache",
					.RegisterTypes = &RegisterEditorMethodTypes,
				});
				REQUIRE_MESSAGE(engine.has_value(), engine.error().ToString());
				m_Engine = std::move(*engine);
				Result<Scope<EditorContext>> editor = EditorContext::Create(*m_Engine, {
																						   .IdGeneratorState = Test::EditorTestIdState,
																						   .TemplatesDirectory = Test::GetRepositoryRoot() / "Resources" / "Templates" / "Projects",
																						   .ReadOnlyCacheRoot = m_Directory / "ReadOnlyCache",
																						   .HistoryLimits = {},
																						   .EnvironmentBaker = &baker,
																					   });
				REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
				m_Editor = std::move(*editor);

				const Result<CreatedProject> created = ProjectManager::CreateProject(
					{
						.Directory = GetProjectRoot(),
						.Name = "TestProject",
						.Template = ProjectTemplate::Empty,
						.TemplatesDirectory = m_Editor->GetSpecification().TemplatesDirectory,
					},
					m_Engine->GetTypeRegistry());
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(created->ProjectFile, {}, m_Engine->GetTypeRegistry());
				REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
				REQUIRE(m_Editor->OpenProject(std::move(*project)).has_value());

				Scope<Scene> scene = MakeRoom(*m_Editor, "Room", true);
				const VfsPath path = SaveScene(*m_Editor, *scene, "Assets/Scenes/Room.scene");
				m_Editor->SetScene(std::move(scene), path);
			}

			~BakeEditor()
			{
				// The editor holds the project lock and the project:// mount: release both before the directory goes.
				m_Editor.reset();
				m_Engine.reset();
			}

			BakeEditor(const BakeEditor&) = delete;
			BakeEditor& operator=(const BakeEditor&) = delete;

			[[nodiscard]] EditorContext& GetEditor() { return *m_Editor; }
			[[nodiscard]] std::filesystem::path GetProjectRoot() const { return m_Directory / "TestProject"; }

			// Saves `scene` to `path` and opens it beside the open scenes as the active one; returns its id.
			[[nodiscard]] OpenSceneId OpenScene(Scope<Scene> scene, std::string_view path)
			{
				const VfsPath saved = SaveScene(*m_Editor, *scene, path);
				const Result<OpenSceneId> opened = m_Editor->AddScene(std::move(scene), saved);
				REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
				return *opened;
			}
		private:
			Test::TempDirectory m_Directory;
			Scope<EngineContext> m_Engine;
			Scope<EditorContext> m_Editor;
		};

		// The probe capture the editor's startup injects, on `gpu`'s device: a cache over the editor's assets, the renderer's
		// pipelines and the ProbeCapture, set as the editor's (EditorContext::SetProbeCapture). `gpu` and `editor` are
		// documented back-references that outlive this; the destructor takes the capture out of the editor, destroys the
		// capture (its renderer), then the pipelines, and idles the device before the cache goes.
		class BakeCapture
		{
		public:
			BakeCapture(Test::HeadlessGpuFixture& gpu, EditorContext& editor)
				: m_Gpu(gpu), m_Editor(editor), m_Cache(gpu.GetDevice(), editor.GetAssets())
			{
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				m_Pipelines = std::move(*pipelines);
				Result<Scope<ProbeCapture>> capture = ProbeCapture::Create(gpu.GetDevice(), *m_Pipelines, m_Cache, editor.GetAssets());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				m_Capture = std::move(*capture);
				editor.SetProbeCapture(m_Capture.get());
			}

			~BakeCapture()
			{
				m_Editor.SetProbeCapture(nullptr);
				m_Capture.reset();
				m_Pipelines.reset();
				m_Gpu.GetDevice().WaitForIdle();
			}

			BakeCapture(const BakeCapture&) = delete;
			BakeCapture& operator=(const BakeCapture&) = delete;
		private:
			Test::HeadlessGpuFixture& m_Gpu; // documented back-reference
			EditorContext& m_Editor;         // documented back-reference
			GpuResourceCache m_Cache;
			Scope<SceneRendererPipelines> m_Pipelines{};
			Scope<ProbeCapture> m_Capture{};
		};

		// The bytes of every file a bake reported, by project-relative path.
		std::map<std::string, Buffer> ReadBakedFiles(const std::filesystem::path& root, const LightingBakeResult& result)
		{
			std::map<std::string, Buffer> files;
			for (const auto* entries : { &result.IrradianceVolumes, &result.ReflectionProbes })
			{
				for (const LightingBakeEntry& entry : *entries)
				{
					Result<Buffer> bytes = FileSystem::ReadFile(root / entry.Path);
					REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
					files.emplace(entry.Path, std::move(*bytes));
				}
			}
			return files;
		}

		// The bytes of the project file `relative` under `root`.
		Buffer ReadProjectFile(const std::filesystem::path& root, std::string_view relative)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(root / FileSystem::PathFromUtf8(relative));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

		// Whether `root` holds a project folder Assets/Lighting.
		bool HasLightingFolder(const std::filesystem::path& root)
		{
			std::error_code error;
			return std::filesystem::exists(root / "Assets" / "Lighting", error);
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Decision 8's refusals, before anything renders or is written: an unsaved scene (the hint names scene.save), and an
		// editor without a renderer (--renderer none: no probe capture injected).
		TEST_CASE("LightingBake: an unsaved scene and an editor without a renderer are refused, writing nothing")
		{
			Test::EditorTestFixture fixture("LightingBakeRefusals");
			fixture.CreateAndOpenProject();
			EditorContext& editor = fixture.GetEditor();
			editor.SetScene(editor.CreateScene("Unsaved"), std::nullopt);
			const Result<LightingBakeResult> unsaved = BakeLighting(editor, editor.GetActiveSceneId(), {});
			REQUIRE_FALSE(unsaved.has_value());
			CHECK(unsaved.error().GetCode() == ErrorCode::InvalidState);
			CHECK(unsaved.error().GetHint().find("scene.save") != std::string::npos);

			fixture.CreateAndOpenScene("Assets/Scenes/Room.scene");
			const uint64_t revision = editor.GetRevision();
			const Result<LightingBakeResult> noRenderer = BakeLighting(editor, editor.GetActiveSceneId(), {});
			REQUIRE_FALSE(noRenderer.has_value());
			CHECK(noRenderer.error().GetCode() == ErrorCode::InvalidState);
			CHECK(editor.GetRevision() == revision);
			CHECK_FALSE(HasLightingFolder(fixture.GetProjectRoot()));

			// An open scene id that names nothing.
			const Result<LightingBakeResult> unknown = BakeLighting(editor, editor.GetActiveSceneId() + 100, {});
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
		}

		// Decision 8 (W26.1's review): every probe the options select is checked before anything renders, so a probe out of
		// its ranges (COMPONENT_FIELD_OUT_OF_RANGE) refuses the bake with Validation even in an editor that could not render
		// it, and nothing is written; a kind the options leave out is not checked. A grid count far beyond the limit is refused
		// before it sizes anything.
		TEST_CASE("LightingBake: a probe out of its ranges is refused with Validation before anything renders, writing nothing")
		{
			Test::EditorTestFixture fixture("LightingBakeValidation");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene("Assets/Scenes/Room.scene");
			EditorContext& editor = fixture.GetEditor();
			Scene& scene = editor.GetScene();
			const Entity volume = scene.CreateEntity("Volume");
			volume.AddComponent<IrradianceVolumeComponent>();
			const Entity mirror = scene.CreateEntity("Mirror");
			mirror.AddComponent<ReflectionProbeComponent>();
			const uint64_t revision = editor.GetRevision();
			const auto refused = [&editor](const LightingBakeOptions& options)
			{
				const Result<LightingBakeResult> baked = BakeLighting(editor, editor.GetActiveSceneId(), options);
				REQUIRE_FALSE(baked.has_value());
				return baked.error().GetCode();
			};
			const auto setGrid = [&volume](uint32_t countY, uint32_t countZ, float sizeX)
			{
				IrradianceVolumeComponent& grid = volume.GetComponent<IrradianceVolumeComponent>();
				grid.ProbeCountY = countY;
				grid.ProbeCountZ = countZ;
				grid.Size.x = sizeX;
			};

			// A reflection probe's Resolution that is not a power of two, and one beyond the largest.
			mirror.GetComponent<ReflectionProbeComponent>().Resolution = 48;
			CHECK(refused({}) == ErrorCode::Validation);
			mirror.GetComponent<ReflectionProbeComponent>().Resolution = 1024;
			CHECK(refused({}) == ErrorCode::Validation);
			// Left out by its option, it is not checked: the next refusal is the missing renderer's.
			CHECK(refused({ .IrradianceVolumes = true, .ReflectionProbes = false }) == ErrorCode::InvalidState);
			mirror.GetComponent<ReflectionProbeComponent>().Resolution = 64;

			// A volume with too few probes along an axis, far too many, or a box side beyond the largest.
			setGrid(1, 4, 10.0f);
			CHECK(refused({}) == ErrorCode::Validation);
			setGrid(0xFFFFFFFFU, 0xFFFFFFFFU, 10.0f);
			CHECK(refused({}) == ErrorCode::Validation);
			setGrid(3, 4, MaxIrradianceVolumeSize * 2.0f);
			CHECK(refused({}) == ErrorCode::Validation);
			CHECK(refused({ .IrradianceVolumes = false, .ReflectionProbes = true }) == ErrorCode::InvalidState);

			// Every probe in range: only the renderer is missing.
			setGrid(3, 4, 10.0f);
			CHECK(refused({}) == ErrorCode::InvalidState);
			CHECK(editor.GetRevision() == revision);
			CHECK_FALSE(HasLightingFolder(fixture.GetProjectRoot()));
		}

		// The acceptance (Roadmap M26, decision 8): "two probe bakes are byte-identical".
		TEST_CASE("LightingBake: two bakes of a scene write byte-identical probe files" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
			{
				BakeEditor setup(**baker);
				EditorContext& editor = setup.GetEditor();
				const BakeCapture capture(gpu, editor);

				const Result<LightingBakeResult> first = BakeLighting(editor, editor.GetActiveSceneId(), {});
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				REQUIRE(first->IrradianceVolumes.size() == 1);
				REQUIRE(first->ReflectionProbes.size() == 1);
				const LightingBakeEntry& volume = first->IrradianceVolumes.front();
				const LightingBakeEntry& mirror = first->ReflectionProbes.front();
				CHECK(volume.Entity == RoomVolumeId);
				// The folder mirrors the scene's path below Assets/ (Assets/Scenes/Room.scene).
				CHECK(volume.Path == "Assets/Lighting/Scenes/Room/" + RoomVolumeId.ToString() + ".irradiance");
				CHECK(mirror.Path == "Assets/Lighting/Scenes/Room/" + RoomMirrorId.ToString() + ".hdr");
				CHECK(volume.Changed);
				CHECK(mirror.Changed);
				CHECK(editor.GetHistory().GetUndoLabel().find("Bake Lighting") != std::string::npos);
				Scene& scene = editor.GetScene();
				CHECK(scene.FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>().Data.GetHandle() == volume.Asset);
				CHECK(scene.FindEntityByID(RoomMirrorId).GetComponent<ReflectionProbeComponent>().Data.GetHandle() == mirror.Asset);

				// The data imported as their types: the volume's grid, whose upper probes see the lit floor below them.
				const Result<AssetRef<Asset>> loadedVolume = editor.GetAssets().Load(volume.Asset);
				REQUIRE_MESSAGE(loadedVolume.has_value(), loadedVolume.error().ToString());
				const AssetRef<IrradianceVolumeData> grid = AssetCast<IrradianceVolumeData>(*loadedVolume);
				REQUIRE(grid != nullptr);
				CHECK(grid->CountX == 2);
				CHECK(grid->Size == glm::vec3(4.0f, 2.0f, 4.0f));
				const IrradianceProbe& upper = grid->Probes[GetIrradianceProbeIndex(*grid, 0, 1, 0)];
				std::array<glm::dvec3, 9> coefficients{};
				for (size_t k = 0; k < 9; ++k)
					coefficients[k] = glm::dvec3(upper.SH9[k]);
				CHECK(Test::EvaluateIrradianceSH9(coefficients, glm::dvec3(0.0, -1.0, 0.0)).x > 0.01);
				const Result<AssetRef<Asset>> loadedMirror = editor.GetAssets().Load(mirror.Asset);
				REQUIRE_MESSAGE(loadedMirror.has_value(), loadedMirror.error().ToString());
				CHECK(AssetCast<EnvironmentData>(*loadedMirror) != nullptr);
				const std::map<std::string, Buffer> baked = ReadBakedFiles(setup.GetProjectRoot(), *first);

				// The second bake of the unchanged scene: the same bytes, no file rewritten, no component changed, no undo step.
				// The scene now holds baked probes, which render extraction lists in every view (stream Q-B), so the second
				// bake's captures meet the first bake's volume and reflection probe: only the captures' Probes off
				// (RenderQualitySettings::Probes, ProbeCapture) keeps them out of the bake (one bounce), which these equal
				// bytes prove (W26.1's integration, Docs/Decisions/0037-m26-contract.md decisions 8 and 16).
				const Result<RenderSnapshot> withProbes = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
				REQUIRE_MESSAGE(withProbes.has_value(), withProbes.error().ToString());
				REQUIRE(withProbes->IrradianceVolumes.size() == 1);
				REQUIRE(withProbes->ReflectionProbes.size() == 1);
				CHECK(withProbes->IrradianceVolumes.front().Data == volume.Asset);
				CHECK(withProbes->ReflectionProbes.front().Data == mirror.Asset);
				const size_t undoCount = editor.GetHistory().GetUndoCount();
				const uint64_t revision = editor.GetRevision();
				const Result<LightingBakeResult> second = BakeLighting(editor, editor.GetActiveSceneId(), {});
				REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
				REQUIRE(second->IrradianceVolumes.size() == 1);
				REQUIRE(second->ReflectionProbes.size() == 1);
				CHECK_FALSE(second->IrradianceVolumes.front().Changed);
				CHECK_FALSE(second->ReflectionProbes.front().Changed);
				CHECK(second->IrradianceVolumes.front().Asset == volume.Asset);
				CHECK(second->ReflectionProbes.front().Asset == mirror.Asset);
				CHECK(ReadBakedFiles(setup.GetProjectRoot(), *second) == baked);
				CHECK(editor.GetHistory().GetUndoCount() == undoCount);
				CHECK(editor.GetRevision() == revision);

				// The one undo step takes the first bake's assignment back.
				REQUIRE(editor.GetHistory().Undo(editor).has_value());
				CHECK_FALSE(scene.FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>().Data.IsValid());
				CHECK_FALSE(scene.FindEntityByID(RoomMirrorId).GetComponent<ReflectionProbeComponent>().Data.IsValid());
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// World Tree's brief 21 (its look pass, where the bake of one 13 x 3 x 13 volume left 23,521 native allocations at its
		// end): every capture of a bake releases what it used before the next (ProbeCapture), and so does every reflection
		// probe's import (EnvironmentBaker), so a bake leaves the device's allocations at a fixed amount whatever its probe
		// count, and a bake straight after another, with no frame and so no garbage collection between them, adds nothing to
		// them: neither a second bake of the scene nor a third of six times the volume's probes and four times the reflection
		// probes.
		TEST_CASE("LightingBake: a second bake, even of many more probes, adds nothing to the device's allocations" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
			{
				BakeEditor setup(**baker);
				EditorContext& editor = setup.GetEditor();
				const BakeCapture capture(gpu, editor);
				GraphicsDevice& device = gpu.GetDevice();
				const OpenSceneId room = editor.GetActiveSceneId();
				const Result<LightingBakeResult> first = BakeLighting(editor, room, {});
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				const uint32_t afterFirst = device.GetMemoryAllocationCount();

				const Result<LightingBakeResult> second = BakeLighting(editor, room, {});
				REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
				CHECK(device.GetMemoryAllocationCount() <= afterFirst);

				// The volume's 2 x 2 x 2 probes become 4 x 3 x 4 (48 captures instead of 8), and three more reflection probes
				// are captured and imported beside the first.
				Scene& scene = editor.GetScene();
				{
					IrradianceVolumeComponent& grid = scene.FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>();
					grid.ProbeCountX = 4;
					grid.ProbeCountY = 3;
					grid.ProbeCountZ = 4;
				}
				for (const float x : { -1.5f, 0.5f, 1.5f })
				{
					const Entity mirror = scene.CreateEntity("Mirror");
					mirror.GetComponent<TransformComponent>().Translation = glm::vec3(x, 0.5f, 0.0f);
					mirror.AddComponent<ReflectionProbeComponent>().Resolution = 32;
				}
				const Result<LightingBakeResult> larger = BakeLighting(editor, room, {});
				REQUIRE_MESSAGE(larger.has_value(), larger.error().ToString());
				REQUIRE(larger->IrradianceVolumes.size() == 1);
				CHECK(larger->IrradianceVolumes.front().Changed);
				REQUIRE(larger->ReflectionProbes.size() == 4);
				CHECK(device.GetMemoryAllocationCount() <= afterFirst);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// lighting.bake {irradianceVolumes, reflectionProbes} (decision 8): each option bakes its kind alone, leaving the other
		// kind's files and Data as they were, and an effectively disabled probe is never baked.
		// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3, "Relit probes"): under an enabled Sky the room's volume
		// bakes relightable (version 2 with its relight block, Relightable in the result; its reflection probe as before), two
		// bakes write the same bytes, and a renderer that relights the baked volume at the bake's own sky shades a white quad
		// inside it as the baked coefficients do without relighting (the bake's sky is the renderer's image-based light, read
		// back from the capture's renderer).
		TEST_CASE("LightingBake: a volume baked under a Sky is relightable, bakes byte-identically twice, and relights to its bake at the bake's sky"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
			{
				BakeEditor setup(**baker);
				EditorContext& editor = setup.GetEditor();
				// A Sky whose null Sun takes the room's sun, turned 40 degrees from the zenith so the sky has a slope.
				Scene& scene = editor.GetScene();
				scene.CreateEntity("Sky").AddComponent<SkyComponent>();
				const Entity sun = scene.FindEntityByPath("/Sun");
				REQUIRE(sun.IsValid());
				sun.GetComponent<TransformComponent>().Rotation = glm::angleAxis(glm::radians(-50.0f), glm::vec3(1.0f, 0.0f, 0.0f));
				const BakeCapture capture(gpu, editor);

				const Result<LightingBakeResult> first = BakeLighting(editor, editor.GetActiveSceneId(), {});
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				REQUIRE(first->IrradianceVolumes.size() == 1);
				REQUIRE(first->ReflectionProbes.size() == 1);
				CHECK(first->IrradianceVolumes.front().Relightable);
				CHECK_FALSE(first->ReflectionProbes.front().Relightable);
				const std::map<std::string, Buffer> baked = ReadBakedFiles(setup.GetProjectRoot(), *first);
				const Buffer& source = baked.at(first->IrradianceVolumes.front().Path);
				REQUIRE(source.size() > 8);
				CHECK(std::to_integer<uint32_t>(source[4]) == IrradianceVolumeData::FormatVersion);
				const Result<IrradianceVolumeData> volume = ReadIrradianceVolumeSource(source);
				REQUIRE_MESSAGE(volume.has_value(), volume.error().ToString());
				REQUIRE(volume->Relight.size() == volume->Probes.size());
				// The room has no light of its own, so every probe's local light is dark, and its upper probes see the sky.
				for (const IrradianceProbeRelight& block : volume->Relight)
					CHECK(block.Local[0] == glm::vec3(0.0f));
				CHECK(volume->Relight[GetIrradianceProbeIndex(*volume, 0, 1, 0)].Transfer[0] > 0.1f);

				const Result<LightingBakeResult> second = BakeLighting(editor, editor.GetActiveSceneId(), {});
				REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
				CHECK_FALSE(second->IrradianceVolumes.front().Changed);
				CHECK(second->IrradianceVolumes.front().Relightable);
				CHECK(ReadBakedFiles(setup.GetProjectRoot(), *second) == baked);

				// Relit at the bake's sky, the volume shades as its bake: the scene's Sky and lights, the probes around the
				// fixture's quad, once relightable and once without the block.
				const Result<RenderSnapshot> extracted = ExtractRenderSnapshot(scene, { .Width = 8, .Height = 8 });
				REQUIRE_MESSAGE(extracted.has_value(), extracted.error().ToString());
				REQUIRE(extracted->Sky.Enabled);
				Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
				constexpr AssetHandle Relit{ 0x9c01 };
				constexpr AssetHandle Plain{ 0x9c02 };
				fixture.Assets().Publish(Relit, CreateRef<IrradianceVolumeData>(*volume));
				IrradianceVolumeData plain = *volume;
				plain.Relight.clear();
				fixture.Assets().Publish(Plain, CreateRef<IrradianceVolumeData>(std::move(plain)));
				const auto shade = [&gpu, &fixture, &extracted](AssetHandle data, uint64_t frame)
				{
					RenderSnapshot snapshot = fixture.Snapshot();
					snapshot.Lights = extracted->Lights;
					snapshot.Sky = extracted->Sky;
					snapshot.Camera.ClearToSkybox = true;
					snapshot.FrameIndex = frame;
					snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)),
						.Size = glm::vec3(4.0f),
						.Data = data,
						.BlendDistance = 0.0f,
						.Entity = UUID(31) });
					fixture.Submit(snapshot);
					Readback readback(gpu.GetDevice());
					Result<Image> image = readback.ReadTexture(*fixture.Renderer().GetSceneColorTexture());
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					return std::move(*image);
				};
				const Image relit = shade(Relit, 40);
				CHECK(fixture.Renderer().GetRenderStats().RelitIrradianceVolumes == 1U);
				const Image plainImage = shade(Plain, 41);
				CHECK(fixture.Renderer().GetRenderStats().RelitIrradianceVolumes == 0U);
				// SceneColor's binary16 green over the quad's centre square, relit against baked.
				double relitSum = 0.0;
				double plainSum = 0.0;
				for (uint32_t y = 12; y < 20; ++y)
				{
					for (uint32_t x = 12; x < 20; ++x)
					{
						uint16_t relitGreen = 0;
						uint16_t plainGreen = 0;
						std::memcpy(&relitGreen, relit.GetRow(y).data() + static_cast<size_t>(x) * 8 + 2, sizeof(relitGreen));
						std::memcpy(&plainGreen, plainImage.GetRow(y).data() + static_cast<size_t>(x) * 8 + 2, sizeof(plainGreen));
						relitSum += Test::HalfToDouble(relitGreen);
						plainSum += Test::HalfToDouble(plainGreen);
					}
				}
				INFO("relit " << relitSum << ", baked " << plainSum);
				CHECK(plainSum > 0.0);
				CHECK(relitSum == doctest::Approx(plainSum).epsilon(0.005));
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("LightingBake: each option bakes its kind alone, and a disabled probe is left out" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
			{
				BakeEditor setup(**baker);
				EditorContext& editor = setup.GetEditor();
				const BakeCapture capture(gpu, editor);
				Scene& scene = editor.GetScene();
				const Entity disabled = scene.CreateEntity("Disabled");
				IrradianceVolumeComponent& disabledGrid = disabled.AddComponent<IrradianceVolumeComponent>();
				disabledGrid.ProbeCountX = 2;
				disabledGrid.ProbeCountY = 2;
				disabledGrid.ProbeCountZ = 2;
				disabled.AddComponent<ReflectionProbeComponent>().Resolution = 32;
				disabled.SetActive(false);
				const UUID disabledId = disabled.GetUUID();

				const OpenSceneId room = editor.GetActiveSceneId();
				const Result<LightingBakeResult> volumes = BakeLighting(editor, room, { .IrradianceVolumes = true, .ReflectionProbes = false });
				REQUIRE_MESSAGE(volumes.has_value(), volumes.error().ToString());
				REQUIRE(volumes->IrradianceVolumes.size() == 1);
				CHECK(volumes->IrradianceVolumes.front().Entity == RoomVolumeId);
				CHECK(volumes->ReflectionProbes.empty());
				const AssetHandle volumeAsset = volumes->IrradianceVolumes.front().Asset;
				CHECK(scene.FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>().Data.GetHandle() == volumeAsset);
				CHECK_FALSE(scene.FindEntityByID(RoomMirrorId).GetComponent<ReflectionProbeComponent>().Data.IsValid());
				const std::filesystem::path folder = setup.GetProjectRoot() / "Assets" / "Lighting" / "Scenes" / "Room";
				std::error_code error;
				CHECK_FALSE(std::filesystem::exists(folder / (RoomMirrorId.ToString() + ".hdr"), error));
				const Buffer volumeBytes = ReadProjectFile(setup.GetProjectRoot(), volumes->IrradianceVolumes.front().Path);

				const Result<LightingBakeResult> reflections = BakeLighting(editor, room, { .IrradianceVolumes = false, .ReflectionProbes = true });
				REQUIRE_MESSAGE(reflections.has_value(), reflections.error().ToString());
				CHECK(reflections->IrradianceVolumes.empty());
				REQUIRE(reflections->ReflectionProbes.size() == 1);
				CHECK(reflections->ReflectionProbes.front().Entity == RoomMirrorId);
				const AssetHandle mirrorAsset = reflections->ReflectionProbes.front().Asset;
				CHECK(scene.FindEntityByID(RoomMirrorId).GetComponent<ReflectionProbeComponent>().Data.GetHandle() == mirrorAsset);
				CHECK(scene.FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>().Data.GetHandle() == volumeAsset);
				CHECK(ReadProjectFile(setup.GetProjectRoot(), volumes->IrradianceVolumes.front().Path) == volumeBytes);

				// The disabled entity's probes: neither baked nor assigned, by either bake.
				CHECK_FALSE(std::filesystem::exists(folder / (disabledId.ToString() + ".irradiance"), error));
				CHECK_FALSE(std::filesystem::exists(folder / (disabledId.ToString() + ".hdr"), error));
				CHECK_FALSE(disabled.GetComponent<IrradianceVolumeComponent>().Data.IsValid());
				CHECK_FALSE(disabled.GetComponent<ReflectionProbeComponent>().Data.IsValid());
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		// W26.1's review (decision 31's amendment of decision 8): a bake's folder mirrors its scene's path below Assets/, so two
		// scenes of one name in different folders, the second a copy that kept the first's entity ids (as scene.save {path}
		// keeps them), bake into folders of their own and never rewrite each other's data. Each is baked as an open scene that
		// is not the active one, through the scene target BakeLighting addresses itself.
		TEST_CASE("LightingBake: scenes of one name in different folders bake into their own folders, an inactive scene through its target"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
			{
				BakeEditor setup(**baker);
				EditorContext& editor = setup.GetEditor();
				const BakeCapture capture(gpu, editor);
				const std::filesystem::path root = setup.GetProjectRoot();
				const OpenSceneId forest = setup.OpenScene(MakeRoom(editor, "Main", true), "Assets/Levels/Forest/Main.scene");
				const OpenSceneId desert = setup.OpenScene(MakeRoom(editor, "Main", false), "Assets/Levels/Desert/Main.scene");
				const OpenSceneId room = editor.GetOpenScenes().front();
				REQUIRE(editor.ActivateScene(room).has_value());

				const Result<LightingBakeResult> forestBake = BakeLighting(editor, forest, { .IrradianceVolumes = true, .ReflectionProbes = false });
				REQUIRE_MESSAGE(forestBake.has_value(), forestBake.error().ToString());
				REQUIRE(forestBake->IrradianceVolumes.size() == 1);
				const LightingBakeEntry& forestVolume = forestBake->IrradianceVolumes.front();
				CHECK(forestVolume.Path == "Assets/Lighting/Levels/Forest/Main/" + RoomVolumeId.ToString() + ".irradiance");
				const Buffer forestBytes = ReadProjectFile(root, forestVolume.Path);
				CHECK(editor.GetActiveSceneId() == room);

				const Result<LightingBakeResult> desertBake = BakeLighting(editor, desert, { .IrradianceVolumes = true, .ReflectionProbes = false });
				REQUIRE_MESSAGE(desertBake.has_value(), desertBake.error().ToString());
				REQUIRE(desertBake->IrradianceVolumes.size() == 1);
				const LightingBakeEntry& desertVolume = desertBake->IrradianceVolumes.front();
				CHECK(desertVolume.Path == "Assets/Lighting/Levels/Desert/Main/" + RoomVolumeId.ToString() + ".irradiance");
				CHECK(desertVolume.Asset != forestVolume.Asset);
				CHECK(desertVolume.Changed);
				// The unlit copy's probes differ from the sunlit original's, whose file and Data stay its own.
				CHECK(ReadProjectFile(root, desertVolume.Path) != forestBytes);
				CHECK(ReadProjectFile(root, forestVolume.Path) == forestBytes);
				const Scene* forestScene = editor.FindScene(forest);
				const Scene* desertScene = editor.FindScene(desert);
				REQUIRE(forestScene != nullptr);
				REQUIRE(desertScene != nullptr);
				CHECK(forestScene->FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>().Data.GetHandle() == forestVolume.Asset);
				CHECK(desertScene->FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>().Data.GetHandle() == desertVolume.Asset);
				// The active scene was never baked or changed.
				CHECK(editor.GetActiveSceneId() == room);
				CHECK_FALSE(editor.GetScene().FindEntityByID(RoomVolumeId).GetComponent<IrradianceVolumeComponent>().Data.IsValid());
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
