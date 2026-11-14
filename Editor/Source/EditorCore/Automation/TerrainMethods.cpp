#include "EditorPCH.h"
#include "EditorCore/Automation/TerrainMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Importers/HeightfieldImporter.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// terrain.edit and terrain.importHeightmap (Docs/Decisions/0037-m26-contract.md decisions 20 and 23). The wire types and the
// registration are the contract's; terrain.importHeightmap's handler is stream T-A's, terrain.edit's stream T-E's.

namespace Engine {

	namespace Utils {

		// Whether `text` equals `lower` (lowercase ASCII) ignoring ASCII case.
		[[nodiscard]] static bool EqualsIgnoringAsciiCase(std::string_view text, std::string_view lower)
		{
			return std::ranges::equal(text, lower, [](char left, char right)
			{
				const char folded = left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
				return folded == right;
			});
		}

		// The heightmap kind of terrain.importHeightmap's source by its extension (ASCII case ignored): .png, or .raw and .r16.
		// Errors: InvalidArgument at /source for another extension.
		[[nodiscard]] static Result<HeightmapSourceKind> FindHeightmapSourceKind(std::string_view source)
		{
			const std::string extension = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(source).extension());
			if (EqualsIgnoringAsciiCase(extension, ".png"))
				return HeightmapSourceKind::Png16;
			if (EqualsIgnoringAsciiCase(extension, ".raw") || EqualsIgnoringAsciiCase(extension, ".r16"))
				return HeightmapSourceKind::Raw16;
			return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/source",
				std::format("'{}' is not a heightmap: terrain.importHeightmap reads a 16-bit .png, or a .raw or .r16 of 16-bit samples", source),
				"export the heightmap as a 16-bit greyscale PNG or as little-endian 16-bit RAW samples"));
		}

		// The bytes of terrain.importHeightmap's source: a native file for an absolute path (read-only), else a project path.
		// Errors: InvalidArgument at /source for an empty path; NotFound at /source for a missing file; ResolveProjectPath's;
		// the read's (Io).
		[[nodiscard]] static Result<Buffer> ReadHeightmapSource(const EditorMethodContext& context, std::string_view source)
		{
			if (source.empty())
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/source", "the source must not be empty"));
			const std::filesystem::path native = FileSystem::PathFromUtf8(source);
			std::optional<Result<Buffer>> bytes;
			if (source.find("://") == std::string_view::npos && native.is_absolute())
			{
				bytes.emplace(FileSystem::ReadFile(native));
			}
			else
			{
				ENGINE_TRY_ASSIGN(const VfsPath path, context.ResolveProjectPath(source, "/source"));
				bytes.emplace(context.GetEditor().GetVfs().ReadFile(path));
			}
			if (bytes->has_value())
				return std::move(*bytes);
			if (bytes->error().GetCode() == ErrorCode::NotFound)
				return std::unexpected(MakeParamError(ErrorCode::NotFound, "/source", std::format("'{}' does not exist", source)));
			return std::unexpected(ToEditorFileError(std::move(*bytes).error()));
		}

	}

	namespace Automation {

		Result<TerrainEditMethodResult> TerrainEditMethod(EditorMethodContext& context, const TerrainEditParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveRequestScene());
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			const EntitySummary summary = context.MakeEntitySummary(entity);

			// The world points in the terrain's local space (the entity's world matrix as its transform gives it now), their
			// local Y dropped: a point picked on the surface, as the editor's terrain brush picks it, works as is.
			const glm::mat4 toLocal = glm::affineInverse(TransformSystem::ComputeWorldMatrix(entity));
			TerrainStroke stroke{ .Tool = params.Tool, .Points = {}, .Radius = params.Radius, .Strength = params.Strength, .Height = params.Height, .Layer = params.Layer };
			stroke.Points.reserve(params.Points.size());
			for (size_t index = 0; index < params.Points.size(); ++index)
			{
				const glm::vec3 point = params.Points[index];
				if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/points/{}", index),
						std::format("point {} of the stroke is not finite", index)));
				}
				const glm::vec4 local = toLocal * glm::vec4(point, 1.0f);
				if (!std::isfinite(local.x) || !std::isfinite(local.z))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/entity",
						std::format("the transform of '{}' is not invertible, so no point reaches its terrain's local space", summary.Path),
						"give the entity's Transform a scale without zeros"));
				}
				stroke.Points.emplace_back(local.x, local.z);
			}

			Result<TerrainEditResult> edited = EditTerrain(editor, editor.GetAddressedSceneId(), entity.GetUUID(), stroke);
			if (!edited.has_value())
			{
				// EditTerrain locates a stroke's refusal at its param; what else it refuses with InvalidArgument or NotFound
				// concerns the entity's Terrain (no Terrain, a missing heightfield, a splat map it cannot paint).
				const Error& error = edited.error();
				const ErrorCode code = error.GetCode();
				if (!error.GetLocation().JsonPointer.has_value() && (code == ErrorCode::InvalidArgument || code == ErrorCode::NotFound))
					return std::unexpected(Utils::LocateAtParam(error, "/entity"));
				return std::unexpected(std::move(edited).error());
			}
			return TerrainEditMethodResult{ .Entity = summary, .Path = edited->Path, .CreatedSplatMap = edited->CreatedSplatMap, .Changed = edited->Changed };
		}

		Result<TerrainImportHeightmapResult> TerrainImportHeightmapMethod(EditorMethodContext& context, const TerrainImportHeightmapParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", ".heightfield"));
			ENGINE_TRY_ASSIGN(const HeightmapSourceKind kind, Utils::FindHeightmapSourceKind(params.Source));
			ENGINE_TRY_ASSIGN(const Buffer heightmap, Utils::ReadHeightmapSource(context, params.Source));
			Result<HeightfieldData> heightfield = ConvertHeightmap(heightmap, kind);
			if (!heightfield.has_value())
				return std::unexpected(Utils::LocateAtParam(std::move(heightfield).error().WithContext(std::format("converting '{}'", params.Source)), "/source"));
			const uint32_t resolution = heightfield->Resolution;
			Buffer source = WriteHeightfieldSource(*heightfield);
			ENGINE_TRY(Utils::RefreshAssets(editor));

			// A heightfield already at the path keeps its .meta, so its handle and every Terrain that names it stay; a new one
			// gets the .meta of the importer that takes .heightfield.
			EditorAssetManager& assets = editor.GetAssets();
			VirtualFileSystem& vfs = editor.GetVfs();
			const std::string relative = Utils::ToProjectRelative(path);
			std::optional<AssetHandle> handle = assets.Resolve(relative);
			if (handle.has_value() && assets.GetAssetType(*handle) != AssetType::Heightfield)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/path",
					std::format("'{}' is registered as a {}, not a Heightfield", relative, AssetTypeToString(assets.GetAssetType(*handle))),
					"choose another path, or delete the asset first (asset.delete)"));
			}
			std::vector<AssetFileEdit> edits;
			if (handle.has_value())
			{
				Result<Buffer> current = vfs.ReadFile(path);
				if (!current.has_value())
					return std::unexpected(Utils::ToEditorFileError(std::move(current).error()));
				if (*current != source)
					edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::File, .Before = std::move(*current), .After = std::move(source) });
			}
			else
			{
				ENGINE_TRY(Utils::CheckAssetPathFree(vfs, path, "/path"));
				ENGINE_TRY_ASSIGN(const AssetMetadata metadata, assets.CreateMetadata(path));
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
				const std::string metaText = SerializeAssetMetadata(metadata);
				const std::span<const std::byte> metaBytes = std::as_bytes(std::span(metaText));
				Utils::AddMissingDirectoryEdits(vfs, path.GetParent(), edits);
				edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = std::move(source) });
				edits.push_back(
					AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = Buffer(metaBytes.begin(), metaBytes.end()) });
				handle = metadata.Handle;
			}
			if (!edits.empty())
			{
				// One AssetEditCommand: one undo step (the new files removed, or the replaced bytes restored), written through
				// EditorContext's write path (provenance, the registry update).
				ENGINE_TRY(editor.Execute(CreateScope<AssetEditCommand>(std::format("Import Heightmap '{}'", path.GetStem()), std::move(edits))));
			}
			// The import now, so its failure is this call's (ImportFailed) and the result names an imported heightfield; while a
			// running session defers reloads (§7.5 race rule 4), the change is published when the deferral ends.
			if (!assets.AreReloadsDeferred())
			{
				Result<AssetImportOutcome> imported = assets.Reimport(*handle);
				if (!imported.has_value())
					return std::unexpected(Utils::LocateAtParam(imported.error(), "/path"));
			}
			ENGINE_INFO("terrain.importHeightmap converted '{}' ({} samples a side) into '{}'", params.Source, resolution, relative);
			return TerrainImportHeightmapResult{ .Asset = Utils::MakeAssetSummary(assets, *handle), .Resolution = resolution };
		}

	}

	void RegisterTerrainMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<TerrainTool>("TerrainTool", "What a terrain stroke does.")
			.Entry(TerrainTool::Raise, "Raise", "Raises the surface under the brush by Strength times the falloff, in metres.")
			.Entry(TerrainTool::Lower, "Lower", "Lowers the surface under the brush by Strength times the falloff, in metres.")
			.Entry(TerrainTool::Smooth, "Smooth", "Moves each sample under the brush towards the mean of its neighbours.")
			.Entry(TerrainTool::Flatten, "Flatten", "Moves the surface under the brush towards Height.")
			.Entry(TerrainTool::Paint, "Paint", "Paints Layer's weight into the splat map under the brush, keeping the weights normalized.");
		registry.Struct<TerrainEditParams>("TerrainEditParams", "The params of terrain.edit.")
			.Field("entity", &TerrainEditParams::Entity, "The entity whose Terrain to edit: its id, a unique id prefix or its path.")
			.Field("tool", &TerrainEditParams::Tool, "The stroke's tool.")
			.Field("points", &TerrainEditParams::Points,
				"The brush's centres in world space, 1 to 4,096 in stroke order; their height above the terrain is ignored.", { .Unit = "m" })
			.Field("radius", &TerrainEditParams::Radius, "The brush's radius in metres in the terrain's local space.", { .Min = 0.001, .Max = 1000.0, .Unit = "m" })
			.Field("strength", &TerrainEditParams::Strength, "The stroke's strength, from 0 to 1.", { .Min = 0.0, .Max = 1.0 })
			.Field("height", &TerrainEditParams::Height, "Flatten's target height above the terrain's origin, in metres.", { .Min = 0.0, .Unit = "m" })
			.Field("layer", &TerrainEditParams::Layer, "Paint's layer, 0 to 3 (the splat map's red, green, blue or alpha).", { .Min = 0.0, .Max = 3.0 });
		registry.Struct<TerrainEditMethodResult>("TerrainEditMethodResult", "What terrain.edit wrote.")
			.Field("entity", &TerrainEditMethodResult::Entity, "The edited terrain's entity.")
			.Field("path", &TerrainEditMethodResult::Path, "The rewritten source, project-relative: the heightfield's, or the splat map's PNG.")
			.Field("createdSplatMap", &TerrainEditMethodResult::CreatedSplatMap, "Whether the paint made and assigned the terrain's splat map.")
			.Field("changed", &TerrainEditMethodResult::Changed, "The samples or texels whose value changed.");
		registry.Struct<TerrainImportHeightmapParams>("TerrainImportHeightmapParams", "The params of terrain.importHeightmap.")
			.Field("source", &TerrainImportHeightmapParams::Source, "The heightmap: a 16-bit .png, or a .raw or .r16 of little-endian 16-bit samples.")
			.Field("path", &TerrainImportHeightmapParams::Path, "The .heightfield to write, project-relative below Assets/.");
		registry.Struct<TerrainImportHeightmapResult>("TerrainImportHeightmapResult", "The heightfield terrain.importHeightmap wrote.")
			.Field("asset", &TerrainImportHeightmapResult::Asset, "The imported heightfield.")
			.Field("resolution", &TerrainImportHeightmapResult::Resolution, "Its samples along each side.");
	}

	void RegisterTerrainMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "terrain.edit",
				.Description = "Sculpts (Raise, Lower, Smooth, Flatten) or paints (Paint) an edit scene's Terrain with one brush stroke, exactly as "
							   "the editor's terrain tools do: rewrites the heightfield's or splat map's source and records one undo step.",
				.RequiredParams = { "entity", "points" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsSceneTarget = true,
				.Examples = { { .Description = "Raise a hill at the terrain's middle.",
					.Params = { { "entity", "/Terrain" }, { "tool", "Raise" }, { "points", Json::array({ Json::array({ 32, 0, 32 }) }) }, { "radius", 8 },
						{ "strength", 0.5 } } } },
			},
			&Automation::TerrainEditMethod);
		methods.Add(
			{
				.Name = "terrain.importHeightmap",
				.Description = "Converts a 16-bit PNG or RAW heightmap (at most 1025 x 1025 samples) into a .heightfield asset that a Terrain can use.",
				.RequiredParams = { "source", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.Examples = { { .Description = "Import a heightmap beside the scenes.",
					.Params = { { "source", "Heightmaps/Valley.png" }, { "path", "Assets/Terrain/Valley.heightfield" } } } },
			},
			&Automation::TerrainImportHeightmapMethod);
	}

}
