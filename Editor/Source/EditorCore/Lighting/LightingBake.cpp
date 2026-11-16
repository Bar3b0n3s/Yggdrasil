#include "EditorPCH.h"
#include "EditorCore/Lighting/LightingBake.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Renderer/ProbeCapture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lighting.bake's work (LightingBake.h; Docs/Decisions/0037-m26-contract.md decision 8): every probe checked first, so a
// probe out of its ranges renders nothing; then every capture, so a failed capture writes nothing; then the files, written
// only where their bytes changed, their .meta files for new sources, and the imports of what changed; then the probes' Data
// in one undoable edit, recorded only when a probe's Data changes.

namespace Engine {

	namespace Utils {

		// The face size of an irradiance probe's capture (LightingBake.h: 16 texels a face, ample for an SH9 projection).
		constexpr uint32_t IrradianceProbeFaceSize = 16;
		// The captures' clip planes: geometry nearer than 5 cm to a probe is clipped (a probe is never inside a wall), and the
		// culling distance holds a volume of MaxIrradianceVolumeSize.
		constexpr float ProbeNearClip = 0.05f;
		constexpr float ProbeFarClip = 10000.0f;

		// One probe to bake, read from the scene before any capture.
		struct PendingProbe
		{
			UUID Entity{};
			bool IsVolume = false;
			glm::mat4 World = glm::mat4(1.0f);
			IrradianceVolumeComponent Volume{};
			ReflectionProbeComponent Reflection{};
			// The baked file: its path and bytes.
			VfsPath Path{};
			Buffer Bytes{};
		};

		// The scene's snapshot as every capture renders it (the camera is the capture's own): the scene's meshes, lights,
		// environment and post-processing at the project's rendering quality, cleared to black where no environment's skybox
		// draws, so a probe sees the scene's light and nothing an editor's clear colour adds. The scene's world matrices are
		// current (BakeLighting updates them first).
		[[nodiscard]] static Result<RenderSnapshot> ExtractBakeSnapshot(EditorContext& editor, const Scene& scene)
		{
			const ProjectSettings& settings = editor.GetProject().GetSettings();
			RenderExtractionRequest request;
			request.Camera = RenderCameraSource::Explicit;
			request.ExplicitCamera.NearClip = ProbeNearClip;
			request.ExplicitCamera.FarClip = ProbeFarClip;
			request.ExplicitCamera.ClearColor = glm::vec3(0.0f);
			request.Width = IrradianceProbeFaceSize;
			request.Height = IrradianceProbeFaceSize;
			request.Quality = { .ShadowMapSize = settings.Rendering.ShadowMapSize,
				.SsaoHalfResolution = settings.Rendering.SsaoHalfResolution,
				.MaxShadedLights = settings.Rendering.MaxShadedLights };
			request.SortingLayers = settings.SortingLayers.Layers;
			ENGINE_TRY_ASSIGN(RenderSnapshot snapshot, ExtractRenderSnapshot(scene, request));
			snapshot.Camera.ClearColor = glm::vec3(0.0f);
			snapshot.Camera.ClearToSkybox = true;
			snapshot.SceneRevision = scene.GetRevision();
			return snapshot;
		}

		// The probes of `scene` the options select, in canonical entity order, effectively enabled ones only.
		[[nodiscard]] static std::vector<PendingProbe> CollectProbes(const Scene& scene, const LightingBakeOptions& options)
		{
			std::vector<PendingProbe> probes;
			scene.ForEachCanonical([&probes, &options](ConstEntity entity)
			{
				if (!entity.IsActive())
					return;
				if (options.IrradianceVolumes)
				{
					if (const IrradianceVolumeComponent* volume = entity.TryGetComponent<IrradianceVolumeComponent>())
						probes.push_back({ .Entity = entity.GetUUID(), .IsVolume = true, .World = ComputeRenderedWorldMatrix(entity, 1.0f), .Volume = *volume });
				}
				if (options.ReflectionProbes)
				{
					if (const ReflectionProbeComponent* reflection = entity.TryGetComponent<ReflectionProbeComponent>())
					{
						probes.push_back(
							{ .Entity = entity.GetUUID(), .IsVolume = false, .World = ComputeRenderedWorldMatrix(entity, 1.0f), .Reflection = *reflection });
					}
				}
			});
			return probes;
		}

		// The empty grid of `probe`'s IrradianceVolume (its counts and Size, every probe's coefficients zero), checked against
		// IrradianceVolumeData's rules: the counts first, so a component out of its ranges (COMPONENT_FIELD_OUT_OF_RANGE) never
		// sizes an allocation. Errors: Validation naming the entity.
		[[nodiscard]] static Result<IrradianceVolumeData> MakeVolumeGrid(const PendingProbe& probe)
		{
			IrradianceVolumeData volume;
			volume.CountX = probe.Volume.ProbeCountX;
			volume.CountY = probe.Volume.ProbeCountY;
			volume.CountZ = probe.Volume.ProbeCountZ;
			volume.Size = probe.Volume.Size;
			for (const uint32_t count : { volume.CountX, volume.CountY, volume.CountZ })
			{
				if (count < MinIrradianceProbesPerAxis || count > MaxIrradianceProbesPerAxis)
				{
					return MakeError(ErrorCode::Validation, "the IrradianceVolume of {} has {} x {} x {} probes; a volume bakes {} to {} along each axis",
						probe.Entity.ToString(), volume.CountX, volume.CountY, volume.CountZ, MinIrradianceProbesPerAxis, MaxIrradianceProbesPerAxis);
				}
			}
			volume.Probes.resize(static_cast<size_t>(volume.CountX) * volume.CountY * volume.CountZ);
			ENGINE_TRY(WithContext(ValidateIrradianceVolumeData(volume), std::format("baking the IrradianceVolume of {}", probe.Entity.ToString())));
			return volume;
		}

		// The bake's check of `probe` before anything renders (decision 8: a probe out of its ranges, which project.validate
		// reports as COMPONENT_FIELD_OUT_OF_RANGE, refuses the whole bake): an IrradianceVolume's grid (MakeVolumeGrid), a
		// ReflectionProbe's Resolution a power of two in [MinReflectionProbeResolution, MaxReflectionProbeResolution]. Errors:
		// Validation naming the entity.
		[[nodiscard]] static Status ValidatePendingProbe(const PendingProbe& probe)
		{
			if (probe.IsVolume)
			{
				ENGINE_TRY(MakeVolumeGrid(probe));
				return {};
			}
			const uint32_t resolution = probe.Reflection.Resolution;
			if (resolution < MinReflectionProbeResolution || resolution > MaxReflectionProbeResolution || (resolution & (resolution - 1)) != 0)
			{
				return MakeError(ErrorCode::Validation, "the ReflectionProbe of {} has the Resolution {}; a probe bakes at a power of two from {} to {}",
					probe.Entity.ToString(), resolution, MinReflectionProbeResolution, MaxReflectionProbeResolution);
			}
			return {};
		}

		// The folder of the bake of the scene saved at `scenePath`: Assets/Lighting/ followed by the scene's project path below
		// Assets/ without its extension (Assets/Scenes/Room.scene bakes into Assets/Lighting/Scenes/Room/), so two scenes of one
		// name in different folders never share a file, even when a copy kept the entities' ids (decision 31's amendment of
		// decision 8; a path, as terrain.edit names its splat maps). Errors: those of the path rules (Validation).
		[[nodiscard]] static Result<VfsPath> GetBakeFolder(const VfsPath& scenePath)
		{
			constexpr std::string_view AssetsPrefix = "Assets/";
			std::string_view relative = scenePath.GetPath();
			if (relative.starts_with(AssetsPrefix))
				relative.remove_prefix(AssetsPrefix.size());
			relative.remove_suffix(scenePath.GetExtension().size());
			return VfsPath::Create("project", std::format("Assets/Lighting/{}", relative));
		}

		// The irradiance volume of `probe` (IrradianceVolumeData.h's grid: the probes span the box of the component's Size
		// centred on the entity, in its axes, its scale included, x fastest), one capture per probe.
		[[nodiscard]] static Result<IrradianceVolumeData> BakeVolume(ProbeCapture& capture, const RenderSnapshot& snapshot, const PendingProbe& probe)
		{
			ENGINE_TRY_ASSIGN(IrradianceVolumeData volume, MakeVolumeGrid(probe));
			const glm::vec3 counts(static_cast<float>(volume.CountX - 1), static_cast<float>(volume.CountY - 1), static_cast<float>(volume.CountZ - 1));
			for (uint32_t z = 0; z < volume.CountZ; ++z)
			{
				for (uint32_t y = 0; y < volume.CountY; ++y)
				{
					for (uint32_t x = 0; x < volume.CountX; ++x)
					{
						const glm::vec3 fraction = glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)) / counts;
						const glm::vec3 local = -0.5f * volume.Size + volume.Size * fraction;
						const glm::vec3 position = glm::vec3(probe.World * glm::vec4(local, 1.0f));
						ENGINE_TRY_ASSIGN(const ProbeCubeCapture cube,
							WithContext(capture.Capture(snapshot, position, IrradianceProbeFaceSize),
								std::format("capturing probe ({}, {}, {}) of the IrradianceVolume of {}", x, y, z, probe.Entity.ToString())));
						volume.Probes[GetIrradianceProbeIndex(volume, x, y, z)].SH9 = ProjectCubeToIrradianceSH9(cube);
					}
				}
			}
			ENGINE_TRY(WithContext(ValidateIrradianceVolumeData(volume), std::format("baking the IrradianceVolume of {}", probe.Entity.ToString())));
			return volume;
		}

		// The reflection probe of `probe`: one capture of its Resolution (ValidatePendingProbe checked it) at the entity's
		// position, as an equirectangular Radiance HDR image.
		[[nodiscard]] static Result<Buffer> BakeReflection(ProbeCapture& capture, const RenderSnapshot& snapshot, const PendingProbe& probe)
		{
			const glm::vec3 position(probe.World[3]);
			const uint32_t resolution = probe.Reflection.Resolution;
			ENGINE_TRY_ASSIGN(const ProbeCubeCapture cube,
				WithContext(capture.Capture(snapshot, position, resolution), std::format("capturing the ReflectionProbe of {}", probe.Entity.ToString())));
			return WriteRadianceHdr(ResampleCubeToEquirect(cube));
		}

		// Writes `probe`'s file where its bytes changed and a .meta for a source the registry does not know (its importer's
		// defaults), then imports it when either was written. Returns the entry.
		[[nodiscard]] static Result<LightingBakeEntry> CommitProbeFile(EditorContext& editor, const PendingProbe& probe)
		{
			EditorAssetManager& assets = editor.GetAssets();
			LightingBakeEntry entry;
			entry.Entity = probe.Entity;
			entry.Path = std::string(probe.Path.GetPath());
			const Result<Buffer> existing = editor.GetVfs().ReadFile(probe.Path);
			entry.Changed = !existing.has_value() || *existing != probe.Bytes;
			if (entry.Changed)
				ENGINE_TRY(WithContext(editor.WriteProjectFile(probe.Path, probe.Bytes), std::format("writing '{}'", entry.Path)));

			bool newMeta = false;
			if (const AssetRecord* record = assets.GetRegistry().FindBySourcePath(probe.Path))
			{
				entry.Asset = record->Metadata.Handle;
			}
			else
			{
				ENGINE_TRY_ASSIGN(const AssetMetadata metadata, assets.CreateMetadata(probe.Path));
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(probe.Path));
				ENGINE_TRY(WithContext(editor.WriteProjectFile(metaPath, AsBytes(SerializeAssetMetadata(metadata))), std::format("writing the .meta of '{}'", entry.Path)));
				entry.Asset = metadata.Handle;
				newMeta = true;
			}
			if (entry.Changed || newMeta)
			{
				const Result<AssetImportOutcome> imported = assets.Reimport(entry.Asset);
				if (!imported.has_value())
					return std::unexpected(Error(imported.error()).WithContext(std::format("importing the baked '{}'", entry.Path)));
			}
			return entry;
		}

	}

	Result<LightingBakeResult> BakeLighting(EditorContext& editor, OpenSceneId scene, const LightingBakeOptions& options)
	{
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "lighting.bake needs an open project");
		if (editor.FindScene(scene) == nullptr)
			return MakeError(ErrorCode::NotFound, "no open scene has the id {}", scene);
		// The bake addresses its scene for its whole stretch: the snapshot, the probes and the undo step are that scene's.
		Scope<EditorSceneTargetScope> target;
		if (editor.GetAddressedSceneId() != scene)
		{
			ENGINE_TRY_ASSIGN(target, EditorSceneTargetScope::Begin(editor, scene));
		}
		// The probes are baked from the edit scene and stored in it; a play session renders and runs the scene it copied.
		if (editor.GetPlay().IsPlaying())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "lighting is baked from the edit scene, not the play scene, and the editor is playing")
					.WithHint("stop playing (play.stop), then bake"));
		}
		const std::optional<VfsPath>& scenePath = editor.GetScenePath();
		if (!scenePath.has_value())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "the scene was never saved, and a bake is written beside its file")
					.WithHint("save the scene first with scene.save {path}"));
		}
		Scene& bakedScene = editor.GetScene();
		// The edit scene's world matrices recomputed (runtime-only components, so its revision does not change), as
		// viewport.screenshot extracts it.
		TransformSystem::Update(bakedScene);
		std::vector<Utils::PendingProbe> probes = Utils::CollectProbes(bakedScene, options);
		// Every probe checked before anything renders: one out of its ranges refuses the bake, nothing rendered or written.
		for (const Utils::PendingProbe& probe : probes)
			ENGINE_TRY(Utils::ValidatePendingProbe(probe));
		ProbeCapture* capture = editor.GetProbeCapture();
		if (capture == nullptr)
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "baking lighting renders the scene, and this editor has no GPU device (--renderer none)")
					.WithHint("bake in a rendering editor: start it without --renderer none"));
		}

		ENGINE_TRY_ASSIGN(const RenderSnapshot snapshot, Utils::ExtractBakeSnapshot(editor, bakedScene));
		ENGINE_TRY_ASSIGN(const VfsPath folder, Utils::GetBakeFolder(*scenePath));

		// Every capture first: a failed one writes nothing.
		for (Utils::PendingProbe& probe : probes)
		{
			const std::string id = probe.Entity.ToString();
			if (probe.IsVolume)
			{
				ENGINE_TRY_ASSIGN(const IrradianceVolumeData volume, Utils::BakeVolume(*capture, snapshot, probe));
				probe.Bytes = WriteIrradianceVolumeSource(volume);
				ENGINE_TRY_ASSIGN(probe.Path, folder.Join(id + ".irradiance"));
			}
			else
			{
				ENGINE_TRY_ASSIGN(probe.Bytes, Utils::BakeReflection(*capture, snapshot, probe));
				ENGINE_TRY_ASSIGN(probe.Path, folder.Join(id + ".hdr"));
			}
		}

		// The files, their .meta files and their imports, in canonical order; the probes' Data last.
		if (!probes.empty())
			ENGINE_TRY(WithContext(editor.CreateProjectDirectory(folder), std::format("creating '{}'", folder.GetPath())));
		LightingBakeResult result;
		for (const Utils::PendingProbe& probe : probes)
		{
			ENGINE_TRY_ASSIGN(LightingBakeEntry entry, Utils::CommitProbeFile(editor, probe));
			(probe.IsVolume ? result.IrradianceVolumes : result.ReflectionProbes).push_back(std::move(entry));
		}
		editor.GetAssets().WaitIdle();

		SceneEdit edit(editor, "Bake Lighting");
		bool assigned = false;
		size_t volumeIndex = 0;
		size_t reflectionIndex = 0;
		for (const Utils::PendingProbe& probe : probes)
		{
			const Entity entity = bakedScene.FindEntityByID(probe.Entity);
			if (probe.IsVolume)
			{
				const AssetHandle data = result.IrradianceVolumes[volumeIndex++].Asset;
				if (entity.GetComponent<IrradianceVolumeComponent>().Data.GetHandle() != data)
				{
					entity.Patch<IrradianceVolumeComponent>([data](IrradianceVolumeComponent& volume)
					{
						volume.Data = TypedAssetHandle<AssetType::IrradianceVolume>(data);
					});
					assigned = true;
				}
			}
			else
			{
				const AssetHandle data = result.ReflectionProbes[reflectionIndex++].Asset;
				if (entity.GetComponent<ReflectionProbeComponent>().Data.GetHandle() != data)
				{
					entity.Patch<ReflectionProbeComponent>([data](ReflectionProbeComponent& reflection)
					{
						reflection.Data = TypedAssetHandle<AssetType::Environment>(data);
					});
					assigned = true;
				}
			}
		}
		// A bake that changes no component records no undo step.
		if (assigned)
		{
			ENGINE_TRY(edit.Commit());
		}
		else
		{
			edit.Cancel();
		}
		ENGINE_INFO("Baked the lighting of '{}': {} irradiance volume(s), {} reflection probe(s)", scenePath->GetPath(), result.IrradianceVolumes.size(),
			result.ReflectionProbes.size());
		return result;
	}

}
