#include "EditorPCH.h"
#include "EditorCore/Terrain/TerrainEditing.h"

#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Terrain sculpting and painting (TerrainEditing.h; Docs/Decisions/0037-m26-contract.md decision 23), stream T-E. The
// brushes compute in double precision with IEEE-exact operations (+, -, *, /, floor) in one fixed order, and round every
// result half to even without reading the floating-point environment, so a stroke gives the same bytes in every
// configuration. EditTerrain writes through one EditorTransaction: the source's AssetEditCommand, its synchronous reimport
// and, when a paint made the splat map, the SceneEdit that assigns it; a failure before the commit rolls all of it back.

namespace Engine {

	namespace Utils {

		// The stroke's limits (TerrainStroke's comments; terrain.edit's registry ranges).
		constexpr size_t MaxTerrainStrokePoints = 4096;
		constexpr float MaxTerrainBrushRadius = 1000.0f;
		// The sum of a splat texel's four weights.
		constexpr uint32_t SplatWeightSum = 255;
		// The bytes of one RGBA8 splat texel.
		constexpr size_t SplatTexelBytes = 4;
		// The most `-<n>` suffixes the first paint tries for a free splat-map name.
		constexpr uint32_t MaxSplatNameSuffix = 10000;

		// `value` rounded to the nearest integer, ties to even, whatever the rounding mode (finite values only).
		[[nodiscard]] static double RoundHalfToEven(double value)
		{
			const double floored = std::floor(value);
			const double fraction = value - floored;
			if (fraction > 0.5)
				return floored + 1.0;
			if (fraction < 0.5)
				return floored;
			return std::fmod(floored, 2.0) == 0.0 ? floored : floored + 1.0;
		}

		// `value` rounded half to even and clamped to [0, maximum].
		[[nodiscard]] static uint32_t RoundAndClamp(double value, uint32_t maximum)
		{
			return static_cast<uint32_t>(std::clamp(RoundHalfToEven(value), 0.0, static_cast<double>(maximum)));
		}

		// The brush's falloff at the squared distance `distanceSquared` from a dab of squared radius `radiusSquared`:
		// (1 - (d / r)^2)^2 inside the brush, 0 at and beyond its edge.
		[[nodiscard]] static double GetBrushWeight(double distanceSquared, double radiusSquared)
		{
			if (!(distanceSquared < radiusSquared))
				return 0.0;
			const double inside = 1.0 - distanceSquared / radiusSquared;
			return inside * inside;
		}

		// The inclusive range of grid indices [first, last] whose coordinate index * spacing + offset may lie within `radius`
		// of `centre` (one index of margin each side, so rounding never drops a point the weight would reach), clamped to
		// [0, count - 1]; nullopt when the range is empty.
		[[nodiscard]] static std::optional<std::pair<uint32_t, uint32_t>> GetBrushIndexRange(double centre, double radius, double spacing, double offset,
			uint32_t count)
		{
			const double first = std::floor((centre - radius - offset) / spacing) - 1.0;
			const double last = std::floor((centre + radius - offset) / spacing) + 1.0;
			const double maximum = static_cast<double>(count - 1);
			if (!(last >= 0.0) || !(first <= maximum))
				return std::nullopt;
			return std::pair{ static_cast<uint32_t>(std::max(first, 0.0)), static_cast<uint32_t>(std::min(last, maximum)) };
		}

		// The value of `tool` for one sample: `current` moved by the dab's `amount` (Strength times the falloff) in samples.
		[[nodiscard]] static double SculptSample(TerrainTool tool, double current, double amount, double samplesPerMetre, double target)
		{
			switch (tool)
			{
				case TerrainTool::Raise:   return current + amount * samplesPerMetre;
				case TerrainTool::Lower:   return current - amount * samplesPerMetre;
				case TerrainTool::Flatten:
				case TerrainTool::Smooth:  return current + amount * (target - current);
				case TerrainTool::Paint:   break;
			}
			ENGINE_CORE_ASSERT(false, "Paint is not a sculpting tool");
			return current;
		}

		// Moves texel `texel`'s weight of `layer` towards SplatWeightSum by `amount` of its distance and rescales the other
		// three so the four sum to SplatWeightSum (largest remainders, ties to the lowest channel); with the other three all
		// zero the layer takes the whole sum.
		static void PaintTexel(std::span<uint8_t, SplatTexelBytes> texel, uint32_t layer, double amount)
		{
			const uint32_t current = texel[layer];
			uint32_t others = 0;
			for (uint32_t channel = 0; channel < SplatTexelBytes; ++channel)
			{
				if (channel != layer)
					others += texel[channel];
			}
			if (others == 0)
			{
				texel[layer] = static_cast<uint8_t>(SplatWeightSum);
				return;
			}
			const uint32_t painted = RoundAndClamp(static_cast<double>(current) + amount * static_cast<double>(SplatWeightSum - current), SplatWeightSum);
			const uint32_t rest = SplatWeightSum - painted;
			std::array<uint32_t, SplatTexelBytes> remainders{};
			uint32_t assigned = 0;
			for (uint32_t channel = 0; channel < SplatTexelBytes; ++channel)
			{
				if (channel == layer)
					continue;
				const uint32_t scaled = static_cast<uint32_t>(texel[channel]) * rest;
				texel[channel] = static_cast<uint8_t>(scaled / others);
				remainders[channel] = scaled % others;
				assigned += texel[channel];
			}
			texel[layer] = static_cast<uint8_t>(painted);
			// Fewer than three units remain: one each to the channels of the largest remainders.
			for (uint32_t deficit = rest - assigned; deficit > 0; --deficit)
			{
				uint32_t best = SplatTexelBytes;
				for (uint32_t channel = 0; channel < SplatTexelBytes; ++channel)
				{
					if (channel != layer && (best == SplatTexelBytes || remainders[channel] > remainders[best]))
						best = channel;
				}
				texel[best] = static_cast<uint8_t>(texel[best] + 1);
				remainders[best] = 0;
			}
		}

		// Checks `stroke` for a terrain of `extent`: InvalidArgument located at the terrain.edit param the member is (the
		// stroke's members are that method's params, so its handler passes the location on).
		[[nodiscard]] static Status ValidateTerrainStroke(const TerrainStroke& stroke, const TerrainExtent& extent)
		{
			const auto refuse = [](std::string_view pointer, std::string message)
			{
				ErrorLocation location;
				location.JsonPointer = std::string(pointer);
				return std::unexpected(Error(ErrorCode::InvalidArgument, std::move(message)).WithLocation(std::move(location)));
			};
			if (stroke.Points.empty() || stroke.Points.size() > MaxTerrainStrokePoints)
				return refuse("/points", std::format("a terrain stroke holds 1 to {} points, not {}", MaxTerrainStrokePoints, stroke.Points.size()));
			for (size_t index = 0; index < stroke.Points.size(); ++index)
			{
				if (!std::isfinite(stroke.Points[index].x) || !std::isfinite(stroke.Points[index].y))
					return refuse(std::format("/points/{}", index), std::format("point {} of the stroke is not finite", index));
			}
			if (!(stroke.Radius > 0.0f && stroke.Radius <= MaxTerrainBrushRadius))
				return refuse("/radius", std::format("the brush's radius is {} m; it must be above 0 and at most {} m", stroke.Radius, MaxTerrainBrushRadius));
			if (!(stroke.Strength >= 0.0f && stroke.Strength <= 1.0f))
				return refuse("/strength", std::format("the stroke's strength is {}; it must be from 0 to 1", stroke.Strength));
			if (stroke.Tool == TerrainTool::Flatten && !(stroke.Height >= 0.0f && stroke.Height <= extent.HeightScale))
			{
				return refuse("/height",
					std::format("Flatten's height is {} m; it must be from 0 to the terrain's HeightScale, {} m", stroke.Height, extent.HeightScale));
			}
			if (stroke.Tool == TerrainTool::Paint && stroke.Layer >= MaxTerrainLayers)
				return refuse("/layer", std::format("Paint's layer is {}; a terrain has layers 0 to {}", stroke.Layer, MaxTerrainLayers - 1));
			return {};
		}

		// The project source of a main asset of the importer `importer`, or of type `type`: NotFound for a handle the registry
		// does not hold; InvalidArgument for a built-in, a sub-asset or a dependency, or an asset of another type.
		[[nodiscard]] static Result<const AssetRecord*> FindTerrainSource(const EditorAssetManager& assets, AssetHandle handle, AssetType type,
			std::string_view field, std::string_view entityPath)
		{
			if (IsBuiltinAssetHandle(handle) || assets.GetBuiltins().Find(handle) != nullptr)
			{
				return MakeError(ErrorCode::InvalidArgument, "the {} of the Terrain of '{}' is the built-in '{}', which terrain.edit cannot change", field, entityPath,
					assets.GetReferencePath(handle));
			}
			const AssetRecord* record = assets.GetRegistry().Find(handle);
			if (record == nullptr)
			{
				return std::unexpected(Error(ErrorCode::NotFound, std::format("the {} {} of the Terrain of '{}' is not an asset of the project", field, handle.ToString(), entityPath))
						.WithHint("assign one of the project's assets to the field"));
			}
			if (record->Metadata.Kind != AssetMetaKind::Asset || record->Metadata.Handle != handle || record->Metadata.Type != type)
			{
				return MakeError(ErrorCode::InvalidArgument, "the {} of the Terrain of '{}' is '{}', not a {} source", field, entityPath, assets.GetReferencePath(handle),
					AssetTypeToString(type));
			}
			return record;
		}

		// Whether the import settings `settings` of a Texture make a data texture (Usage Linear and Compression None): merging
		// those two members changes nothing. A .meta written before TextureImporter version 2 lacks Compression, which reads
		// as Auto, so it is not one.
		[[nodiscard]] static bool IsDataTexture(const EditorAssetManager& assets, const AssetMetadata& metadata)
		{
			if (metadata.Importer != TextureImporter::Id)
				return false;
			const Result<VariantValue> merged = assets.MergeImportSettings(TextureImporter::Id, metadata.Settings, Json{ { "Usage", "Linear" }, { "Compression", "None" } });
			return merged.has_value() && *merged == metadata.Settings;
		}

		// The first free splat-map path of `entity`'s first paint (TerrainEditing.h): `<stem>.<entity id>.splat.png` beside the
		// heightfield `heightfield`, else `<stem>.<entity id>-<n>.splat.png` from n = 2; free means neither the file nor its
		// .meta exists. Errors: AlreadyExists when MaxSplatNameSuffix names are taken; the path rules' (Validation).
		[[nodiscard]] static Result<VfsPath> FindFreeSplatMapPath(const VirtualFileSystem& vfs, const VfsPath& heightfield, UUID entity)
		{
			const VfsPath folder = heightfield.GetParent();
			const std::string stem = std::format("{}.{}", heightfield.GetStem(), entity.ToString());
			for (uint32_t suffix = 1; suffix <= MaxSplatNameSuffix; ++suffix)
			{
				const std::string name = suffix == 1 ? std::format("{}.splat.png", stem) : std::format("{}-{}.splat.png", stem, suffix);
				ENGINE_TRY_ASSIGN(const VfsPath path, folder.Join(name));
				ENGINE_TRY_ASSIGN(const VfsPath meta, GetMetaPath(path));
				if (!vfs.Exists(path) && !vfs.Exists(meta))
					return path;
			}
			return MakeError(ErrorCode::AlreadyExists, "every splat-map name of the Terrain {} beside '{}' is taken", entity.ToString(), heightfield.GetPath());
		}

		// The project-relative text of a project:// path.
		[[nodiscard]] static std::string ToRelative(const VfsPath& path)
		{
			return std::string(path.GetPath());
		}

		// What a stroke writes: the files of its one AssetEditCommand, the asset to reimport and, for a first paint, the splat
		// map to assign.
		struct TerrainWrite
		{
			std::vector<AssetFileEdit> Edits{};
			AssetHandle Reimported{};
			std::optional<AssetHandle> AssignedSplatMap{};
			TerrainEditResult Result{};
		};

		// The heightfield source of `terrain` read and checked (sculpting, and naming and sizing a first paint's splat map).
		struct TerrainHeightfieldSource
		{
			AssetHandle Handle{};
			VfsPath Path{};
			Buffer Bytes{};
			HeightfieldData Data{};
		};

		[[nodiscard]] static Result<TerrainHeightfieldSource> ReadTerrainHeightfield(EditorContext& editor, const TerrainComponent& terrain,
			std::string_view entityPath)
		{
			const AssetHandle handle = terrain.Heightfield.GetHandle();
			if (!handle.IsValid())
			{
				return std::unexpected(Error(ErrorCode::NotFound, std::format("the Terrain of '{}' has no Heightfield", entityPath))
						.WithHint("assign one: asset.create {type: \"Heightfield\"} makes a flat heightfield, terrain.importHeightmap converts a heightmap"));
			}
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindTerrainSource(editor.GetAssets(), handle, AssetType::Heightfield, "Heightfield", entityPath));
			TerrainHeightfieldSource source{ .Handle = handle, .Path = record->SourcePath };
			Result<Buffer> bytes = editor.GetVfs().ReadFile(source.Path);
			if (!bytes.has_value())
			{
				if (bytes.error().GetCode() == ErrorCode::NotFound)
					return MakeError(ErrorCode::NotFound, "the Heightfield source '{}' of the Terrain of '{}' does not exist", source.Path.GetPath(), entityPath);
				return std::unexpected(ToEditorFileError(std::move(bytes).error()));
			}
			source.Bytes = std::move(*bytes);
			Result<HeightfieldData> data = ReadHeightfieldSource(source.Bytes);
			if (!data.has_value())
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' cannot be read as a heightfield: {}", source.Path.GetPath(), data.error().GetMessageText()))
						.WithHint("project.validate reports what is wrong with the file; reimport or recreate it, then edit again"));
			}
			source.Data = std::move(*data);
			return source;
		}

		// A sculpting stroke's write: the heightfield's new source when a sample changed.
		[[nodiscard]] static Result<TerrainWrite> PlanSculpt(EditorContext& editor, const TerrainComponent& terrain, const TerrainExtent& extent,
			const TerrainStroke& stroke, std::string_view entityPath)
		{
			ENGINE_TRY_ASSIGN(TerrainHeightfieldSource source, ReadTerrainHeightfield(editor, terrain, entityPath));
			HeightfieldData sculpted = source.Data;
			ApplyTerrainSculpt(sculpted, extent, stroke);
			TerrainWrite write;
			write.Result.Path = ToRelative(source.Path);
			for (size_t index = 0; index < sculpted.Heights.size(); ++index)
				write.Result.Changed += sculpted.Heights[index] != source.Data.Heights[index] ? 1U : 0U;
			if (write.Result.Changed == 0)
				return write;
			write.Edits.push_back(
				AssetFileEdit{ .Path = source.Path, .Kind = AssetFileKind::File, .Before = std::move(source.Bytes), .After = WriteHeightfieldSource(sculpted) });
			write.Reimported = source.Handle;
			return write;
		}

		// A painting stroke's write: the splat map's new PNG when a texel changed, or, for a terrain without one, a new splat
		// map with its .meta (a data texture), assigned in the same step.
		[[nodiscard]] static Result<TerrainWrite> PlanPaint(EditorContext& editor, ConstEntity entity, const TerrainComponent& terrain,
			const TerrainExtent& extent, const TerrainStroke& stroke, std::string_view entityPath)
		{
			EditorAssetManager& assets = editor.GetAssets();
			TerrainWrite write;
			TerrainSplatImage splat;
			TerrainSplatImage before;
			std::optional<Buffer> beforeBytes;
			VfsPath path;
			std::optional<AssetMetadata> created;
			if (const AssetHandle handle = terrain.SplatMap.GetHandle(); handle.IsValid())
			{
				ENGINE_TRY_ASSIGN(const AssetRecord* record, FindTerrainSource(assets, handle, AssetType::Texture, "SplatMap", entityPath));
				if (!IsDataTexture(assets, record->Metadata))
				{
					return std::unexpected(Error(ErrorCode::InvalidArgument,
						std::format("the SplatMap '{}' of the Terrain of '{}' is not imported as a data texture (Usage Linear, Compression None), so terrain.edit "
									"cannot paint it (TERRAIN_SPLAT_NOT_RAW)",
							record->SourcePath.GetPath(), entityPath))
							.WithHint("set its import settings with asset.setImportSettings {settings: {Usage: \"Linear\", Compression: \"None\"}}"));
				}
				if (record->SourcePath.GetExtension() != ".png")
				{
					return std::unexpected(
						Error(ErrorCode::InvalidArgument, std::format("the SplatMap '{}' of the Terrain of '{}' is not a .png, the splat maps terrain.edit paints", record->SourcePath.GetPath(), entityPath))
							.WithHint("clear the Terrain's SplatMap, and the first paint makes a .png splat map"));
				}
				path = record->SourcePath;
				Result<Buffer> bytes = editor.GetVfs().ReadFile(path);
				if (!bytes.has_value())
				{
					if (bytes.error().GetCode() == ErrorCode::NotFound)
						return MakeError(ErrorCode::NotFound, "the SplatMap source '{}' of the Terrain of '{}' does not exist", path.GetPath(), entityPath);
					return std::unexpected(ToEditorFileError(std::move(bytes).error()));
				}
				Result<Image> image = DecodePng(*bytes);
				if (!image.has_value())
				{
					return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' cannot be read as a PNG: {}", path.GetPath(), image.error().GetMessageText()))
							.WithHint("project.validate reports what is wrong with the file; replace it, then paint again"));
				}
				splat.Width = image->Width;
				splat.Height = image->Height;
				splat.Texels.resize(image->Pixels.size());
				std::ranges::transform(image->Pixels, splat.Texels.begin(), [](std::byte value)
				{
					return static_cast<uint8_t>(value);
				});
				beforeBytes = std::move(*bytes);
				write.Reimported = handle;
			}
			else
			{
				// The first paint: a map of the heightfield's cells, layer 0 everywhere, named after the entity.
				ENGINE_TRY_ASSIGN(const TerrainHeightfieldSource heightfield, ReadTerrainHeightfield(editor, terrain, entityPath));
				ENGINE_TRY_ASSIGN(path, FindFreeSplatMapPath(editor.GetVfs(), heightfield.Path, entity.GetUUID()));
				ENGINE_TRY_ASSIGN(AssetMetadata metadata, assets.CreateMetadata(path));
				ENGINE_TRY_ASSIGN(metadata.Settings,
					assets.MergeImportSettings(metadata.Importer, metadata.Settings, Json{ { "Usage", "Linear" }, { "Compression", "None" } }));
				created = std::move(metadata);
				splat.Width = heightfield.Data.Resolution - 1;
				splat.Height = heightfield.Data.Resolution - 1;
				splat.Texels.reserve(static_cast<size_t>(splat.Width) * splat.Height * SplatTexelBytes);
				for (size_t texel = 0; texel < static_cast<size_t>(splat.Width) * splat.Height; ++texel)
					splat.Texels.insert(splat.Texels.end(), { static_cast<uint8_t>(SplatWeightSum), 0, 0, 0 });
				write.Reimported = created->Handle;
				write.AssignedSplatMap = created->Handle;
				write.Result.CreatedSplatMap = true;
			}

			before = splat;
			ApplyTerrainPaint(splat, extent, stroke);
			write.Result.Path = ToRelative(path);
			for (size_t texel = 0; texel < splat.Texels.size(); texel += SplatTexelBytes)
			{
				const bool changed = !std::equal(splat.Texels.begin() + static_cast<ptrdiff_t>(texel), splat.Texels.begin() + static_cast<ptrdiff_t>(texel + SplatTexelBytes),
					before.Texels.begin() + static_cast<ptrdiff_t>(texel));
				write.Result.Changed += changed ? 1U : 0U;
			}
			if (write.Result.Changed == 0 && !created.has_value())
			{
				write.Reimported = AssetHandle();
				return write;
			}
			ENGINE_TRY_ASSIGN(Image image, CreateImage(splat.Width, splat.Height, nvrhi::Format::RGBA8_UNORM));
			std::ranges::transform(splat.Texels, image.Pixels.begin(), [](uint8_t value)
			{
				return static_cast<std::byte>(value);
			});
			ENGINE_TRY_ASSIGN(Buffer png, EncodePng(image));
			write.Edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::File, .Before = std::move(beforeBytes), .After = std::move(png) });
			if (created.has_value())
			{
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
				const std::string metaText = SerializeAssetMetadata(*created);
				const std::span<const std::byte> metaBytes = AsBytes(metaText);
				write.Edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = Buffer(metaBytes.begin(), metaBytes.end()) });
			}
			return write;
		}

	}

	void ApplyTerrainSculpt(HeightfieldData& heightfield, const TerrainExtent& extent, const TerrainStroke& stroke)
	{
		ENGINE_CORE_ASSERT(stroke.Tool != TerrainTool::Paint, "ApplyTerrainSculpt is given a Paint stroke");
		ENGINE_CORE_ASSERT(ValidateHeightfieldData(heightfield).has_value(), "ApplyTerrainSculpt is given an invalid heightfield");
		ENGINE_CORE_ASSERT(extent.Size.x > 0.0f && extent.Size.y > 0.0f && extent.HeightScale > 0.0f, "ApplyTerrainSculpt is given an empty terrain");
		const uint32_t resolution = heightfield.Resolution;
		const double cells = static_cast<double>(resolution - 1);
		const double sizeX = static_cast<double>(extent.Size.x);
		const double sizeZ = static_cast<double>(extent.Size.y);
		const double spacingX = sizeX / cells;
		const double spacingZ = sizeZ / cells;
		const double radius = static_cast<double>(stroke.Radius);
		const double radiusSquared = radius * radius;
		const double strength = static_cast<double>(stroke.Strength);
		const double samplesPerMetre = static_cast<double>(MaxHeightfieldSample) / static_cast<double>(extent.HeightScale);
		const double flattenTarget = std::clamp(static_cast<double>(stroke.Height) * samplesPerMetre, 0.0, static_cast<double>(MaxHeightfieldSample));
		// Smooth's copy of the dab's samples and their neighbours as the previous dab left them, so a dab's own changes never
		// feed its means: the box [firstX, firstX + width) x [firstZ, ...) of the heightfield.
		std::vector<uint16_t> previous;
		uint32_t previousX = 0;
		uint32_t previousZ = 0;
		uint32_t previousWidth = 0;
		const auto neighbour = [resolution](uint32_t index, int32_t offset) -> uint32_t
		{
			if (offset < 0)
				return index == 0 ? 0U : index - 1U;
			if (offset > 0)
				return index + 1U == resolution ? index : index + 1U;
			return index;
		};
		for (const glm::vec2 point : stroke.Points)
		{
			const double centreX = static_cast<double>(point.x);
			const double centreZ = static_cast<double>(point.y);
			const auto rangeX = Utils::GetBrushIndexRange(centreX, radius, spacingX, 0.0, resolution);
			const auto rangeZ = Utils::GetBrushIndexRange(centreZ, radius, spacingZ, 0.0, resolution);
			if (!rangeX.has_value() || !rangeZ.has_value())
				continue;
			if (stroke.Tool == TerrainTool::Smooth)
			{
				previousX = neighbour(rangeX->first, -1);
				previousZ = neighbour(rangeZ->first, -1);
				previousWidth = neighbour(rangeX->second, 1) - previousX + 1;
				const uint32_t previousDepth = neighbour(rangeZ->second, 1) - previousZ + 1;
				previous.resize(static_cast<size_t>(previousWidth) * previousDepth);
				for (uint32_t z = 0; z < previousDepth; ++z)
				{
					const auto row = heightfield.Heights.begin() + static_cast<ptrdiff_t>(previousX + static_cast<size_t>(resolution) * (previousZ + z));
					std::copy(row, row + previousWidth, previous.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(previousWidth) * z));
				}
			}
			for (uint32_t z = rangeZ->first; z <= rangeZ->second; ++z)
			{
				const double distanceZ = static_cast<double>(z) * sizeZ / cells - centreZ;
				for (uint32_t x = rangeX->first; x <= rangeX->second; ++x)
				{
					const double distanceX = static_cast<double>(x) * sizeX / cells - centreX;
					const double weight = Utils::GetBrushWeight(distanceX * distanceX + distanceZ * distanceZ, radiusSquared);
					if (weight == 0.0)
						continue;
					uint16_t& sample = heightfield.Heights[x + static_cast<size_t>(resolution) * z];
					double target = flattenTarget;
					if (stroke.Tool == TerrainTool::Smooth)
					{
						// The mean of the 3 x 3 neighbourhood, its indices clamped at the edges, summed row by row.
						double sum = 0.0;
						for (int32_t offsetZ = -1; offsetZ <= 1; ++offsetZ)
						{
							const uint32_t neighbourZ = neighbour(z, offsetZ) - previousZ;
							for (int32_t offsetX = -1; offsetX <= 1; ++offsetX)
							{
								const uint32_t neighbourX = neighbour(x, offsetX) - previousX;
								sum += static_cast<double>(previous[neighbourX + static_cast<size_t>(previousWidth) * neighbourZ]);
							}
						}
						target = sum / 9.0;
					}
					const double value = Utils::SculptSample(stroke.Tool, static_cast<double>(sample), strength * weight, samplesPerMetre, target);
					sample = static_cast<uint16_t>(Utils::RoundAndClamp(value, MaxHeightfieldSample));
				}
			}
		}
	}

	void ApplyTerrainPaint(TerrainSplatImage& splat, const TerrainExtent& extent, const TerrainStroke& stroke)
	{
		ENGINE_CORE_ASSERT(stroke.Tool == TerrainTool::Paint, "ApplyTerrainPaint is given a sculpting stroke");
		ENGINE_CORE_ASSERT(stroke.Layer < MaxTerrainLayers, "Paint's layer {} is past the terrain's {} layers", stroke.Layer, MaxTerrainLayers);
		ENGINE_CORE_ASSERT(splat.Width > 0 && splat.Height > 0 && splat.Texels.size() == static_cast<size_t>(splat.Width) * splat.Height * Utils::SplatTexelBytes,
			"ApplyTerrainPaint is given a {} x {} splat map of {} bytes", splat.Width, splat.Height, splat.Texels.size());
		ENGINE_CORE_ASSERT(extent.Size.x > 0.0f && extent.Size.y > 0.0f, "ApplyTerrainPaint is given an empty terrain");
		const double sizeX = static_cast<double>(extent.Size.x);
		const double sizeZ = static_cast<double>(extent.Size.y);
		const double width = static_cast<double>(splat.Width);
		const double height = static_cast<double>(splat.Height);
		const double radius = static_cast<double>(stroke.Radius);
		const double radiusSquared = radius * radius;
		const double strength = static_cast<double>(stroke.Strength);
		for (const glm::vec2 point : stroke.Points)
		{
			const double centreX = static_cast<double>(point.x);
			const double centreZ = static_cast<double>(point.y);
			// Texel (x, y) covers ((x + 0.5) Size.x / W, (y + 0.5) Size.y / H).
			const auto rangeX = Utils::GetBrushIndexRange(centreX, radius, sizeX / width, 0.5 * sizeX / width, splat.Width);
			const auto rangeY = Utils::GetBrushIndexRange(centreZ, radius, sizeZ / height, 0.5 * sizeZ / height, splat.Height);
			if (!rangeX.has_value() || !rangeY.has_value())
				continue;
			for (uint32_t y = rangeY->first; y <= rangeY->second; ++y)
			{
				const double distanceZ = (static_cast<double>(y) + 0.5) * sizeZ / height - centreZ;
				for (uint32_t x = rangeX->first; x <= rangeX->second; ++x)
				{
					const double distanceX = (static_cast<double>(x) + 0.5) * sizeX / width - centreX;
					const double weight = Utils::GetBrushWeight(distanceX * distanceX + distanceZ * distanceZ, radiusSquared);
					if (weight == 0.0)
						continue;
					const size_t offset = (x + static_cast<size_t>(splat.Width) * y) * Utils::SplatTexelBytes;
					Utils::PaintTexel(std::span<uint8_t, Utils::SplatTexelBytes>(splat.Texels.data() + offset, Utils::SplatTexelBytes), stroke.Layer, strength * weight);
				}
			}
		}
	}

	Result<TerrainEditResult> EditTerrain(EditorContext& editor, OpenSceneId scene, UUID entity, const TerrainStroke& stroke)
	{
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "terrain.edit needs an open project");
		if (editor.FindScene(scene) == nullptr)
			return MakeError(ErrorCode::NotFound, "no open scene has the id {}", scene);
		// The stroke addresses its scene for its whole stretch: the entity, the SceneEdit and the undo step are that scene's.
		Scope<EditorSceneTargetScope> target;
		if (editor.GetAddressedSceneId() != scene)
		{
			ENGINE_TRY_ASSIGN(target, EditorSceneTargetScope::Begin(editor, scene));
		}
		// No run-time edits (a kept limit, Backlog §6): a play session runs the assets the edit would rewrite.
		if (editor.GetPlay().IsPlaying())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "terrains are edited in the edit scene, and the editor is playing")
					.WithHint("stop playing (play.stop), then edit the terrain"));
		}
		Scene& edited = editor.GetScene();
		const Entity found = edited.FindEntityByID(entity);
		if (!found.IsValid())
			return MakeError(ErrorCode::NotFound, "no entity of the scene has the id {}", entity.ToString());
		const std::string entityPath = edited.GetEntityPath(found);
		const TerrainComponent* terrain = found.TryGetComponent<TerrainComponent>();
		if (terrain == nullptr)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("'{}' has no Terrain", entityPath))
					.WithHint("terrain.edit sculpts and paints an entity holding a Terrain component"));
		}
		const TerrainExtent extent{ .Size = terrain->Size, .HeightScale = terrain->HeightScale };
		if (!(extent.Size.x > 0.0f && extent.Size.y > 0.0f && extent.HeightScale > 0.0f) || !std::isfinite(extent.Size.x) || !std::isfinite(extent.Size.y)
			|| !std::isfinite(extent.HeightScale))
		{
			return MakeError(ErrorCode::InvalidArgument, "the Terrain of '{}' has the Size ({}, {}) and the HeightScale {}, which hold no surface", entityPath, extent.Size.x,
				extent.Size.y, extent.HeightScale);
		}
		ENGINE_TRY(Utils::ValidateTerrainStroke(stroke, extent));

		const bool paint = stroke.Tool == TerrainTool::Paint;
		ENGINE_TRY_ASSIGN(Utils::TerrainWrite write,
			paint ? Utils::PlanPaint(editor, found, *terrain, extent, stroke, entityPath) : Utils::PlanSculpt(editor, *terrain, extent, stroke, entityPath));
		if (write.Edits.empty())
			return write.Result;

		// One undo step: the files, their import now (so a failure is this call's and rolls the files back with the
		// transaction), and the new splat map's assignment.
		const std::string label = paint ? "Paint Terrain" : "Sculpt Terrain";
		EditorTransaction transaction(editor, label);
		ENGINE_TRY(editor.Execute(CreateScope<AssetEditCommand>(label, std::move(write.Edits))));
		EditorAssetManager& assets = editor.GetAssets();
		if (!assets.AreReloadsDeferred())
		{
			const Result<AssetImportOutcome> imported = assets.Reimport(write.Reimported);
			if (!imported.has_value())
				return std::unexpected(Error(imported.error()).WithContext(std::format("importing '{}'", write.Result.Path)));
		}
		if (write.AssignedSplatMap.has_value())
		{
			SceneEdit edit(editor, label);
			const AssetHandle splatMap = *write.AssignedSplatMap;
			edited.FindEntityByID(entity).Patch<TerrainComponent>([splatMap](TerrainComponent& component)
			{
				component.SplatMap = TypedAssetHandle<AssetType::Texture>(splatMap);
			});
			ENGINE_TRY(edit.Commit());
		}
		static_cast<void>(transaction.Commit());
		ENGINE_INFO("{} the Terrain of '{}': {} {} changed in '{}'", paint ? "Painted" : "Sculpted", entityPath, write.Result.Changed, paint ? "texel(s)" : "sample(s)",
			write.Result.Path);
		return write.Result;
	}

}
