#include "EditorPCH.h"
#include "EditorCore/Automation/AssetMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/AssetDeleteCommand.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/AssetMoveCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetReference.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/StringTableData.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/Importers/GltfImporter.h"
#include "Engine/AssetPipeline/Importers/MaterialImporter.h"
#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"
#include "Engine/AssetPipeline/Importers/SpriteSheetImporter.h"
#include "Engine/AssetPipeline/Importers/StringTableImporter.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/AssetPipeline/Importers/TilemapImporter.h"
#include "Engine/Audio/SoundSynth.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/MergePatch.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/SpriteValidation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		constexpr uint32_t MaxListLimit = 1000;
		constexpr std::string_view NotNativeHint = "use asset.getImportSettings and asset.setImportSettings for an imported asset";

		Buffer TextToBuffer(std::string_view text)
		{
			const std::span<const std::byte> bytes = AsBytes(text);
			return Buffer(bytes.begin(), bytes.end());
		}

		// What asset.info and asset.setImportSettings need of the importer `importerId` the editor registers: its version and
		// the registry name of its settings struct. The editor registers the built-in importers (RegisterBuiltinImporters);
		// they are stateless, so a registry built here answers exactly as the editor's.
		struct ImporterFacts
		{
			uint32_t Version = 0;
			std::string SettingsTypeName{};
		};

		std::optional<ImporterFacts> FindImporterFacts(std::string_view importerId)
		{
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const IAssetImporter* importer = importers.FindById(importerId);
			if (importer == nullptr)
				return std::nullopt;
			return ImporterFacts{ .Version = importer->GetVersion(), .SettingsTypeName = std::string(importer->GetSettingsTypeName()) };
		}

		// The registered record of the main asset `handle` that a method needing its .meta reads (`what`: "import settings",
		// "properties"). Errors at `pointer`: InvalidArgument for a built-in, a sub-asset or a dependency file; NotFound for
		// an unknown handle.
		Result<const AssetRecord*> FindMainAssetRecord(const EditorAssetManager& assets, AssetHandle handle, std::string_view pointer, std::string_view what)
		{
			const AssetRegistry& registry = assets.GetRegistry();
			if (assets.GetBuiltins().Find(handle) != nullptr || IsBuiltinAssetHandle(handle))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a built-in asset: it has no {}", assets.GetReferencePath(handle), what), "built-in assets cannot change"));
			}
			if (const std::optional<AssetRegistry::Location> location = registry.Locate(handle); location.has_value() && !location->SubAssetKey.empty())
			{
				const std::string source = Utils::ToProjectRelative(location->Record->SourcePath);
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a sub-asset: its source '{}' has the {}", assets.GetReferencePath(handle), source, what),
					std::format("pass \"{}\"", source)));
			}
			const AssetRecord* record = registry.Find(handle);
			if (record == nullptr)
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer, std::format("no asset has the handle {}", handle.ToString())));
			if (record->Metadata.Kind == AssetMetaKind::Dependency)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a dependency file of {}, not an asset", Utils::ToProjectRelative(record->SourcePath),
						Utils::MakeAssetSummary(assets, record->Metadata.Owner).Path),
					"pass the asset that owns it"));
			}
			return record;
		}

		// The registry JSON of `material` as asset.getProperties reports it: every field, without "Format" and "Version".
		Result<Json> MaterialToValues(const MaterialData& material, const TypeRegistry& registry)
		{
			ENGINE_TRY_ASSIGN(Json document, MaterialToJson(material, registry));
			document.erase("Format");
			document.erase("Version");
			return document;
		}

		// The material `document` (a canonical .material document) with the merge patch `values` applied (null resets a field),
		// read strictly through the registry. Enum names are accepted in any ASCII case (§13.4). Errors: InvalidArgument at
		// `pointer` for values that are not an object or that set "Format" or "Version"; Validation located under `pointer`.
		Result<MaterialData> PatchMaterial(const Json& document, const Json& values, const TypeRegistry& registry, std::string_view pointer)
		{
			if (!values.is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the values must be an object of material fields",
					"for example {\"BaseColor\": [0.9, 0.15, 0.15, 1], \"Roughness\": 0.45}"));
			}
			for (const std::string_view key : { std::string_view("Format"), std::string_view("Version") })
			{
				if (values.contains(key))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, key),
						std::format("'{}' is part of the file format, not a material property", key)));
				}
			}
			Json patch = values;
			const StructInfo* type = registry.FindStruct<MaterialData>();
			ENGINE_ASSERT(type != nullptr, "The Material struct is registered by the engine context (RegisterAssetTypes)");
			Utils::CanonicalizeEnumSpellings(patch, type->GetType());
			MaterialLoadReport report;
			Result<MaterialData> material = MaterialFromJson(ApplyMergePatch(document, patch), registry, report, true);
			if (!material)
				return std::unexpected(Utils::PrefixPointers(material.error(), pointer));
			return material;
		}

		// The registry JSON of `sound` as asset.getProperties reports it: every field, without "Format" and "Version".
		Result<Json> SoundEffectToValues(const SoundEffectDescription& sound, const TypeRegistry& registry)
		{
			ENGINE_TRY_ASSIGN(Json document, SoundEffectToJson(sound, registry));
			document.erase("Format");
			document.erase("Version");
			return document;
		}

		// The sound effect `document` (a canonical .sfx document) with the merge patch `values` applied (null resets a field),
		// read strictly through the registry and checked by ValidateSoundEffect (SoundEffectFromJson). Enum names are accepted
		// in any ASCII case (§13.4). The same rule as PatchMaterial: InvalidArgument at `pointer` for values that are not an
		// object or that set "Format" or "Version"; Validation located under `pointer` for every violation of the registry
		// ranges or of ValidateSoundEffect (at least one layer, valid notes).
		Result<SoundEffectDescription> PatchSoundEffect(const Json& document, const Json& values, const TypeRegistry& registry, std::string_view pointer)
		{
			if (!values.is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the values must be an object of sound effect fields",
					"for example {\"Layers\": [{\"Wave\": \"Square\", \"Notes\": [\"C5:0.06\", \"G5:0.1\"]}]}"));
			}
			for (const std::string_view key : { std::string_view("Format"), std::string_view("Version") })
			{
				if (values.contains(key))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, key),
						std::format("'{}' is part of the file format, not a sound effect property", key)));
				}
			}
			Json patch = values;
			const StructInfo* type = registry.FindStruct<SoundEffectDescription>();
			ENGINE_ASSERT(type != nullptr, "The SoundEffect struct is registered by the engine context (RegisterAssetPipelineTypes)");
			Utils::CanonicalizeEnumSpellings(patch, type->GetType());
			SoundEffectLoadReport report;
			Result<SoundEffectDescription> sound = SoundEffectFromJson(ApplyMergePatch(document, patch), registry, report, true);
			if (!sound)
				return std::unexpected(Utils::PrefixPointers(sound.error(), pointer));
			return sound;
		}

		// The registry JSON of `sheet` as asset.getProperties reports it: every field, without "Format" and "Version".
		Result<Json> SpriteSheetToValues(const SpriteSheetData& sheet, const TypeRegistry& registry)
		{
			ENGINE_TRY_ASSIGN(Json document, SpriteSheetToJson(sheet, registry));
			document.erase("Format");
			document.erase("Version");
			return document;
		}

		// The sprite sheet `document` (a canonical .spritesheet document) with the merge patch `values` applied (null resets a
		// field; an array replaces the whole array, RFC 7386), read strictly through the registry and checked by
		// ValidateSpriteSheet (SpriteSheetFromJson). The same rule as PatchMaterial: InvalidArgument at `pointer` for values
		// that are not an object or that set "Format" or "Version"; Validation located under `pointer` for every violation of
		// the registry ranges or of ValidateSpriteSheet.
		Result<SpriteSheetData> PatchSpriteSheet(const Json& document, const Json& values, const TypeRegistry& registry, std::string_view pointer)
		{
			if (!values.is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the values must be an object of sprite sheet fields",
					"for example {\"Frames\": [{\"Name\": \"Idle\", \"Rect\": {\"X\": 0, \"Y\": 0, \"Width\": 16, \"Height\": 16}}]}"));
			}
			for (const std::string_view key : { std::string_view("Format"), std::string_view("Version") })
			{
				if (values.contains(key))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, key),
						std::format("'{}' is part of the file format, not a sprite sheet property", key)));
				}
			}
			SpriteSheetLoadReport report;
			Result<SpriteSheetData> sheet = SpriteSheetFromJson(ApplyMergePatch(document, values), registry, report, true);
			if (!sheet)
				return std::unexpected(Utils::PrefixPointers(sheet.error(), pointer));
			return sheet;
		}

		// Native-asset values (asset.create, asset.setProperties) take a texture field `key` in every spelling of an asset
		// reference (Architecture §7.1: a 16-hex handle, a project path, a sub-asset path or an engine path), as the AssetRef
		// fields of params do (MethodRegistry.h convention 13): a value that is a string but not a handle is rewritten to the
		// handle of the Texture it names; a handle is kept as given (it may name a missing asset). A sprite sheet's Texture
		// (Docs/Decisions/0032-m21-contract.md, W21.1's review) and a Material's maps (Docs/Decisions/0034-m23-contract.md
		// decision 21 and its Landing). Errors, located at `pointer`/`key`: ResolveAssetParam's (InvalidArgument for a malformed
		// reference, NotFound for one that names nothing, the refresh's errors) and InvalidArgument for an asset of another
		// type.
		Status ResolveTextureReference(EditorMethodContext& context, Json& values, std::string_view key, std::string_view pointer)
		{
			if (!values.is_object())
				return {};
			const auto texture = values.find(std::string(key));
			if (texture == values.end() || !texture->is_string())
				return {};
			const Result<std::string> reference = JsonReader(*texture).ReadString();
			if (!reference || UUID::FromString(*reference).has_value())
				return {};
			const std::string location = std::format("{}/{}", pointer, key);
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, *reference, location));
			const AssetType type = context.GetEditor().GetAssets().GetAssetType(handle);
			if (type != AssetType::Texture)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, location,
					std::format("'{}' is a {}, not a Texture", *reference, AssetTypeToString(type)),
					"give a Texture asset; asset.list {\"type\": \"Texture\"} lists them"));
			}
			*texture = handle.ToString();
			return {};
		}

		// A sprite sheet's Texture through ResolveTextureReference.
		Status ResolveSpriteSheetTexture(EditorMethodContext& context, Json& values, std::string_view pointer)
		{
			return ResolveTextureReference(context, values, "Texture", pointer);
		}

		// A Material's five maps through ResolveTextureReference, each located at its own name.
		Status ResolveMaterialTextures(EditorMethodContext& context, Json& values, std::string_view pointer)
		{
			for (const std::string_view key : { "BaseColorMap", "MetallicRoughnessMap", "NormalMap", "OcclusionMap", "EmissiveMap" })
				ENGINE_TRY(ResolveTextureReference(context, values, key, pointer));
			return {};
		}

		// The texture handle a sprite sheet's values name after ResolveSpriteSheetTexture: its Texture when that is a handle,
		// nullopt otherwise (absent or null, or a value the sheet's own validation refuses).
		std::optional<AssetHandle> FindSpriteSheetTexture(const Json& values)
		{
			if (!values.is_object())
				return std::nullopt;
			const auto texture = values.find("Texture");
			if (texture == values.end() || !texture->is_string())
				return std::nullopt;
			const Result<std::string> reference = JsonReader(*texture).ReadString();
			if (!reference)
				return std::nullopt;
			const std::optional<UUID> handle = UUID::FromString(*reference);
			if (!handle.has_value())
				return std::nullopt;
			return AssetHandle(*handle);
		}

		// The registry JSON of `table` as asset.getProperties reports it: every field, without "Format" and "Version".
		Result<Json> StringTableToValues(const StringTableData& table, const TypeRegistry& registry)
		{
			ENGINE_TRY_ASSIGN(Json document, StringTableToJson(table, registry));
			document.erase("Format");
			document.erase("Version");
			return document;
		}

		// The string table `document` (a canonical .stringtable document) with the merge patch `values` applied (null resets a
		// field; an array replaces the whole array, RFC 7386), read strictly through the registry and checked by
		// ValidateStringTable (StringTableFromJson). The same rule as PatchSpriteSheet: InvalidArgument at `pointer` for values
		// that are not an object or that set "Format" or "Version"; Validation located under `pointer` for every violation of
		// ValidateStringTable.
		Result<StringTableData> PatchStringTable(const Json& document, const Json& values, const TypeRegistry& registry, std::string_view pointer)
		{
			if (!values.is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the values must be an object of string table fields",
					"for example {\"Locales\": [\"en\", \"fr\"], \"Entries\": [{\"Key\": \"Menu.Start\", \"Values\": {\"en\": \"Start\", \"fr\": \"Commencer\"}}]}"));
			}
			for (const std::string_view key : { std::string_view("Format"), std::string_view("Version") })
			{
				if (values.contains(key))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, key),
						std::format("'{}' is part of the file format, not a string table property", key)));
				}
			}
			StringTableLoadReport report;
			Result<StringTableData> table = StringTableFromJson(ApplyMergePatch(document, values), registry, report, true);
			if (!table)
				return std::unexpected(Utils::PrefixPointers(table.error(), pointer));
			return table;
		}

		// asset.create's and asset.setProperties' texture check of a sprite sheet (Docs/Decisions/0029-m18-contract.md decision
		// 18): when the sheet's Texture is a registered Texture that loads, every frame's Rect lies inside it. A texture that is
		// missing, of another type or that fails to load is not checked (project.validate and the texture's own diagnostics
		// report it). Errors: Validation with one issue per frame that leaves the texture, at `pointer`/Frames/<i>/Rect.
		Status CheckSpriteSheetTexture(EditorAssetManager& assets, const SpriteSheetData& sheet, std::string_view pointer)
		{
			// The frames project.validate reports as SPRITE_FRAME_OUTSIDE_TEXTURE (Scene/SpriteValidation.h), refused here.
			ValidationContext context{ std::string(pointer) };
			for (const SpriteFrameTextureIssue& frame : FindSpriteFramesOutsideTexture(sheet, assets))
			{
				ValidationIssue issue;
				issue.Severity = DiagnosticSeverity::Error;
				issue.JsonPointer = std::format("{}/Frames/{}/Rect", pointer, frame.Frame);
				issue.Message = std::format("frame {} ends at texel ({}, {}), outside its texture '{}' of {} x {} texels", frame.Frame, frame.Right, frame.Bottom,
					assets.GetReferencePath(sheet.Texture.GetHandle()), frame.TextureWidth, frame.TextureHeight);
				issue.Hint = "X + Width must be at most the texture's width and Y + Height at most its height";
				context.AddIssue(std::move(issue));
			}
			return context.ToStatus("the sprite sheet");
		}

		// The registry JSON of `tilemap` as asset.getProperties reports it: every field, without "Format" and "Version".
		Result<Json> TilemapToValues(const TilemapData& tilemap, const TypeRegistry& registry)
		{
			ENGINE_TRY_ASSIGN(Json document, TilemapToJson(tilemap, registry));
			document.erase("Format");
			document.erase("Version");
			return document;
		}

		// The cells of `tilemap` (valid) laid in chunks of `chunkSize` cells a side, a chunk size ValidateTilemap accepted: every
		// cell keeps its value, and the chunks are canonical, sorted by (Y, X) with none empty (Asset/TilemapData.h), so a cell's
		// value reads back unchanged through GetTilemapCell. PatchTilemap's ChunkSize set without Chunks
		// (Docs/Decisions/0032-m21-contract.md decision 23). Errors: Validation at `pointer`/ChunkSize when the new chunks would
		// store more than MaxTilemapCells cells.
		Result<std::vector<TilemapChunk>> RechunkTilemapCells(const TilemapData& tilemap, uint32_t chunkSize, std::string_view pointer)
		{
			struct PlacedCell
			{
				glm::ivec2 Chunk = glm::ivec2(0);
				uint32_t Index = 0;
				uint32_t Value = 0;
			};
			const int64_t from = tilemap.ChunkSize;
			const int64_t to = chunkSize;
			std::vector<PlacedCell> cells;
			for (const TilemapChunk& chunk : tilemap.Chunks)
			{
				for (size_t index = 0; index < chunk.Tiles.size(); ++index)
				{
					if (chunk.Tiles[index] == 0)
						continue;
					// Every cell of a valid chunk lies within MaxTilemapCellCoordinate, so its coordinates fit in 32 bits.
					const glm::ivec2 cell(static_cast<int32_t>(chunk.X * from + static_cast<int64_t>(index) % from),
						static_cast<int32_t>(chunk.Y * from + static_cast<int64_t>(index) / from));
					const glm::ivec2 coordinate = GetTilemapChunkCoordinate(cell, chunkSize);
					const int64_t row = cell.y - coordinate.y * to;
					const int64_t column = cell.x - coordinate.x * to;
					cells.push_back(PlacedCell{ .Chunk = coordinate, .Index = static_cast<uint32_t>(row * to + column), .Value = chunk.Tiles[index] });
				}
			}
			std::ranges::sort(cells, [](const PlacedCell& left, const PlacedCell& right)
			{
				return std::tie(left.Chunk.y, left.Chunk.x, left.Index) < std::tie(right.Chunk.y, right.Chunk.x, right.Index);
			});
			uint64_t chunkCount = 0;
			for (size_t index = 0; index < cells.size(); ++index)
			{
				if (index == 0 || cells[index].Chunk != cells[index - 1].Chunk)
					++chunkCount;
			}
			const uint64_t cellsPerChunk = static_cast<uint64_t>(chunkSize) * chunkSize;
			if (chunkCount * cellsPerChunk > MaxTilemapCells)
			{
				ValidationContext context{ std::string(pointer) };
				ValidationIssue issue;
				issue.Severity = DiagnosticSeverity::Error;
				issue.JsonPointer = std::format("{}/ChunkSize", pointer);
				issue.Message = std::format("the painted cells in chunks of {} would store {} cells in {} chunks, more than the {} a tilemap may have", chunkSize,
					chunkCount * cellsPerChunk, chunkCount, MaxTilemapCells);
				issue.Hint = "use a smaller chunk size, or split the map";
				context.AddIssue(std::move(issue));
				const Status refused = context.ToStatus("the tilemap");
				return std::unexpected(refused.error());
			}
			std::vector<TilemapChunk> chunks;
			chunks.reserve(static_cast<size_t>(chunkCount));
			for (const PlacedCell& cell : cells)
			{
				if (chunks.empty() || chunks.back().X != cell.Chunk.x || chunks.back().Y != cell.Chunk.y)
					chunks.push_back(TilemapChunk{ .X = cell.Chunk.x, .Y = cell.Chunk.y, .Tiles = std::vector<uint32_t>(static_cast<size_t>(cellsPerChunk), 0) });
				chunks.back().Tiles[cell.Index] = cell.Value;
			}
			return chunks;
		}

		// The tilemap `document` (a canonical .tilemap document) with the merge patch `values` applied (null resets a field; an
		// array such as Palette or Chunks replaces the whole array, RFC 7386), read strictly through the registry and checked by
		// ValidateTilemap (TilemapFromJson). A ChunkSize set without Chunks (a value, or null for the default) keeps the
		// document's cells, laid in chunks of the new size (RechunkTilemapCells), so a painted map's chunk size can be changed
		// alone, as the inspector's field does; Chunks set beside it replace the cells whole, at the chunk size the patch sets.
		// The same rule as PatchMaterial: InvalidArgument at `pointer` for values that are not an object or that set "Format" or
		// "Version"; Validation located under `pointer` for every violation of the registry ranges or of ValidateTilemap, and
		// RechunkTilemapCells' (Docs/Decisions/0032-m21-contract.md decision 23).
		Result<TilemapData> PatchTilemap(const Json& document, const Json& values, const TypeRegistry& registry, std::string_view pointer)
		{
			if (!values.is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the values must be an object of tilemap fields",
					"for example {\"CellSize\": [1, 1], \"Palette\": [{\"Sheet\": \"Assets/Sheets/Tiles.spritesheet\", \"Frame\": 0, \"Solid\": true}]}"));
			}
			for (const std::string_view key : { std::string_view("Format"), std::string_view("Version") })
			{
				if (values.contains(key))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, key),
						std::format("'{}' is part of the file format, not a tilemap property", key)));
				}
			}
			TilemapLoadReport report;
			Json patched = ApplyMergePatch(document, values);
			if (values.contains("ChunkSize") && !values.contains("Chunks"))
			{
				// The patched map without its cells (every field the patch sets, the new ChunkSize's rules included), then with the
				// document's cells at the document's chunk size (the cells against the patched palette), then those cells re-chunked.
				Json withCells = patched;
				if (const auto previous = document.find("ChunkSize"); previous != document.end())
					withCells["ChunkSize"] = *previous;
				else
					withCells.erase("ChunkSize");
				patched["Chunks"] = Json::array();
				Result<TilemapData> tilemap = TilemapFromJson(patched, registry, report, true);
				if (!tilemap)
					return std::unexpected(Utils::PrefixPointers(tilemap.error(), pointer));
				Result<TilemapData> cells = TilemapFromJson(withCells, registry, report, true);
				if (!cells)
					return std::unexpected(Utils::PrefixPointers(cells.error(), pointer));
				if (cells->ChunkSize == tilemap->ChunkSize)
					return cells;
				ENGINE_TRY_ASSIGN(tilemap->Chunks, RechunkTilemapCells(*cells, tilemap->ChunkSize, pointer));
				return tilemap;
			}
			Result<TilemapData> tilemap = TilemapFromJson(patched, registry, report, true);
			if (!tilemap)
				return std::unexpected(Utils::PrefixPointers(tilemap.error(), pointer));
			return tilemap;
		}

		// A tilemap's values (asset.create, asset.setProperties) take each palette entry's Sheet in every spelling of an asset
		// reference (Architecture §7.1), as a sprite sheet's Texture does (ResolveSpriteSheetTexture;
		// Docs/Decisions/0032-m21-contract.md decision 23): a Sheet that is a string but not a handle is rewritten to the handle
		// of the SpriteSheet it names; a handle is kept as given (it may name a missing asset). Values that are not an object, a
		// Palette that is not an array and entries that are not objects are left to PatchTilemap's errors. Errors, located at
		// `pointer`/Palette/<i>/Sheet: ResolveAssetParam's and InvalidArgument for an asset of another type.
		Status ResolveTilemapSheets(EditorMethodContext& context, Json& values, std::string_view pointer)
		{
			if (!values.is_object())
				return {};
			const auto palette = values.find("Palette");
			if (palette == values.end() || !palette->is_array())
				return {};
			for (size_t index = 0; index < palette->size(); ++index)
			{
				Json& entry = (*palette)[index];
				if (!entry.is_object())
					continue;
				const auto sheet = entry.find("Sheet");
				if (sheet == entry.end() || !sheet->is_string())
					continue;
				const Result<std::string> reference = JsonReader(*sheet).ReadString();
				if (!reference || UUID::FromString(*reference).has_value())
					continue;
				const std::string location = std::format("{}/Palette/{}/Sheet", pointer, index);
				ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, *reference, location));
				const AssetType type = context.GetEditor().GetAssets().GetAssetType(handle);
				if (type != AssetType::SpriteSheet)
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, location,
						std::format("'{}' is a {}, not a SpriteSheet", *reference, AssetTypeToString(type)),
						"give a SpriteSheet asset; asset.list {\"type\": \"SpriteSheet\"} lists them"));
				}
				*sheet = handle.ToString();
			}
			return {};
		}

		// The native assets with properties of their own (asset.getProperties, asset.setProperties), by importer.
		enum class NativePropertiesKind : uint8_t
		{
			Material,
			SoundEffect,
			SpriteSheet,
			Tilemap,    // M21 (Docs/Decisions/0032-m21-contract.md decision 23)
			StringTable // M22 (Docs/Decisions/0033-m22-contract.md decision 10)
		};

		std::optional<NativePropertiesKind> FindNativePropertiesKind(const AssetRecord& record)
		{
			if (record.Metadata.Importer == MaterialImporter::Id)
				return NativePropertiesKind::Material;
			if (record.Metadata.Importer == SoundEffectImporter::Id)
				return NativePropertiesKind::SoundEffect;
			if (record.Metadata.Importer == SpriteSheetImporter::Id)
				return NativePropertiesKind::SpriteSheet;
			if (record.Metadata.Importer == TilemapImporter::Id)
				return NativePropertiesKind::Tilemap;
			if (record.Metadata.Importer == StringTableImporter::Id)
				return NativePropertiesKind::StringTable;
			return std::nullopt;
		}

		// What asset.setProperties' undo label calls a native asset of `kind`: "Set <name> '<stem>'".
		std::string_view GetNativePropertiesLabel(NativePropertiesKind kind)
		{
			switch (kind)
			{
				case NativePropertiesKind::Material:    return "Material";
				case NativePropertiesKind::SoundEffect: return "Sound Effect";
				case NativePropertiesKind::SpriteSheet: return "Sprite Sheet";
				case NativePropertiesKind::Tilemap:     return "Tilemap";
				case NativePropertiesKind::StringTable: return "String Table";
			}
			return "Asset";
		}

		// A native asset's properties: the canonical text of its file and the values asset.getProperties reports.
		struct NativeProperties
		{
			std::string Text{};
			Json Values{};
		};

		// The properties of the native asset of `kind` whose file holds `text`, read from `source` (named in errors), with
		// the merge patch `patch` applied when given (a patched sprite sheet's frames are checked against its texture through
		// `assets`). Errors: those of the readers (Parse, Validation, UnsupportedVersion, with "reading '<source>'"), of
		// PatchMaterial, PatchSoundEffect, PatchSpriteSheet, CheckSpriteSheetTexture, PatchTilemap and PatchStringTable (located
		// under /values) and of the writers.
		Result<NativeProperties> ReadNativeProperties(NativePropertiesKind kind, std::string_view text, const VfsPath& source, const TypeRegistry& registry,
			const Json* patch, EditorAssetManager& assets)
		{
			const std::string reading = std::format("reading '{}'", Utils::ToProjectRelative(source));
			NativeProperties properties;
			switch (kind)
			{
				case NativePropertiesKind::Material:
				{
					MaterialLoadReport report;
					ENGINE_TRY_ASSIGN(MaterialData material, WithContext(MaterialFromText(text, registry, report), reading));
					if (patch != nullptr)
					{
						ENGINE_TRY_ASSIGN(const Json document, MaterialToJson(material, registry));
						ENGINE_TRY_ASSIGN(material, PatchMaterial(document, *patch, registry, "/values"));
					}
					ENGINE_TRY_ASSIGN(properties.Text, MaterialToText(material, registry));
					ENGINE_TRY_ASSIGN(properties.Values, MaterialToValues(material, registry));
					return properties;
				}
				case NativePropertiesKind::SoundEffect:
				{
					SoundEffectLoadReport report;
					ENGINE_TRY_ASSIGN(SoundEffectDescription sound, WithContext(SoundEffectFromText(text, registry, report), reading));
					if (patch != nullptr)
					{
						ENGINE_TRY_ASSIGN(const Json document, SoundEffectToJson(sound, registry));
						ENGINE_TRY_ASSIGN(sound, PatchSoundEffect(document, *patch, registry, "/values"));
					}
					ENGINE_TRY_ASSIGN(properties.Text, SoundEffectToText(sound, registry));
					ENGINE_TRY_ASSIGN(properties.Values, SoundEffectToValues(sound, registry));
					return properties;
				}
				case NativePropertiesKind::SpriteSheet:
				{
					SpriteSheetLoadReport report;
					ENGINE_TRY_ASSIGN(SpriteSheetData sheet, WithContext(SpriteSheetFromText(text, registry, report), reading));
					if (patch != nullptr)
					{
						ENGINE_TRY_ASSIGN(const Json document, SpriteSheetToJson(sheet, registry));
						ENGINE_TRY_ASSIGN(sheet, PatchSpriteSheet(document, *patch, registry, "/values"));
						ENGINE_TRY(CheckSpriteSheetTexture(assets, sheet, "/values"));
					}
					ENGINE_TRY_ASSIGN(properties.Text, SpriteSheetToText(sheet, registry));
					ENGINE_TRY_ASSIGN(properties.Values, SpriteSheetToValues(sheet, registry));
					return properties;
				}
				case NativePropertiesKind::Tilemap:
				{
					TilemapLoadReport report;
					ENGINE_TRY_ASSIGN(TilemapData tilemap, WithContext(TilemapFromText(text, registry, report), reading));
					if (patch != nullptr)
					{
						ENGINE_TRY_ASSIGN(const Json document, TilemapToJson(tilemap, registry));
						ENGINE_TRY_ASSIGN(tilemap, PatchTilemap(document, *patch, registry, "/values"));
					}
					ENGINE_TRY_ASSIGN(properties.Text, TilemapToText(tilemap, registry));
					ENGINE_TRY_ASSIGN(properties.Values, TilemapToValues(tilemap, registry));
					return properties;
				}
				case NativePropertiesKind::StringTable:
				{
					StringTableLoadReport report;
					ENGINE_TRY_ASSIGN(StringTableData table, WithContext(StringTableFromText(text, registry, report), reading));
					if (patch != nullptr)
					{
						ENGINE_TRY_ASSIGN(const Json document, StringTableToJson(table, registry));
						ENGINE_TRY_ASSIGN(table, PatchStringTable(document, *patch, registry, "/values"));
					}
					ENGINE_TRY_ASSIGN(properties.Text, StringTableToText(table, registry));
					ENGINE_TRY_ASSIGN(properties.Values, StringTableToValues(table, registry));
					return properties;
				}
			}

			ENGINE_ASSERT(false, "Unknown NativePropertiesKind {}", std::to_underlying(kind));
			return MakeError(ErrorCode::Unsupported, "this asset has no properties of its own");
		}

		// The text of a new native asset of `type` at `path`: a material, a sound effect, a sprite sheet, a tilemap or a string
		// table from `values` over the defaults, an empty scene named after the file stem, or a prefab with one root entity
		// named after it. Errors: those of PatchMaterial, PatchSoundEffect, PatchSpriteSheet, CheckSpriteSheetTexture,
		// PatchTilemap, PatchStringTable and the serializers.
		Result<std::string> MakeNativeAssetText(EditorContext& editor, AssetCreateType type, const VfsPath& path, const VariantValue& values)
		{
			const std::string stem(path.GetStem());
			const TypeRegistry& registry = editor.GetTypeRegistry();
			switch (type)
			{
				case AssetCreateType::Material:
				{
					MaterialData material;
					if (!values.Get().is_null())
					{
						ENGINE_TRY_ASSIGN(const Json defaults, MaterialToJson(material, registry));
						ENGINE_TRY_ASSIGN(material, PatchMaterial(defaults, values.Get(), registry, "/values"));
					}
					return MaterialToText(material, registry);
				}
				case AssetCreateType::Scene:
				{
					const Scope<Scene> scene = editor.CreateScene(stem);
					return SceneSerializer::SaveToString(*scene);
				}
				case AssetCreateType::Prefab:
				{
					const Scope<Scene> scratch = editor.CreateScene(stem);
					const Entity root = scratch->CreateEntity(stem);
					ENGINE_TRY_ASSIGN(const Prefab prefab, Prefab::CreateFromEntity(root, stem));
					return prefab.SaveToString();
				}
				case AssetCreateType::SoundEffect:
				{
					// The defaults have no layer, so a sound effect always needs values (ValidateSoundEffect).
					ENGINE_TRY_ASSIGN(const Json defaults, SoundEffectToJson(SoundEffectDescription{}, registry));
					const Json patch = values.Get().is_null() ? Json::object() : values.Get();
					ENGINE_TRY_ASSIGN(const SoundEffectDescription sound, PatchSoundEffect(defaults, patch, registry, "/values"));
					return SoundEffectToText(sound, registry);
				}
				case AssetCreateType::SpriteSheet:
				{
					// The defaults are an empty sheet (no texture, no frames, no clips), which absent values create.
					SpriteSheetData sheet;
					if (!values.Get().is_null())
					{
						ENGINE_TRY_ASSIGN(const Json defaults, SpriteSheetToJson(sheet, registry));
						ENGINE_TRY_ASSIGN(sheet, PatchSpriteSheet(defaults, values.Get(), registry, "/values"));
					}
					ENGINE_TRY(CheckSpriteSheetTexture(editor.GetAssets(), sheet, "/values"));
					return SpriteSheetToText(sheet, registry);
				}
				case AssetCreateType::Tilemap:
				{
					// The defaults are an empty map of unit cells in 16-cell chunks without a palette, which absent values create.
					TilemapData tilemap;
					if (!values.Get().is_null())
					{
						ENGINE_TRY_ASSIGN(const Json defaults, TilemapToJson(tilemap, registry));
						ENGINE_TRY_ASSIGN(tilemap, PatchTilemap(defaults, values.Get(), registry, "/values"));
					}
					return TilemapToText(tilemap, registry);
				}
				case AssetCreateType::StringTable:
				{
					// The defaults are an empty table in the locale "en", which absent values create.
					StringTableData table;
					if (!values.Get().is_null())
					{
						ENGINE_TRY_ASSIGN(const Json defaults, StringTableToJson(table, registry));
						ENGINE_TRY_ASSIGN(table, PatchStringTable(defaults, values.Get(), registry, "/values"));
					}
					return StringTableToText(table, registry);
				}
				case AssetCreateType::Folder:
				case AssetCreateType::Heightfield:
					break;
			}
			return MakeError(ErrorCode::Unsupported, "assets of this type have no content to create");
		}

		// The samples a side of the flat heightfield asset.create makes (Docs/Decisions/0037-m26-contract.md decision 20): four
		// of the terrain's 32-cell chunks a side.
		constexpr uint32_t CreatedHeightfieldResolution = 129;

		// The bytes of a new native asset of `type` at `path`: a heightfield's binary source (MakeFlatHeightfield of
		// CreatedHeightfieldResolution samples a side, M26), or the text of MakeNativeAssetText. Errors: those of
		// MakeNativeAssetText.
		Result<Buffer> MakeNativeAssetBytes(EditorContext& editor, AssetCreateType type, const VfsPath& path, const VariantValue& values)
		{
			if (type == AssetCreateType::Heightfield)
				return WriteHeightfieldSource(MakeFlatHeightfield(CreatedHeightfieldResolution));
			ENGINE_TRY_ASSIGN(const std::string text, MakeNativeAssetText(editor, type, path, values));
			return TextToBuffer(text);
		}

		// The file extension asset.create requires for `type` (empty for a folder).
		std::string_view GetCreateExtension(AssetCreateType type)
		{
			switch (type)
			{
				case AssetCreateType::Material:    return ".material";
				case AssetCreateType::Scene:       return ".scene";
				case AssetCreateType::Prefab:      return ".prefab";
				case AssetCreateType::SoundEffect: return ".sfx";
				case AssetCreateType::Folder:      return {};
				case AssetCreateType::SpriteSheet: return ".spritesheet";
				case AssetCreateType::Tilemap:     return ".tilemap";
				case AssetCreateType::StringTable: return ".stringtable";
				case AssetCreateType::Heightfield: return ".heightfield";
			}
			return {};
		}

		// The settings patch `settings` spelled canonically for the importer `importerId` (enum names in any ASCII case,
		// §13.4), validated to be an object. Errors: InvalidArgument at `pointer` for a value that is not an object.
		Result<Json> PrepareSettingsPatch(const TypeRegistry& registry, std::string_view importerId, const VariantValue& settings, std::string_view pointer)
		{
			if (!settings.Get().is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, "the settings must be an object (an RFC 7386 merge patch)",
					"for example {\"Usage\": \"NormalMap\"}"));
			}
			Json patch = settings.Get();
			if (const std::optional<ImporterFacts> facts = FindImporterFacts(importerId); facts.has_value() && !facts->SettingsTypeName.empty())
			{
				if (const StructInfo* type = registry.FindStruct(facts->SettingsTypeName); type != nullptr)
					Utils::CanonicalizeEnumSpellings(patch, type->GetType());
			}
			return patch;
		}

		// EditorAssetManager::MergeImportSettings with its errors located under `pointer`.
		Result<VariantValue> MergeSettings(const EditorAssetManager& assets, std::string_view importerId, const VariantValue& base, const Json& patch,
			std::string_view pointer)
		{
			Result<VariantValue> merged = assets.MergeImportSettings(importerId, base, patch);
			if (!merged)
			{
				if (merged.error().GetCode() == ErrorCode::InvalidArgument)
					return std::unexpected(Utils::LocateAtParam(merged.error(), pointer));
				return std::unexpected(Utils::PrefixPointers(merged.error(), pointer));
			}
			return merged;
		}

		// A file to write and its new bytes (AssetEditCommand::CreateForWrites).
		using FileWrite = std::pair<VfsPath, Buffer>;

		// Sprite, tile and UI art is stored uncompressed (ADR 0028's phase-2 ledger, "Texture compression";
		// Docs/Decisions/0034-m23-contract.md decision 13 and its Landing): asset.create and asset.setProperties of a sprite sheet
		// (and so the content browser's create list and the inspector's sheet fields, which go through them) give the sheet's
		// Texture Compression None in the sheet's own undo step, so a texture a sprite sheet references never cooks as BC
		// blocks, which blur hard pixel edges and shift flat colours. Returns the .meta of the texture `texture` with
		// Compression None, to write with the sheet (the write schedules the texture's import), or nullopt when there is
		// nothing to change: a null handle, a built-in texture (never compressed), an asset that is not a registered source of
		// the TextureImporter (missing, another type, or a glTF's texture sub-asset, which follows its source's settings), or
		// a texture already at None. Errors: MergeSettings's, located at `pointer`.
		Result<std::optional<FileWrite>> MakeUncompressedTextureMeta(const EditorAssetManager& assets, AssetHandle texture, std::string_view pointer)
		{
			if (!texture.IsValid() || IsBuiltinAssetHandle(texture) || assets.GetBuiltins().Find(texture) != nullptr)
				return std::nullopt;
			const AssetRecord* record = assets.GetRegistry().Find(texture);
			if (record == nullptr || record->Metadata.Kind == AssetMetaKind::Dependency || record->Metadata.Handle != texture
				|| record->Metadata.Importer != TextureImporter::Id)
				return std::nullopt;
			const Json patch{ { "Compression", "None" } };
			ENGINE_TRY_ASSIGN(VariantValue settings, MergeSettings(assets, record->Metadata.Importer, record->Metadata.Settings, patch, pointer));
			if (settings == record->Metadata.Settings)
				return std::nullopt;
			AssetMetadata changed = record->Metadata;
			changed.Settings = std::move(settings);
			return std::make_optional(FileWrite{ record->MetaPath, TextToBuffer(SerializeAssetMetadata(changed)) });
		}

		// The decimal offset of a list cursor ("" is 0). Errors: InvalidArgument at "/cursor" for anything else, or an offset
		// beyond `total`.
		Result<size_t> ParseListCursor(std::string_view cursor, size_t total)
		{
			if (cursor.empty())
				return size_t{ 0 };
			size_t offset = 0;
			const auto [end, error] = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
			if (error != std::errc() || end != cursor.data() + cursor.size() || offset > total)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/cursor", std::format("'{}' is not a cursor of this listing", cursor),
					"pass \"\" for the start, or the nextCursor of the previous call"));
			}
			return offset;
		}

		// The prefix of an asset.list dir that lists the built-in assets (M17 friction F02).
		constexpr std::string_view EngineDirPrefix = "engine://";

		// asset.list's built-in form (M17 friction F02): the BuiltinAssetCatalog entries below `params.Dir` ("engine://" or a
		// folder below it, such as "engine://Environments"), with type, recursive, limit and cursor as for project assets (a
		// built-in has no sub-assets), sorted by path. Errors: InvalidArgument at /dir for a malformed engine path or one that
		// names a built-in asset; NotFound when no built-in lies below it; ParseListCursor's.
		Result<AssetListResult> ListBuiltinAssets(const EditorAssetManager& assets, const AssetListParams& params)
		{
			const Result<VfsPath> directory = VfsPath::Parse(params.Dir);
			if (!directory)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/dir",
					std::format("'{}' is not a valid engine path: {}", params.Dir, directory.error().GetMessageText()),
					"pass \"engine://\" or a folder below it, such as \"engine://Environments\""));
			}
			const BuiltinAssetCatalog& builtins = assets.GetBuiltins();
			if (builtins.FindByPath(params.Dir) != nullptr)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/dir", std::format("'{}' is a built-in asset, not a folder", params.Dir),
					std::format("asset.info {{asset: \"{}\"}} describes it", params.Dir)));
			}
			bool folderExists = false;
			std::vector<const BuiltinAssetEntry*> matches;
			for (const BuiltinAssetEntry& entry : builtins.GetEntries())
			{
				// The catalogue validated every path when it was parsed (BuiltinAssetCatalog::Parse).
				const Result<VfsPath> path = VfsPath::Parse(entry.Path);
				if (!path || !path->IsUnder(*directory) || *path == *directory)
					continue;
				folderExists = true;
				const bool inside = params.Recursive || path->GetParent() == *directory;
				if (inside && (params.Type == AssetType::None || params.Type == entry.Type))
					matches.push_back(&entry);
			}
			if (!folderExists)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/dir", std::format("'{}' is not a folder of the built-in assets", params.Dir),
					"asset.list {dir: \"engine://\"} lists every built-in asset"));
			}
			std::ranges::sort(matches, {}, &BuiltinAssetEntry::Path);

			ENGINE_TRY_ASSIGN(const size_t offset, ParseListCursor(params.Cursor, matches.size()));
			const size_t end = std::min(matches.size(), offset + params.Limit);
			AssetListResult result;
			result.Total = ToAutomationCounter(matches.size());
			for (size_t index = offset; index < end; ++index)
				result.Assets.push_back(AssetSummary{ .Id = matches[index]->Handle.ToString(), .Path = matches[index]->Path, .Type = matches[index]->Type });
			if (end < matches.size())
				result.NextCursor = std::to_string(end);
			return result;
		}

		// asset.reimport's §5.5 "Update" after `handle`, a Prefab whose new version updates instances (Utils::
		// UpdatesPrefabInstances), got that version from its import: the prefabs nesting it are rewritten from it
		// (EditorContext::CreateNestingPrefabUpdateCommand) and the addressed scene's instances of it and of them rebuilt
		// (CreatePrefabUpdateCommand), as one undo step "Update Prefab Instances" of their own (the import itself is not
		// undoable), and the other open scenes follow in their own histories, after the step, and after its undo and redo
		// when it rewrote a prefab (Docs/Decisions/0031-m20-contract.md decisions 8 and 15). Errors: those of loading the new
		// version, of Utils::FindNestingPrefabs (PREFAB_CYCLE) and of the update; the step rolls back then.
		Status UpdateReimportedPrefab(EditorContext& editor, AssetHandle handle)
		{
			EditorAssetManager& assets = editor.GetAssets();
			LoadReport report;
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			ENGINE_TRY_ASSIGN(const Prefab current, LoadPrefabAsset(assets, handle, editor.GetTypeRegistry(), schemas.get(), report));
			ENGINE_TRY_ASSIGN(std::vector<AssetHandle> rebuilt, Utils::FindNestingPrefabs(editor, handle, current));
			const bool nested = !rebuilt.empty();
			rebuilt.push_back(handle);
			std::ranges::sort(rebuilt);
			const AssetHandle prefabs[] = { handle };
			Scope<Command> rewrite;
			if (nested)
			{
				ENGINE_TRY_ASSIGN(NestingPrefabUpdate nesting, editor.CreateNestingPrefabUpdateCommand(prefabs));
				rewrite = std::move(nesting.Rewrite);
			}
			if (rewrite == nullptr)
			{
				if (editor.HasScene())
				{
					ENGINE_TRY_ASSIGN(Scope<Command> update, editor.CreatePrefabUpdateCommand(rebuilt));
					if (update != nullptr)
						ENGINE_TRY(editor.Execute(std::move(update)));
				}
				editor.PropagatePrefabUpdate(prefabs);
				return {};
			}
			// The rewritten prefabs' files and their restore on undo reach the other open scenes like any prefab write.
			EditorTransaction transaction(editor, "Update Prefab Instances");
			ENGINE_TRY(editor.Execute(Utils::CreatePrefabPropagationCommand({ handle }, true, {})));
			ENGINE_TRY(editor.Execute(std::move(rewrite)));
			if (editor.HasScene())
			{
				ENGINE_TRY_ASSIGN(Scope<Command> update, editor.CreatePrefabUpdateCommand(rebuilt));
				if (update != nullptr)
					ENGINE_TRY(editor.Execute(std::move(update)));
			}
			ENGINE_TRY(editor.Execute(Utils::CreatePrefabPropagationCommand({ handle }, false, {})));
			static_cast<void>(transaction.Commit());
			return {};
		}

		std::vector<AssetSummary> MakeSubAssetSummaries(const EditorAssetManager& assets, std::span<const SubAssetEntry> subAssets)
		{
			std::vector<AssetSummary> summaries;
			summaries.reserve(subAssets.size());
			for (const SubAssetEntry& subAsset : subAssets)
				summaries.push_back(Utils::MakeAssetSummary(assets, subAsset.Handle));
			return summaries;
		}

		std::vector<AssetDiagnosticInfo> MakeDiagnosticInfos(std::span<const AssetDiagnostic> diagnostics)
		{
			std::vector<AssetDiagnosticInfo> infos;
			infos.reserve(diagnostics.size());
			for (const AssetDiagnostic& diagnostic : diagnostics)
				infos.push_back(Utils::MakeAssetDiagnosticInfo(diagnostic));
			return infos;
		}

		// asset.import's operation: the import of the copied asset, resolved once its job completed and its result is
		// published.
		class AssetImportOperation final : public PendingOperation
		{
		public:
			AssetImportOperation(EditorContext& editor, JobHandle<AssetImportOutcome> import, AssetImportResult result)
				: m_Editor(&editor), m_Import(std::move(import)), m_Result(std::move(result))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (!m_Import.IsReady())
					return std::nullopt;
				EditorAssetManager& assets = m_Editor->GetAssets();
				assets.WaitIdle();
				Result<AssetImportOutcome> outcome = m_Import.Take();
				if (!outcome)
					return std::unexpected(std::move(outcome).error().WithContext(std::format("importing '{}'", m_Result.Asset.Path)));
				m_Result.Asset = Utils::MakeAssetSummary(assets, outcome->Handle);
				m_Result.SubAssets = MakeSubAssetSummaries(assets, outcome->SubAssets);
				m_Result.Diagnostics = MakeDiagnosticInfos(outcome->Diagnostics);
				return context.SerializeResult(m_Result);
			}

			[[nodiscard]] std::string GetPhase() const override { return std::format("Automation:asset.import {}", m_Result.Asset.Path); }
		private:
			EditorContext* m_Editor = nullptr; // documented back-reference: the editor outlives its requests
			JobHandle<AssetImportOutcome> m_Import;
			AssetImportResult m_Result;
		};

		// asset.reimport's operation: the reimport, then (for a Prefab an open scene instantiates or another prefab may nest)
		// the update of the prefabs nesting it and of the instances, as an undo step of its own (UpdateReimportedPrefab).
		class AssetReimportOperation final : public PendingOperation
		{
		public:
			AssetReimportOperation(EditorContext& editor, AssetHandle handle, JobHandle<AssetImportOutcome> reimport)
				: m_Editor(&editor), m_Handle(handle), m_Reimport(std::move(reimport))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (!m_Reimport.IsReady())
					return std::nullopt;
				EditorAssetManager& assets = m_Editor->GetAssets();
				assets.WaitIdle();
				Result<AssetImportOutcome> outcome = m_Reimport.Take();
				if (!outcome)
					return std::unexpected(std::move(outcome).error().WithContext(std::format("reimporting '{}'", assets.GetReferencePath(m_Handle))));
				// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 10): the rebuilds' dropped overrides, for the result.
				PrefabRebuildCollector rebuilds(*m_Editor);
				if (Utils::UpdatesPrefabInstances(*m_Editor, m_Handle))
				{
					if (Status updated = UpdateReimportedPrefab(*m_Editor, m_Handle); !updated)
						return std::unexpected(std::move(updated).error());
				}
				AssetReimportResult result;
				result.RebuildDiagnostics = Utils::MakeRebuildDiagnostics(rebuilds.Take());
				result.Asset = Utils::MakeAssetSummary(assets, m_Handle);
				result.SubAssets = MakeSubAssetSummaries(assets, outcome->SubAssets);
				result.Diagnostics = MakeDiagnosticInfos(outcome->Diagnostics);
				return context.SerializeResult(result);
			}

			[[nodiscard]] std::string GetPhase() const override { return std::format("Automation:asset.reimport {}", m_Handle.ToString()); }
		private:
			EditorContext* m_Editor = nullptr; // documented back-reference: the editor outlives its requests
			AssetHandle m_Handle{};
			JobHandle<AssetImportOutcome> m_Reimport;
		};

		// One file asset.import copies: where it comes from and where it goes.
		struct ImportCopy
		{
			Buffer Bytes{};
			VfsPath Destination{};
		};

		// Refuses a native file that is a symbolic link or another non-regular file, or whose canonical path leaves
		// `canonicalDirectory` (asset.import never reads outside the source's folder). Errors: NotFound for a missing file;
		// PermissionDenied for the refusals; Io when the file system cannot answer.
		Status CheckNativeImportFile(const std::filesystem::path& file, const std::filesystem::path& canonicalDirectory, std::string_view pointer)
		{
			const std::string text = FileSystem::PathToUtf8(file);
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(file, error);
			if (status.type() == std::filesystem::file_type::not_found)
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer, std::format("'{}' does not exist", text)));
			if (error)
				return MakeError(ErrorCode::Io, "cannot inspect '{}': {}", text, error.message());
			if (std::filesystem::is_symlink(status) || status.type() != std::filesystem::file_type::regular)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::PermissionDenied, pointer,
					std::format("'{}' is a symbolic link, a reparse point or not a regular file: asset.import copies only regular files", text),
					"copy the file itself next to the source"));
			}
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(file, error);
			if (error)
				return MakeError(ErrorCode::Io, "cannot resolve '{}': {}", text, error.message());
			const std::filesystem::path relative = canonical.lexically_relative(canonicalDirectory);
			if (relative.empty() || *relative.begin() == std::filesystem::path(".."))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::PermissionDenied, pointer,
					std::format("'{}' lies outside the source's folder '{}'", text, FileSystem::PathToUtf8(canonicalDirectory)),
					"keep the glTF's buffers and images in its folder or below it"));
			}
			return {};
		}

		// Reads the source of asset.import and the files of its dependency closure (the decoded relative URIs of a glTF,
		// GltfImporter::ListExternalUris), checking each native file with CheckNativeImportFile. The first copy is the source.
		Result<std::vector<ImportCopy>> ReadImportFiles(EditorMethodContext& context, const AssetImportParams& params, const VfsPath& destination)
		{
			const VirtualFileSystem& vfs = context.GetEditor().GetVfs();
			const std::filesystem::path native = FileSystem::PathFromUtf8(params.Source);
			const bool isNative = params.Source.find("://") == std::string::npos && native.is_absolute();
			std::vector<ImportCopy> copies;

			// The source and a reader for the files beside it.
			std::filesystem::path canonicalDirectory;
			VfsPath projectSource; // the source in the project; empty for a native source
			if (isNative)
			{
				std::error_code error;
				canonicalDirectory = std::filesystem::weakly_canonical(native.parent_path(), error);
				if (error)
					return MakeError(ErrorCode::Io, "cannot resolve the folder of '{}': {}", params.Source, error.message());
				ENGINE_TRY(CheckNativeImportFile(native, canonicalDirectory, "/source"));
				ENGINE_TRY_ASSIGN(Buffer bytes, FileSystem::ReadFile(native));
				copies.push_back(ImportCopy{ .Bytes = std::move(bytes), .Destination = destination });
			}
			else
			{
				ENGINE_TRY_ASSIGN(VfsPath source, context.ResolveProjectPath(params.Source, "/source"));
				Result<Buffer> bytes = vfs.ReadFile(source);
				if (!bytes)
				{
					if (bytes.error().GetCode() == ErrorCode::NotFound)
						return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/source", std::format("'{}' does not exist", params.Source)));
					return std::unexpected(Utils::ToEditorFileError(std::move(bytes).error()));
				}
				copies.push_back(ImportCopy{ .Bytes = std::move(*bytes), .Destination = destination });
				projectSource = std::move(source);
			}

			// Only a glTF has a closure; extensions match importers ignoring ASCII case (IAssetImporter::CanImport).
			if (!GltfImporter().CanImport(destination.GetExtension()))
				return copies;
			ENGINE_TRY_ASSIGN(const std::vector<std::string> uris, WithContext(GltfImporter::ListExternalUris(copies.front().Bytes), std::format("reading the dependency closure of '{}'", params.Source)));
			for (const std::string& uri : uris)
			{
				ENGINE_TRY_ASSIGN(VfsPath target, WithContext(destination.GetParent().Join(uri), std::format("copying '{}'", uri)));
				if (isNative)
				{
					const std::filesystem::path file = native.parent_path() / FileSystem::PathFromUtf8(uri);
					ENGINE_TRY(WithContext(CheckNativeImportFile(file, canonicalDirectory, "/source"), std::format("in the closure of '{}'", params.Source)));
					ENGINE_TRY_ASSIGN(Buffer bytes, FileSystem::ReadFile(file));
					copies.push_back(ImportCopy{ .Bytes = std::move(bytes), .Destination = std::move(target) });
					continue;
				}
				ENGINE_TRY_ASSIGN(const VfsPath file, WithContext(projectSource.GetParent().Join(uri), std::format("in the closure of '{}'", params.Source)));
				Result<Buffer> bytes = vfs.ReadFile(file);
				if (!bytes)
					return std::unexpected(Utils::ToEditorFileError(std::move(bytes).error()).WithContext(std::format("in the closure of '{}'", params.Source)));
				copies.push_back(ImportCopy{ .Bytes = std::move(*bytes), .Destination = std::move(target) });
			}
			return copies;
		}

		// The most files the refusal of a folder that is not empty names (asset.delete).
		constexpr size_t MaxNamedFolderFiles = 5;

		// asset.delete's folder form (M16 friction S3-07): the folder `reference` names when it is an existing directory below
		// (or at) Assets/, as a project path; nullopt otherwise, the reference then naming an asset (or nothing).
		std::optional<VfsPath> FindFolderParam(const EditorMethodContext& context, std::string_view reference)
		{
			const Result<VfsPath> path = context.ResolveProjectPath(reference, "/asset");
			const Result<VfsPath> assets = VfsPath::Create("project", "Assets");
			if (!path || !assets || !path->IsUnder(*assets))
				return std::nullopt;
			const Result<FileInfo> info = context.GetEditor().GetVfs().GetInfo(*path);
			if (!info || !info->IsDirectory)
				return std::nullopt;
			return *path;
		}

		// Removes `folder`, a folder below Assets/ that holds no files (only empty folders, which go with it), as one undoable
		// AssetEditCommand of directory entries: the inverse of asset.create {type: Folder}. An empty folder holds nothing to
		// keep, so nothing goes to the trash; undo creates the folders again. The listing cannot show a host name no VfsPath can
		// spell, so the command removes each folder without recursion (EditorContext::RemoveProjectFile): a folder that still
		// holds such an entry fails the command, which restores the folders it already removed, and nothing is deleted.
		// Errors: InvalidArgument for Assets/ itself; Validation for a folder that holds files, naming the first of them; the
		// listing's and the command's errors (InvalidState for an entry the listing does not show).
		Result<AssetDeleteResult> DeleteEmptyFolder(EditorMethodContext& context, const VfsPath& folder)
		{
			EditorContext& editor = context.GetEditor();
			const std::string relative = Utils::ToProjectRelative(folder);
			ENGINE_TRY_ASSIGN(const VfsPath assets, VfsPath::Create("project", "Assets"));
			if (folder == assets)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/asset", "the Assets folder itself cannot be deleted",
					"delete the assets and folders inside it instead"));
			}
			Result<std::vector<VfsEntry>> entries = editor.GetVfs().List(folder, true);
			if (!entries)
				return std::unexpected(Utils::ToEditorFileError(std::move(entries).error()));
			std::vector<VfsPath> folders = { folder };
			std::vector<std::string> files;
			for (const VfsEntry& entry : *entries)
			{
				if (entry.Info.IsDirectory)
					folders.push_back(entry.Path);
				else
					files.push_back(Utils::ToProjectRelative(entry.Path));
			}
			if (!files.empty())
			{
				std::sort(files.begin(), files.end());
				std::string named;
				for (size_t index = 0; index < std::min(files.size(), MaxNamedFolderFiles); ++index)
					named += std::format("{}'{}'", index == 0 ? "" : ", ", files[index]);
				if (files.size() > MaxNamedFolderFiles)
					named += std::format(" and {} more", files.size() - MaxNamedFolderFiles);
				return std::unexpected(Utils::MakeParamError(ErrorCode::Validation, "/asset",
					std::format("the folder '{}' is not empty: it holds {} file(s): {}", relative, files.size(), named),
					"delete its assets first (asset.delete moves each to the trash with its .meta), then the folder"));
			}
			// A folder's path sorts before every path below it, so the reverse order removes the deepest folders first, and
			// undo, which applies the entries in reverse, creates the outermost first.
			std::sort(folders.begin(), folders.end(), [](const VfsPath& left, const VfsPath& right)
			{
				return left.GetPath() > right.GetPath();
			});
			AssetDeleteResult result;
			result.Asset = AssetSummary{ .Id = {}, .Path = relative, .Type = AssetType::None };
			std::vector<AssetFileEdit> edits;
			for (const VfsPath& path : folders)
			{
				result.RemovedFolders.push_back(Utils::ToProjectRelative(path));
				edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::Directory, .Before = Buffer(), .After = std::nullopt });
			}
			std::sort(result.RemovedFolders.begin(), result.RemovedFolders.end());
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(CreateScope<AssetEditCommand>(std::format("Delete Folder '{}'", folder.GetFileName()), std::move(edits))));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		// asset.import's check that the destination `path` of the source is free: CheckAssetPathFree, its hint naming
		// replace: true when a registered asset's source is there (World Tree brief 08).
		Status CheckImportDestinationFree(const EditorAssetManager& assets, const VirtualFileSystem& vfs, const VfsPath& path)
		{
			Status free = Utils::CheckAssetPathFree(vfs, path, "/destDir");
			const AssetRecord* record = assets.GetRegistry().FindBySourcePath(path);
			if (free || record == nullptr || record->Metadata.Kind != AssetMetaKind::Asset)
				return free;
			return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/destDir", std::format("'{}' already exists", Utils::ToProjectRelative(path)),
				"choose another path, change the existing asset, or pass replace: true to give it this file's bytes and keep its handle"));
		}

		// The sub-assets of a replaced source by key (asset.import {replace: true}): the keys of `after` that `before` has
		// (kept, their handles derived from the same source handle and key, so unchanged), those it lacks (added), and the
		// keys of `before` that `after` lacks (removed, as `before` lists them). Each list sorted by key.
		void DiffSubAssets(std::span<const SubAssetEntry> before, std::span<const SubAssetEntry> after, AssetImportResult& result)
		{
			const auto describe = [](const SubAssetEntry& entry)
			{
				return AssetSubAssetInfo{ .Key = entry.Key, .Id = entry.Handle.ToString(), .Type = entry.Type };
			};
			std::set<std::string_view> previous;
			for (const SubAssetEntry& entry : before)
				previous.insert(entry.Key);
			std::set<std::string_view> current;
			for (const SubAssetEntry& entry : after)
			{
				current.insert(entry.Key);
				(previous.contains(entry.Key) ? result.KeptSubAssets : result.AddedSubAssets).push_back(describe(entry));
			}
			for (const SubAssetEntry& entry : before)
			{
				if (!current.contains(entry.Key))
					result.RemovedSubAssets.push_back(describe(entry));
			}
			for (std::vector<AssetSubAssetInfo>* list : { &result.KeptSubAssets, &result.AddedSubAssets, &result.RemovedSubAssets })
				std::ranges::sort(*list, {}, &AssetSubAssetInfo::Key);
		}

		// A replace's result once its import completed: the asset, its sub-assets, the importer's warnings and the sub-asset
		// lists against `previous`, the sub-assets the .meta listed before the replace.
		void CompleteReplaceResult(const EditorAssetManager& assets, const AssetImportOutcome& outcome, std::span<const SubAssetEntry> previous, AssetImportResult& result)
		{
			result.Asset = Utils::MakeAssetSummary(assets, outcome.Handle);
			result.SubAssets = MakeSubAssetSummaries(assets, outcome.SubAssets);
			result.Diagnostics = MakeDiagnosticInfos(outcome.Diagnostics);
			DiffSubAssets(previous, outcome.SubAssets, result);
		}

		// asset.import {replace: true}'s operation: the reimport of the replaced source, then, as asset.reimport's operation
		// does, for a Prefab an open scene instantiates or another prefab may nest, the update of the prefabs nesting it and
		// of the instances as an undo step of its own (UpdateReimportedPrefab); then the sub-asset lists.
		class AssetReplaceOperation final : public PendingOperation
		{
		public:
			AssetReplaceOperation(EditorContext& editor, AssetHandle handle, JobHandle<AssetImportOutcome> reimport, std::vector<SubAssetEntry> previous,
				AssetImportResult result)
				: m_Editor(&editor), m_Handle(handle), m_Reimport(std::move(reimport)), m_Previous(std::move(previous)), m_Result(std::move(result))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (!m_Reimport.IsReady())
					return std::nullopt;
				EditorAssetManager& assets = m_Editor->GetAssets();
				assets.WaitIdle();
				Result<AssetImportOutcome> outcome = m_Reimport.Take();
				if (!outcome)
					return std::unexpected(std::move(outcome).error().WithContext(std::format("importing the replaced '{}'", m_Result.Asset.Path)));
				PrefabRebuildCollector rebuilds(*m_Editor);
				if (Utils::UpdatesPrefabInstances(*m_Editor, m_Handle))
				{
					if (Status updated = UpdateReimportedPrefab(*m_Editor, m_Handle); !updated)
						return std::unexpected(std::move(updated).error());
				}
				CompleteReplaceResult(assets, *outcome, m_Previous, m_Result);
				m_Result.RebuildDiagnostics = Utils::MakeRebuildDiagnostics(rebuilds.Take());
				return context.SerializeResult(m_Result);
			}

			[[nodiscard]] std::string GetPhase() const override { return std::format("Automation:asset.import {} (replace)", m_Result.Asset.Path); }
		private:
			EditorContext* m_Editor = nullptr; // documented back-reference: the editor outlives its requests
			AssetHandle m_Handle{};
			JobHandle<AssetImportOutcome> m_Reimport;
			std::vector<SubAssetEntry> m_Previous; // the sub-assets the .meta listed before the replace
			AssetImportResult m_Result;
		};

		// A replace's dry run (§13.4): the result the handler computed in the dry-run sandbox, returned by the first poll,
		// which MethodRegistry::AddPending makes at once for a dry run.
		class ResolvedImportOperation final : public PendingOperation
		{
		public:
			explicit ResolvedImportOperation(AssetImportResult result)
				: m_Result(std::move(result))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override { return context.SerializeResult(m_Result); }
		private:
			AssetImportResult m_Result;
		};

		// The files a replaced source's previous version referenced that `copies` (the new version's, the source first) no
		// longer write, and that exist: its closure as `previousBytes` lists it (a glTF's; nothing when it cannot be read)
		// and the dependency files `handle` owns. Project-relative, sorted.
		std::vector<std::string> FindUnreferencedFiles(const EditorAssetManager& assets, const VirtualFileSystem& vfs, AssetHandle handle, const VfsPath& source,
			const std::optional<Buffer>& previousBytes, std::span<const ImportCopy> copies)
		{
			std::set<VfsPath> previous;
			if (previousBytes.has_value() && GltfImporter().CanImport(source.GetExtension()))
			{
				if (const Result<std::vector<VfsPath>> files = GltfImporter().ListDependencyFiles(*previousBytes, source); files.has_value())
					previous.insert(files->begin(), files->end());
			}
			for (const AssetRecord* dependency : assets.GetRegistry().GetDependencyRecords(handle))
				previous.insert(dependency->SourcePath);
			for (const ImportCopy& copy : copies)
				previous.erase(copy.Destination);
			std::vector<std::string> unreferenced;
			for (const VfsPath& file : previous)
			{
				if (vfs.Exists(file))
					unreferenced.push_back(Utils::ToProjectRelative(file));
			}
			std::ranges::sort(unreferenced);
			return unreferenced;
		}

		// What a file at `path` is, for a refusal that names it: the source of another asset, another asset's dependency
		// file, or a file without a .meta.
		std::string DescribeExistingFile(const EditorAssetManager& assets, const VfsPath& path)
		{
			const AssetRecord* record = assets.GetRegistry().FindBySourcePath(path);
			if (record == nullptr)
				return "a file without a .meta";
			if (record->Metadata.Kind == AssetMetaKind::Dependency)
				return std::format("a dependency file of '{}'", assets.GetReferencePath(record->Metadata.Owner));
			return std::format("the source of the {} {}", AssetTypeToString(record->Metadata.Type), record->Metadata.Handle.ToString());
		}

		// asset.import {replace: true} (World Tree brief 08; AssetMethods.h): `directory` is destDir resolved. The destination
		// is `directory` itself when it is a registered source, else directory/<the source's file name>. Imports the replace in
		// the dry-run sandbox first (a dry run stops there and resolves at once; a file that does not import is ImportFailed at
		// /source, nothing written), then writes only the files whose bytes change, as one AssetEditCommand, and reimports
		// through asset.reimport's path.
		Result<Scope<PendingOperation>> ReplaceImportedSource(EditorMethodContext& context, const AssetImportParams& params, const VfsPath& directory)
		{
			EditorContext& editor = context.GetEditor();
			EditorAssetManager& assets = editor.GetAssets();
			VirtualFileSystem& vfs = editor.GetVfs();
			const std::string fileName = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(params.Source).filename());
			VfsPath destination = directory;
			if (assets.GetRegistry().FindBySourcePath(directory) == nullptr)
			{
				ENGINE_TRY_ASSIGN(destination, WithContext(directory.Join(fileName), std::format("replacing with '{}'", params.Source)));
			}
			const std::string relative = Utils::ToProjectRelative(destination);
			const AssetRecord* record = assets.GetRegistry().FindBySourcePath(destination);
			if (record == nullptr)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/destDir", std::format("no imported asset at '{}' to replace", relative),
					"name the asset's source, or the folder that holds it with the same file name, in destDir; import a new file without replace"));
			}
			if (record->Metadata.Kind == AssetMetaKind::Dependency)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/destDir",
					std::format("'{}' is a dependency file of '{}', not an asset", relative, assets.GetReferencePath(record->Metadata.Owner)),
					"replace the asset that owns it: its new version brings the file with it"));
			}
			// Copied before anything is written: a write re-registers the record.
			const AssetMetadata previous = record->Metadata;
			const VfsPath metaPath = record->MetaPath;
			const AssetHandle handle = previous.Handle;

			// The new file's importer, by its own extension, must be the asset's: a replace never changes the asset's type.
			ENGINE_TRY_ASSIGN(const VfsPath named, WithContext(destination.GetParent().Join(fileName), std::format("replacing with '{}'", params.Source)));
			Result<AssetMetadata> fresh = assets.CreateMetadata(named);
			if (!fresh)
			{
				if (fresh.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(std::move(fresh).error());
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/source", fresh.error().GetMessageText(), fresh.error().GetHint()));
			}
			if (fresh->Importer != previous.Importer)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/source",
					std::format("'{}' takes the {} importer, but '{}' is a {} of the {} importer: a replace keeps the asset's importer and type", fileName,
						fresh->Importer, relative, AssetTypeToString(previous.Type), previous.Importer),
					"replace it with a file of the same kind, or import this file as a new asset without replace"));
			}

			// Every file is read and checked before anything is written, as for a first import.
			ENGINE_TRY_ASSIGN(std::vector<ImportCopy> copies, ReadImportFiles(context, params, destination));
			AssetMetadata changed = previous;
			if (!params.Settings.Get().is_null())
			{
				ENGINE_TRY_ASSIGN(const Json patch, PrepareSettingsPatch(editor.GetTypeRegistry(), previous.Importer, params.Settings, "/settings"));
				ENGINE_TRY_ASSIGN(changed.Settings, MergeSettings(assets, previous.Importer, fresh->Settings, patch, "/settings"));
			}
			const auto readCurrent = [&vfs](const VfsPath& path) -> Result<std::optional<Buffer>>
			{
				Result<Buffer> bytes = vfs.ReadFile(path);
				if (bytes)
					return std::make_optional(std::move(*bytes));
				if (bytes.error().GetCode() == ErrorCode::NotFound)
					return std::optional<Buffer>();
				return std::unexpected(Utils::ToEditorFileError(std::move(bytes).error()));
			};
			ENGINE_TRY_ASSIGN(const std::optional<Buffer> previousSource, readCurrent(destination));

			AssetImportResult result;
			result.Asset = AssetSummary{ .Id = handle.ToString(), .Path = relative, .Type = previous.Type };
			result.UnreferencedFiles = FindUnreferencedFiles(assets, vfs, handle, destination, previousSource, copies);
			std::vector<AssetFileEdit> edits;
			std::set<VfsPath> directories;
			for (size_t index = 1; index < copies.size(); ++index)
			{
				ImportCopy& copy = copies[index];
				ENGINE_TRY_ASSIGN(std::optional<Buffer> current, readCurrent(copy.Destination));
				if (current.has_value())
				{
					if (*current == copy.Bytes)
						continue;
					const AssetRecord* owned = assets.GetRegistry().FindBySourcePath(copy.Destination);
					if (owned == nullptr || owned->Metadata.Kind != AssetMetaKind::Dependency || owned->Metadata.Owner != handle)
					{
						return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/source",
							std::format("'{}' already exists as {}, and the new '{}' references other bytes there: a replace never changes another asset's file",
								Utils::ToProjectRelative(copy.Destination), DescribeExistingFile(assets, copy.Destination), fileName),
							"give each glTF its own copy of the file, under its own name"));
					}
					result.CopiedFiles.push_back(Utils::ToProjectRelative(copy.Destination));
					edits.push_back(AssetFileEdit{ .Path = copy.Destination, .Kind = AssetFileKind::File, .Before = std::move(current), .After = std::move(copy.Bytes) });
					continue;
				}
				// A new file of the closure: copied with a dependency meta owned by the asset (§6.4), as a first import copies it.
				ENGINE_TRY(Utils::CheckAssetPathFree(vfs, copy.Destination, "/source"));
				std::vector<AssetFileEdit> missing;
				Utils::AddMissingDirectoryEdits(vfs, copy.Destination.GetParent(), missing);
				for (AssetFileEdit& edit : missing)
				{
					if (directories.insert(edit.Path).second)
						edits.push_back(std::move(edit));
				}
				ENGINE_TRY_ASSIGN(const VfsPath dependencyMeta, GetMetaPath(copy.Destination));
				result.CopiedFiles.push_back(Utils::ToProjectRelative(copy.Destination));
				result.CopiedFiles.push_back(Utils::ToProjectRelative(dependencyMeta));
				edits.push_back(AssetFileEdit{ .Path = copy.Destination, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = std::move(copy.Bytes) });
				edits.push_back(AssetFileEdit{ .Path = dependencyMeta, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(SerializeAssetMetadata(assets.CreateDependencyMetadata(handle))) });
			}
			if (changed.Settings != previous.Settings)
			{
				ENGINE_TRY_ASSIGN(std::optional<Buffer> currentMeta, readCurrent(metaPath));
				Buffer meta = TextToBuffer(SerializeAssetMetadata(changed));
				if (currentMeta != meta)
				{
					result.CopiedFiles.push_back(Utils::ToProjectRelative(metaPath));
					edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::move(currentMeta), .After = std::move(meta) });
				}
			}
			// The source last, so its write's reimport sees every other file already written.
			if (previousSource != copies.front().Bytes)
			{
				result.CopiedFiles.push_back(relative);
				edits.push_back(AssetFileEdit{ .Path = destination, .Kind = AssetFileKind::File, .Before = previousSource, .After = std::move(copies.front().Bytes) });
			}
			std::ranges::sort(result.CopiedFiles);

			// The replace in memory first (the lead's review of brief 08): its files written and the asset imported in the
			// dry-run sandbox (§13.4; the import runs in memory, EditorAssetManager::BeginDryRun), the one a dry run holds or one
			// opened for this call. A dry run reports this import, and a replace whose new file does not import writes nothing.
			const std::string label = std::format("Replace Asset '{}'", destination.GetFileName());
			Result<AssetImportOutcome> trial;
			{
				Scope<EditorDryRunScope> sandbox;
				if (!context.IsDryRun())
				{
					ENGINE_TRY_ASSIGN(sandbox, EditorDryRunScope::Begin(editor));
				}
				if (!edits.empty())
					ENGINE_TRY(editor.Execute(CreateScope<AssetEditCommand>(label, edits)));
				trial = assets.Reimport(handle);
			}
			if (!trial)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::ImportFailed, "/source",
					std::format("'{}' does not import, so '{}' was not replaced: {}", params.Source, relative, trial.error().GetMessageText()),
					"fix the file and replace again (dryRun: true checks a file without writing it); nothing was written"));
			}
			if (context.IsDryRun())
			{
				CompleteReplaceResult(assets, *trial, previous.SubAssets, result);
				return Scope<PendingOperation>(CreateScope<ResolvedImportOperation>(std::move(result)));
			}
			if (!edits.empty())
			{
				ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(CreateScope<AssetEditCommand>(label, std::move(edits))));
				result.UndoIndex = ToAutomationCounter(undoIndex);
			}
			ENGINE_INFO("asset.import replaced '{}' with '{}' ({} file(s) written)", relative, params.Source, result.CopiedFiles.size());
			return Scope<PendingOperation>(CreateScope<AssetReplaceOperation>(editor, handle, assets.ReimportAsync(handle), previous.SubAssets, std::move(result)));
		}

	}

	namespace Automation {

		Result<AssetListResult> AssetList(EditorMethodContext& context, const AssetListParams& params)
		{
			if (params.Limit < 1 || params.Limit > MaxListLimit)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/limit", std::format("limit must be 1 to {}", MaxListLimit)));
			if (params.Dir.starts_with(EngineDirPrefix))
				return ListBuiltinAssets(context.GetEditor().GetAssets(), params);
			ENGINE_TRY_ASSIGN(const VfsPath directory, Utils::ResolveAssetsPath(context, params.Dir, "/dir", {}, true));
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(Utils::RefreshAssets(editor));
			Result<FileInfo> info = editor.GetVfs().GetInfo(directory);
			if (!info || !info->IsDirectory)
			{
				return std::unexpected(Utils::MakeParamError(info ? ErrorCode::InvalidArgument : ErrorCode::NotFound, "/dir",
					std::format("'{}' is not a folder of the project", params.Dir), "asset.list {dir: \"Assets\"} lists every asset"));
			}

			// Sorted by source path, a source's sub-assets right after it; dependency files are never listed.
			const EditorAssetManager& assets = editor.GetAssets();
			std::vector<AssetHandle> matches;
			for (const AssetRecord* record : assets.GetRegistry().GetRecords())
			{
				if (record->Metadata.Kind == AssetMetaKind::Dependency)
					continue;
				const VfsPath& source = record->SourcePath;
				const bool inside = params.Recursive ? source.IsUnder(directory) && source != directory : source.GetParent() == directory;
				if (!inside)
					continue;
				if (params.Type == AssetType::None || params.Type == record->Metadata.Type)
					matches.push_back(record->Metadata.Handle);
				if (!params.SubAssets)
					continue;
				for (const SubAssetEntry& subAsset : record->Metadata.SubAssets)
				{
					if (params.Type == AssetType::None || params.Type == subAsset.Type)
						matches.push_back(subAsset.Handle);
				}
			}

			ENGINE_TRY_ASSIGN(const size_t offset, ParseListCursor(params.Cursor, matches.size()));
			const size_t end = std::min(matches.size(), offset + params.Limit);
			AssetListResult result;
			result.Total = ToAutomationCounter(matches.size());
			for (size_t index = offset; index < end; ++index)
				result.Assets.push_back(Utils::MakeAssetSummary(assets, matches[index]));
			if (end < matches.size())
				result.NextCursor = std::to_string(end);
			return result;
		}

		Result<AssetInfoResult> AssetInfo(EditorMethodContext& context, const AssetInfoParams& params)
		{
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = context.GetEditor().GetAssets();
			AssetInfoResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			result.State = assets.GetState(handle);
			result.Version = ToAutomationCounter(assets.GetVersion(handle));
			for (const AssetHandle dependency : assets.GetDependencyGraph().GetDependencies(handle))
				result.Dependencies.push_back(Utils::MakeAssetSummary(assets, dependency));
			for (const AssetHandle dependent : assets.GetDependencyGraph().GetDependents(handle))
				result.Dependents.push_back(Utils::MakeAssetSummary(assets, dependent));

			if (const BuiltinAssetEntry* builtin = assets.GetBuiltins().Find(handle); builtin != nullptr)
			{
				result.Source = builtin->Path;
				result.Importer = builtin->Importer;
				result.Settings = builtin->Settings;
				if (const std::optional<ImporterFacts> facts = FindImporterFacts(builtin->Importer); facts.has_value())
					result.ImporterVersion = facts->Version;
			}
			else if (const std::optional<AssetRegistry::Location> location = assets.GetRegistry().Locate(handle); location.has_value())
			{
				const AssetRecord& record = *location->Record;
				result.Source = Utils::ToProjectRelative(record.SourcePath);
				result.Importer = record.Metadata.Importer;
				result.Settings = record.Metadata.Settings;
				const std::optional<ImporterFacts> facts = FindImporterFacts(record.Metadata.Importer);
				result.ImporterVersion = facts.has_value() ? facts->Version : record.Metadata.ImporterVersion;
				if (location->SubAssetKey.empty())
				{
					result.SubAssets = MakeSubAssetSummaries(assets, record.Metadata.SubAssets);
					for (const AssetRecord* dependency : assets.GetRegistry().GetDependencyRecords(handle))
						result.DependencyFiles.push_back(Utils::ToProjectRelative(dependency->SourcePath));
					std::sort(result.DependencyFiles.begin(), result.DependencyFiles.end());
				}
			}

			for (const AssetDiagnostic& diagnostic : assets.GetDiagnostics())
			{
				const bool aboutSource = !diagnostic.Asset.IsValid() && !result.Source.empty() && diagnostic.Path == result.Source;
				if (diagnostic.Asset == handle || aboutSource)
					result.Diagnostics.push_back(Utils::MakeAssetDiagnosticInfo(diagnostic));
			}
			return result;
		}

		Result<Scope<PendingOperation>> AssetImport(EditorMethodContext& context, const AssetImportParams& params)
		{
			EditorContext& editor = context.GetEditor();
			if (context.IsDryRun() && !params.Replace)
			{
				return std::unexpected(Error(ErrorCode::Unsupported, "asset.import supports dryRun only with replace: true; a first import cannot be validated without being applied")
						.WithHint("import without dryRun (edit.undo removes the copies), or pass replace: true to preview a replace"));
			}
			ENGINE_TRY_ASSIGN(const VfsPath directory, Utils::ResolveAssetsPath(context, params.DestDir, "/destDir", {}, true));
			if (!params.Settings.Get().is_null() && !params.Settings.Get().is_object())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/settings", "the settings must be an object of import settings",
					"for example {\"Scale\": 0.01}"));
			}
			if (params.Source.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/source", "the source must not be empty"));
			ENGINE_TRY(Utils::RefreshAssets(editor));
			if (params.Replace)
				return ReplaceImportedSource(context, params, directory);

			// Every file is read and checked before anything is written: a rejected URI refuses the whole import.
			const std::string fileName = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(params.Source).filename());
			ENGINE_TRY_ASSIGN(const VfsPath destination, WithContext(directory.Join(fileName), std::format("importing '{}'", params.Source)));
			ENGINE_TRY_ASSIGN(std::vector<ImportCopy> copies, ReadImportFiles(context, params, destination));

			// The asset's .meta: the importer that takes the extension, its settings merged with the given ones.
			EditorAssetManager& assets = editor.GetAssets();
			Result<AssetMetadata> created = assets.CreateMetadata(destination);
			if (!created)
			{
				if (created.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(std::move(created).error());
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/source", created.error().GetMessageText(), created.error().GetHint()));
			}
			AssetMetadata metadata = std::move(*created);
			if (!params.Settings.Get().is_null())
			{
				ENGINE_TRY_ASSIGN(const Json patch, PrepareSettingsPatch(editor.GetTypeRegistry(), metadata.Importer, params.Settings, "/settings"));
				ENGINE_TRY_ASSIGN(metadata.Settings, MergeSettings(assets, metadata.Importer, metadata.Settings, patch, "/settings"));
			}
			VirtualFileSystem& vfs = editor.GetVfs();
			std::vector<AssetFileEdit> edits;
			std::set<VfsPath> directories;
			const auto addDirectories = [&vfs, &edits, &directories](const VfsPath& folder)
			{
				std::vector<AssetFileEdit> missing;
				Utils::AddMissingDirectoryEdits(vfs, folder, missing);
				for (AssetFileEdit& edit : missing)
				{
					if (directories.insert(edit.Path).second)
						edits.push_back(std::move(edit));
				}
			};
			AssetImportResult result;
			for (size_t index = 0; index < copies.size(); ++index)
			{
				ImportCopy& copy = copies[index];
				if (index == 0)
					ENGINE_TRY(CheckImportDestinationFree(assets, vfs, copy.Destination));
				else
					ENGINE_TRY(Utils::CheckAssetPathFree(vfs, copy.Destination, "/destDir"));
				addDirectories(copy.Destination.GetParent());
				// The source's .meta, then a dependency meta per closure file (§6.4), owned by the source.
				const AssetMetadata meta = index == 0 ? metadata : assets.CreateDependencyMetadata(metadata.Handle);
				const std::string metaText = SerializeAssetMetadata(meta);
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(copy.Destination));
				result.CopiedFiles.push_back(Utils::ToProjectRelative(copy.Destination));
				result.CopiedFiles.push_back(Utils::ToProjectRelative(metaPath));
				edits.push_back(AssetFileEdit{ .Path = copy.Destination, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = std::move(copy.Bytes) });
				edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(metaText) });
			}
			std::sort(result.CopiedFiles.begin(), result.CopiedFiles.end());

			ENGINE_TRY_ASSIGN(const uint64_t undoIndex,
				editor.Execute(CreateScope<AssetEditCommand>(std::format("Import Asset '{}'", fileName), std::move(edits))));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			result.Asset = AssetSummary{ .Id = metadata.Handle.ToString(), .Path = Utils::ToProjectRelative(destination), .Type = metadata.Type };
			ENGINE_INFO("asset.import copied '{}' and {} file(s) of its closure to '{}'", params.Source, copies.size() - 1, Utils::ToProjectRelative(directory));
			return Scope<PendingOperation>(CreateScope<AssetImportOperation>(editor, assets.ReimportAsync(metadata.Handle), std::move(result)));
		}

		Result<Scope<PendingOperation>> AssetReimport(EditorMethodContext& context, const AssetReimportParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "source to import"));
			static_cast<void>(record);
			return Scope<PendingOperation>(CreateScope<AssetReimportOperation>(editor, handle, assets.ReimportAsync(handle)));
		}

		Result<AssetCreateResult> AssetCreate(EditorMethodContext& context, const AssetCreateParams& params)
		{
			EditorContext& editor = context.GetEditor();
			const bool takesValues = params.Type == AssetCreateType::Material || params.Type == AssetCreateType::SoundEffect
				|| params.Type == AssetCreateType::SpriteSheet || params.Type == AssetCreateType::Tilemap || params.Type == AssetCreateType::StringTable;
			if (!takesValues && context.HasParam("values") && !params.Values.Get().is_null())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/values",
					"only a Material, a SoundEffect, a SpriteSheet, a Tilemap or a StringTable takes values",
					"leave values out for scenes, prefabs, heightfields and folders"));
			}
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", GetCreateExtension(params.Type)));
			ENGINE_TRY(Utils::RefreshAssets(editor));
			VirtualFileSystem& vfs = editor.GetVfs();
			const std::string relative = Utils::ToProjectRelative(path);
			const std::string stem(path.GetStem());

			AssetCreateResult result;
			result.Path = relative;
			std::vector<AssetFileEdit> edits;
			std::string label;
			if (params.Type == AssetCreateType::Folder)
			{
				if (vfs.Exists(path))
				{
					// M17 friction F07: a folder that is already there is what was asked for, so nothing is written or recorded
					// (created: false) and an edit.batch no longer fails on a folder the project template made.
					Result<FileInfo> existing = vfs.GetInfo(path);
					if (!existing)
						return std::unexpected(Utils::ToEditorFileError(std::move(existing).error()));
					if (!existing->IsDirectory)
					{
						return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/path", std::format("'{}' already exists as a file", relative),
							"choose another path for the folder"));
					}
					return result;
				}
				Utils::AddMissingDirectoryEdits(vfs, path, edits);
				label = std::format("Create Folder '{}'", path.GetFileName());
			}
			else
			{
				ENGINE_TRY(Utils::CheckAssetPathFree(vfs, path, "/path"));
				Json values = params.Values.Get();
				if (params.Type == AssetCreateType::SpriteSheet)
					ENGINE_TRY(ResolveSpriteSheetTexture(context, values, "/values"));
				else if (params.Type == AssetCreateType::Tilemap)
					ENGINE_TRY(ResolveTilemapSheets(context, values, "/values"));
				else if (params.Type == AssetCreateType::Material)
					ENGINE_TRY(ResolveMaterialTextures(context, values, "/values"));
				const std::optional<AssetHandle> sheetTexture = params.Type == AssetCreateType::SpriteSheet ? FindSpriteSheetTexture(values) : std::nullopt;
				ENGINE_TRY_ASSIGN(Buffer bytes, MakeNativeAssetBytes(editor, params.Type, path, VariantValue(std::move(values))));
				std::optional<FileWrite> uncompressed;
				if (sheetTexture.has_value())
				{
					ENGINE_TRY_ASSIGN(uncompressed, MakeUncompressedTextureMeta(editor.GetAssets(), *sheetTexture, "/values/Texture"));
				}
				ENGINE_TRY_ASSIGN(const AssetMetadata metadata, editor.GetAssets().CreateMetadata(path));
				const std::string metaText = SerializeAssetMetadata(metadata);
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
				Utils::AddMissingDirectoryEdits(vfs, path.GetParent(), edits);
				edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = std::move(bytes) });
				edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(metaText) });
				if (uncompressed.has_value())
				{
					// The sheet's texture's .meta, written with its current bytes as the Before state, in the same step.
					ENGINE_TRY_ASSIGN(const Scope<AssetEditCommand> textureWrite, AssetEditCommand::CreateForWrites(editor, std::span(&*uncompressed, 1), ""));
					edits.insert(edits.end(), textureWrite->GetEdits().begin(), textureWrite->GetEdits().end());
				}
				result.Asset = AssetSummary{ .Id = metadata.Handle.ToString(), .Path = relative, .Type = metadata.Type };
				// A sound effect is an AudioClip asset (SoundEffectImporter's main type); its undo label names what was created.
				std::string_view what = AssetTypeToString(metadata.Type);
				if (params.Type == AssetCreateType::SoundEffect)
					what = "Sound Effect";
				else if (params.Type == AssetCreateType::SpriteSheet)
					what = "Sprite Sheet";
				else if (params.Type == AssetCreateType::StringTable)
					what = "String Table";
				label = std::format("Create {} '{}'", what, stem);
			}
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(CreateScope<AssetEditCommand>(std::move(label), std::move(edits))));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			result.Created = true;
			return result;
		}

		Result<AssetGetPropertiesResult> AssetGetProperties(EditorMethodContext& context, const AssetGetPropertiesParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			Result<const AssetRecord*> record = FindMainAssetRecord(assets, handle, "/asset", "properties");
			const std::optional<NativePropertiesKind> kind = record ? FindNativePropertiesKind(**record) : std::nullopt;
			if (!kind.has_value())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/asset",
					std::format("'{}' is not a native asset: it has no properties of its own", assets.GetReferencePath(handle)), std::string(NotNativeHint)));
			}
			const VfsPath source = (*record)->SourcePath;
			Result<std::string> text = editor.GetVfs().ReadText(source);
			if (!text)
				return std::unexpected(Utils::ToEditorFileError(std::move(text).error()));
			ENGINE_TRY_ASSIGN(NativeProperties properties, ReadNativeProperties(*kind, *text, source, editor.GetTypeRegistry(), nullptr, editor.GetAssets()));
			AssetGetPropertiesResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			result.Values = VariantValue(std::move(properties.Values));
			return result;
		}

		Result<AssetSetPropertiesResult> AssetSetProperties(EditorMethodContext& context, const AssetSetPropertiesParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			Result<const AssetRecord*> found = FindMainAssetRecord(assets, handle, "/asset", "properties");
			const std::optional<NativePropertiesKind> kind = found ? FindNativePropertiesKind(**found) : std::nullopt;
			if (!kind.has_value())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/asset",
					std::format("'{}' is not a native asset: it has no properties of its own", assets.GetReferencePath(handle)), std::string(NotNativeHint)));
			}
			const VfsPath source = (*found)->SourcePath;
			Result<std::string> text = editor.GetVfs().ReadText(source);
			if (!text)
				return std::unexpected(Utils::ToEditorFileError(std::move(text).error()));
			Json values = params.Values.Get();
			if (*kind == NativePropertiesKind::SpriteSheet)
				ENGINE_TRY(ResolveSpriteSheetTexture(context, values, "/values"));
			else if (*kind == NativePropertiesKind::Tilemap)
				ENGINE_TRY(ResolveTilemapSheets(context, values, "/values"));
			else if (*kind == NativePropertiesKind::Material)
				ENGINE_TRY(ResolveMaterialTextures(context, values, "/values"));
			ENGINE_TRY_ASSIGN(NativeProperties patched, ReadNativeProperties(*kind, *text, source, editor.GetTypeRegistry(), &values, editor.GetAssets()));

			// The sheet's file and, when its texture is not at None yet, the texture's .meta (MakeUncompressedTextureMeta), as one
			// step.
			std::vector<FileWrite> writes;
			if (patched.Text != *text)
				writes.emplace_back(source, TextToBuffer(patched.Text));
			if (*kind == NativePropertiesKind::SpriteSheet)
			{
				if (const std::optional<AssetHandle> texture = FindSpriteSheetTexture(patched.Values); texture.has_value())
				{
					ENGINE_TRY_ASSIGN(std::optional<FileWrite> uncompressed, MakeUncompressedTextureMeta(editor.GetAssets(), *texture, "/values/Texture"));
					if (uncompressed.has_value())
						writes.push_back(std::move(*uncompressed));
				}
			}
			AssetSetPropertiesResult result;
			if (!writes.empty())
			{
				ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrites(editor, writes, std::format("Set {} '{}'", GetNativePropertiesLabel(*kind), source.GetStem())));
				ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
				result.UndoIndex = ToAutomationCounter(undoIndex);
			}
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			result.Values = VariantValue(std::move(patched.Values));
			return result;
		}

		Result<AssetGetImportSettingsResult> AssetGetImportSettings(EditorMethodContext& context, const AssetGetImportSettingsParams& params)
		{
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = context.GetEditor().GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "import settings"));
			AssetGetImportSettingsResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			result.Importer = record->Metadata.Importer;
			result.Settings = record->Metadata.Settings;
			return result;
		}

		Result<AssetSetImportSettingsResult> AssetSetImportSettings(EditorMethodContext& context, const AssetSetImportSettingsParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "import settings"));
			const AssetMetadata metadata = record->Metadata;
			const VfsPath metaPath = record->MetaPath;
			ENGINE_TRY_ASSIGN(const Json patch, PrepareSettingsPatch(editor.GetTypeRegistry(), metadata.Importer, params.Settings, "/settings"));
			AssetMetadata changed = metadata;
			ENGINE_TRY_ASSIGN(changed.Settings, MergeSettings(assets, metadata.Importer, metadata.Settings, patch, "/settings"));

			AssetSetImportSettingsResult result;
			result.Importer = metadata.Importer;
			result.Settings = changed.Settings;
			if (changed.Settings == metadata.Settings)
			{
				result.Asset = Utils::MakeAssetSummary(assets, handle);
				return result;
			}
			const std::string text = SerializeAssetMetadata(changed);
			const std::string label = std::format("Set Import Settings '{}'", Utils::ToProjectRelative(record->SourcePath));
			ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrite(editor, metaPath, AsBytes(text), label));

			// A Prefab's instances and the prefabs nesting it follow the new settings in the same undo step (§5.5 "Update").
			PrefabRebuildCollector rebuilds(editor);
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, Utils::ExecuteImportSettingsWrite(editor, handle, std::move(command), label));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			result.RebuildDiagnostics = Utils::MakeRebuildDiagnostics(rebuilds.Take());
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			return result;
		}

		Result<AssetMoveResult> AssetMove(EditorMethodContext& context, const AssetMoveParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "file to move"));
			const std::string_view extension = record->SourcePath.GetExtension();
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", extension));
			ENGINE_TRY_ASSIGN(Scope<AssetMoveCommand> command, AssetMoveCommand::Create(editor, handle, path));

			AssetMoveResult result;
			for (const AssetFileMove& move : command->GetMoves())
				result.MovedFiles.push_back(Utils::ToProjectRelative(move.To));
			std::sort(result.MovedFiles.begin(), result.MovedFiles.end());
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			return result;
		}

		Result<AssetDeleteResult> AssetDelete(EditorMethodContext& context, const AssetDeleteParams& params)
		{
			EditorContext& editor = context.GetEditor();
			if (const std::optional<VfsPath> folder = FindFolderParam(context, params.Asset); folder.has_value())
				return DeleteEmptyFolder(context, *folder);
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, params.Asset, "/asset"));
			const EditorAssetManager& assets = editor.GetAssets();
			ENGINE_TRY_ASSIGN(const AssetRecord* record, FindMainAssetRecord(assets, handle, "/asset", "file to delete"));
			static_cast<void>(record);
			AssetDeleteResult result;
			result.Asset = Utils::MakeAssetSummary(assets, handle);
			ENGINE_TRY_ASSIGN(const VfsPath trash, AssetDeleteCommand::ChooseTrashDirectory(editor, handle));
			ENGINE_TRY_ASSIGN(Scope<AssetDeleteCommand> command, AssetDeleteCommand::Create(editor, handle));
			result.TrashDirectory = Utils::ToProjectRelative(trash);
			for (const AssetFileMove& move : command->GetMoves())
				result.TrashedFiles.push_back(Utils::ToProjectRelative(move.From));
			std::sort(result.TrashedFiles.begin(), result.TrashedFiles.end());
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

	}

	void RegisterAssetMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<AssetState>("AssetState", "Where an asset is in its life (§7.2).")
			.Entry(AssetState::Unloaded, "Unloaded", "Registered, not loaded.")
			.Entry(AssetState::Loading, "Loading", "A load or the import it needs is running.")
			.Entry(AssetState::Loaded, "Loaded", "The current version is loaded.")
			.Entry(AssetState::Failed, "Failed", "The last load failed; uses get the placeholder and a diagnostic is recorded.");

		registry.Enum<AssetCreateType>("AssetCreateType", "What asset.create makes.")
			.Entry(AssetCreateType::Material, "Material", "A .material file: a PBR material.")
			.Entry(AssetCreateType::Scene, "Scene", "A .scene file: an empty scene named after the file.")
			.Entry(AssetCreateType::Prefab, "Prefab",
				"A .prefab file with one empty root entity named after the file; it takes no values (prefab.create saves an entity built in a "
				"scene, its components and children, as a prefab).")
			.Entry(AssetCreateType::SoundEffect, "SoundEffect", "A .sfx sound effect: synthesized layers of oscillators, imported as an AudioClip.")
			.Entry(AssetCreateType::Folder, "Folder", "A folder below Assets/; one that already exists is left as it is (created: false).")
			.Entry(AssetCreateType::SpriteSheet, "SpriteSheet",
				"A .spritesheet sprite sheet: frames of a texture with pivots and 9-slice borders, and clips; without values an empty sheet.")
			.Entry(AssetCreateType::Tilemap, "Tilemap",
				"A .tilemap tilemap: a palette of sprite sheet frames and cells in chunks; without values an empty map of unit cells in 16-cell chunks.")
			.Entry(AssetCreateType::StringTable, "StringTable",
				"A .stringtable string table: keyed strings in several locales, read by Loc.Get; without values the locale en and no entries.")
			.Entry(AssetCreateType::Heightfield, "Heightfield",
				"A .heightfield heightfield for a Terrain: a flat square of 129 x 129 samples at height 0; takes no values (terrain.importHeightmap converts "
				"a heightmap, terrain.edit sculpts it).");

		// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 10).
		registry.Struct<PrefabRebuildDiagnostic>("PrefabRebuildDiagnostic", "A diagnostic of a rebuild of an open scene's prefab instances, an override it dropped among them.")
			.Field("scene", &PrefabRebuildDiagnostic::Scene, "The open scene whose instances were rebuilt (its project path; empty for a scene never saved).")
			.Field("severity", &PrefabRebuildDiagnostic::Severity, "How serious it is.")
			.Field("code", &PrefabRebuildDiagnostic::Code, "The diagnostic code: PREFAB_STALE_OVERRIDE for an override the rebuild dropped.")
			.Field("message", &PrefabRebuildDiagnostic::Message, "What happened: which override was dropped and why.")
			.Field("pointer", &PrefabRebuildDiagnostic::Pointer, "Where, within the instance root: /Components/Prefab/Overrides/<i> for a dropped override.")
			.Field("entity", &PrefabRebuildDiagnostic::Entity, "The instance root (16 hex digits).");
		registry.Struct<AssetDiagnosticInfo>("AssetDiagnosticInfo", "A diagnostic of an asset.")
			.Field("severity", &AssetDiagnosticInfo::Severity, "How serious it is.")
			.Field("code", &AssetDiagnosticInfo::Code, "The diagnostic code, such as ASSET_IMPORT_FAILED.")
			.Field("path", &AssetDiagnosticInfo::Path, "The file concerned: project-relative, or an engine path.")
			.Field("message", &AssetDiagnosticInfo::Message, "What is wrong.")
			.Field("hint", &AssetDiagnosticInfo::Hint, "How to fix it; empty when there is no hint.");

		registry.Struct<AssetListParams>("AssetListParams", "The params of asset.list.")
			.Field("dir", &AssetListParams::Dir,
				"The folder to list: project-relative, Assets or below it; or engine:// or a folder below it, such as engine://Environments, "
				"for the built-in assets (which have no sub-assets).")
			.Field("type", &AssetListParams::Type, "Only assets of this type; None lists every type.")
			.Field("recursive", &AssetListParams::Recursive, "Also list the folders below dir.")
			.Field("subAssets", &AssetListParams::SubAssets, "Also list sub-assets (a glTF's meshes, materials and textures) after their source.")
			.Field("limit", &AssetListParams::Limit, "The most assets to return.", { .Min = 1.0, .Max = static_cast<double>(MaxListLimit) })
			.Field("cursor", &AssetListParams::Cursor, "\"\" for the start, or the nextCursor of the previous call.");

		registry.Struct<AssetListResult>("AssetListResult", "One page of assets.")
			.Field("assets", &AssetListResult::Assets, "The assets, sorted by path, sub-assets after their source.")
			.Field("nextCursor", &AssetListResult::NextCursor, "The cursor of the next page; empty when the listing is complete.")
			.Field("total", &AssetListResult::Total, "The number of matching assets over every page.");

		registry.Struct<AssetInfoParams>("AssetInfoParams", "The params of asset.info.")
			.Field("asset", &AssetInfoParams::Asset, "The asset: 16 hex digits, a path such as \"Assets/Materials/Red.material\" or an engine path.");

		registry.Struct<AssetInfoResult>("AssetInfoResult", "An asset's metadata, dependencies and diagnostics.")
			.Field("asset", &AssetInfoResult::Asset, "The asset.")
			.Field("source", &AssetInfoResult::Source, "Its source file (project-relative; a sub-asset's is its source's), or an engine path.")
			.Field("importer", &AssetInfoResult::Importer, "The importer named in its .meta; empty for procedural built-ins.")
			.Field("importerVersion", &AssetInfoResult::ImporterVersion, "The registered importer's version.")
			.Field("settings", &AssetInfoResult::Settings, "The import settings of its .meta; null without settings.")
			.Field("subAssets", &AssetInfoResult::SubAssets, "The sub-assets of a source; empty for a sub-asset.")
			.Field("dependencies", &AssetInfoResult::Dependencies, "The assets it references, sorted by id.")
			.Field("dependents", &AssetInfoResult::Dependents, "The assets that reference it, sorted by id.")
			.Field("dependencyFiles", &AssetInfoResult::DependencyFiles, "Its dependency files (a glTF's buffers and images), project-relative, sorted.")
			.Field("diagnostics", &AssetInfoResult::Diagnostics, "Its diagnostics.")
			.Field("state", &AssetInfoResult::State, "Whether it is loaded.")
			.Field("version", &AssetInfoResult::Version, "Its version: 0 until loaded, +1 for every reload.");

		registry.Struct<AssetImportParams>("AssetImportParams", "The params of asset.import.")
			.Field("source", &AssetImportParams::Source, "The file to import: an absolute native path, or a project path.")
			.Field("destDir", &AssetImportParams::DestDir,
				"The project-relative folder below Assets/ to copy it into; created when missing. With replace, the folder holding the asset to replace "
				"(the file of the source's name there), or that asset's source file itself.")
			.Field("settings", &AssetImportParams::Settings,
				"Import settings merged over the importer's defaults; null for the defaults (with replace, null keeps the asset's settings).")
			.Field("replace", &AssetImportParams::Replace,
				"Give an imported asset this file's bytes instead of importing a new one: the file (and a glTF's changed buffers and images) is written "
				"over the asset's, which keeps its .meta, handle and settings, and is reimported; sub-assets keep their handles wherever their keys "
				"still exist. The new file is imported in memory first: one that does not import is refused (ImportFailed) and nothing is written. "
				"Only the files whose bytes change are written, as one undoable command; dryRun previews it.");

		registry.Struct<AssetSubAssetInfo>("AssetSubAssetInfo", "A sub-asset of a replaced source, by its key.")
			.Field("key", &AssetSubAssetInfo::Key, "Its stable key in the source, such as mesh:0:Strip.")
			.Field("id", &AssetSubAssetInfo::Id, "Its handle, derived from the source's handle and the key.")
			.Field("type", &AssetSubAssetInfo::Type, "Its type.");

		registry.Struct<AssetImportResult>("AssetImportResult", "The imported asset.")
			.Field("asset", &AssetImportResult::Asset, "The imported asset.")
			.Field("subAssets", &AssetImportResult::SubAssets, "Its sub-assets.")
			.Field("copiedFiles", &AssetImportResult::CopiedFiles,
				"The files copied with their .meta files, project-relative, sorted; with replace, the files written (in a dry run, those it would write).")
			.Field("diagnostics", &AssetImportResult::Diagnostics, "The importer's warnings.")
			.Field("undoIndex", &AssetImportResult::UndoIndex, "The undo index of the copy; 0 in a batch, in a dry run, and for a replace that changed no file.")
			.Field("keptSubAssets", &AssetImportResult::KeptSubAssets,
				"With replace: the sub-assets whose keys the new version still has, their handles unchanged, so references to them keep working.")
			.Field("addedSubAssets", &AssetImportResult::AddedSubAssets, "With replace: the sub-assets the new version adds.")
			.Field("removedSubAssets", &AssetImportResult::RemovedSubAssets,
				"With replace: the sub-assets the new version no longer has; project.validate reports what still references them (ASSET_MISSING).")
			.Field("unreferencedFiles", &AssetImportResult::UnreferencedFiles,
				"With replace: the files the previous version referenced that the new one does not, left in place (never deleted).")
			.Field("rebuildDiagnostics", &AssetImportResult::RebuildDiagnostics,
				"With replace of a prefab: the diagnostics of the rebuilds of the open scenes' instances, every override they dropped among them (also logged).");

		registry.Struct<AssetReimportParams>("AssetReimportParams", "The params of asset.reimport.")
			.Field("asset", &AssetReimportParams::Asset, "The asset to import again (a source, not a sub-asset).");

		registry.Struct<AssetReimportResult>("AssetReimportResult", "The reimported asset.")
			.Field("asset", &AssetReimportResult::Asset, "The reimported asset.")
			.Field("subAssets", &AssetReimportResult::SubAssets, "Its sub-assets.")
			.Field("diagnostics", &AssetReimportResult::Diagnostics, "The importer's warnings.")
			.Field("rebuildDiagnostics", &AssetReimportResult::RebuildDiagnostics,
				"For a prefab: the diagnostics of the rebuilds of the open scenes' instances, every override they dropped among them (also logged).");

		registry.Struct<AssetCreateParams>("AssetCreateParams", "The params of asset.create.")
			.Field("type", &AssetCreateParams::Type, "What to create.")
			.Field("path", &AssetCreateParams::Path,
				"The project-relative path below Assets/, with the type's extension (.material, .scene, .prefab, .sfx, .spritesheet, .tilemap, "
				".stringtable; none for a folder); missing parent folders are created with it.")
			.Field("values", &AssetCreateParams::Values,
				"A Material's, a SoundEffect's, a SpriteSheet's, a Tilemap's or a StringTable's fields over the defaults (registry names, such as "
				"Roughness, Layers, which a sound effect needs, a sheet's Texture, Frames and Clips, whose frames must lie inside a texture that "
				"loads, a tilemap's CellSize, ChunkSize, Palette and Chunks, or a table's Locales and Entries; a material's maps, a sheet's Texture "
				"and a palette entry's Sheet take any asset reference: a handle, an Assets/ path or an engine:// path; a sheet's imported Texture "
				"is set to Compression None in the same step, so sprite art is never block-compressed). A Scene, a Prefab, a Heightfield and a "
				"Folder take no values and refuse them: a prefab is created with one empty root entity, and prefab.create saves an entity "
				"built in a scene, its components and children, as a prefab; a heightfield is created flat, and terrain.importHeightmap "
				"converts a heightmap into one and terrain.edit sculpts it.");

		registry.Struct<AssetCreateResult>("AssetCreateResult", "The created asset.")
			.Field("asset", &AssetCreateResult::Asset, "The created asset; an empty id and type None for a folder.")
			.Field("path", &AssetCreateResult::Path, "The project-relative path created.")
			.Field("undoIndex", &AssetCreateResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch, and when nothing was created.")
			.Field("created", &AssetCreateResult::Created,
				"Whether anything was created: false only for a Folder whose path is already a folder, which succeeds without a command.");

		registry.Struct<AssetGetPropertiesParams>("AssetGetPropertiesParams", "The params of asset.getProperties.")
			.Field("asset", &AssetGetPropertiesParams::Asset, "The native asset (a material, a sound effect, a sprite sheet, a tilemap or a string table).");

		registry.Struct<AssetGetPropertiesResult>("AssetGetPropertiesResult", "A native asset's properties.")
			.Field("asset", &AssetGetPropertiesResult::Asset, "The asset.")
			.Field("values", &AssetGetPropertiesResult::Values, "Every property, by registry name.");

		registry.Struct<AssetSetPropertiesParams>("AssetSetPropertiesParams", "The params of asset.setProperties.")
			.Field("asset", &AssetSetPropertiesParams::Asset, "The native asset (a material, a sound effect, a sprite sheet, a tilemap or a string table).")
			.Field("values", &AssetSetPropertiesParams::Values,
				"An RFC 7386 merge patch of its properties: members replace, null resets a field, an array is replaced whole; a material's maps, a "
				"sprite sheet's Texture and a tilemap palette entry's Sheet take any asset reference (a handle, an Assets/ path or an engine:// "
				"path); a sprite sheet's imported Texture is set to Compression None in the same step; a tilemap's ChunkSize set without Chunks "
				"keeps its painted cells, laid in chunks of the new size.");

		registry.Struct<AssetSetPropertiesResult>("AssetSetPropertiesResult", "The properties after the patch.")
			.Field("asset", &AssetSetPropertiesResult::Asset, "The asset.")
			.Field("values", &AssetSetPropertiesResult::Values, "Every property after the patch.")
			.Field("undoIndex", &AssetSetPropertiesResult::UndoIndex, "The command's undo index; 0 when nothing changed, in a dry run or a batch.");

		registry.Struct<AssetGetImportSettingsParams>("AssetGetImportSettingsParams", "The params of asset.getImportSettings.")
			.Field("asset", &AssetGetImportSettingsParams::Asset, "The asset (a source, not a sub-asset).");

		registry.Struct<AssetGetImportSettingsResult>("AssetGetImportSettingsResult", "An asset's import settings.")
			.Field("asset", &AssetGetImportSettingsResult::Asset, "The asset.")
			.Field("importer", &AssetGetImportSettingsResult::Importer, "The importer's id.")
			.Field("settings", &AssetGetImportSettingsResult::Settings, "Every field of the importer's settings; null without settings.");

		registry.Struct<AssetSetImportSettingsParams>("AssetSetImportSettingsParams", "The params of asset.setImportSettings.")
			.Field("asset", &AssetSetImportSettingsParams::Asset, "The asset (a source, not a sub-asset).")
			.Field("settings", &AssetSetImportSettingsParams::Settings, "An RFC 7386 merge patch of the import settings.");

		registry.Struct<AssetSetImportSettingsResult>("AssetSetImportSettingsResult", "The import settings after the patch.")
			.Field("asset", &AssetSetImportSettingsResult::Asset, "The asset.")
			.Field("importer", &AssetSetImportSettingsResult::Importer, "The importer's id.")
			.Field("settings", &AssetSetImportSettingsResult::Settings, "Every field after the patch.")
			.Field("undoIndex", &AssetSetImportSettingsResult::UndoIndex, "The undo index; 0 when nothing changed or in a batch.")
			.Field("rebuildDiagnostics", &AssetSetImportSettingsResult::RebuildDiagnostics,
				"For a prefab: the diagnostics of the rebuilds of the open scenes' instances, every override they dropped among them (also logged).");

		registry.Struct<AssetMoveParams>("AssetMoveParams", "The params of asset.move.")
			.Field("asset", &AssetMoveParams::Asset, "The asset to move (a source, not a sub-asset).")
			.Field("path", &AssetMoveParams::Path, "Its new project-relative path below Assets/, with the same extension.");

		registry.Struct<AssetMoveResult>("AssetMoveResult", "The moved asset.")
			.Field("asset", &AssetMoveResult::Asset, "The asset at its new path, with its handle unchanged.")
			.Field("movedFiles", &AssetMoveResult::MovedFiles, "The new paths of every file moved, .meta files and dependency files included.")
			.Field("undoIndex", &AssetMoveResult::UndoIndex, "The command's undo index; 0 in a batch.");

		registry.Struct<AssetDeleteParams>("AssetDeleteParams", "The params of asset.delete.")
			.Field("asset", &AssetDeleteParams::Asset, "The asset to move to the trash (a source, not a sub-asset), or a folder below Assets/ that holds no files, to remove.");

		registry.Struct<AssetDeleteResult>("AssetDeleteResult", "The deleted asset.")
			.Field("asset", &AssetDeleteResult::Asset, "The asset as it was; for a folder its path, with an empty id and type None.")
			.Field("trashDirectory", &AssetDeleteResult::TrashDirectory, "Where its files went: \"Library/Trash/<entry>\"; empty for a folder, which holds none.")
			.Field("trashedFiles", &AssetDeleteResult::TrashedFiles, "The old paths of every file trashed, .meta files and dependency files included.")
			.Field("removedFolders", &AssetDeleteResult::RemovedFolders, "A folder delete: the folder and every empty folder below it, project-relative and sorted; empty for an asset.")
			.Field("undoIndex", &AssetDeleteResult::UndoIndex, "The command's undo index; 0 in a batch.");
	}

	void RegisterAssetMethods(MethodRegistry& methods)
	{
		Json listExample = Json::object();
		listExample["dir"] = "Assets/Materials";
		listExample["type"] = "Material";
		Json builtinListExample = Json::object();
		builtinListExample["dir"] = "engine://Environments";
		methods.Add(
			{
				.Name = "asset.list",
				.Description = "Lists the project's assets below a folder, or the built-in assets below engine:// (such as engine://Environments), "
							   "optionally of one type, sorted by path with each glTF's sub-assets after it; paged with limit and cursor.",
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "List the materials.", .Params = listExample },
					{ .Description = "List the built-in environments.", .Params = builtinListExample } },
			},
			&Automation::AssetList);

		Json infoExample = Json::object();
		infoExample["asset"] = "Assets/Models/Track.glb";
		methods.Add(
			{
				.Name = "asset.info",
				.Description = "Reports an asset's source, importer, import settings, sub-assets, dependencies, dependents, dependency files, "
							   "diagnostics, load state and version.",
				.RequiredParams = { "asset" },
				.AllowedInBatch = true,
				.Examples = { { .Description = "Inspect a glTF model.", .Params = infoExample } },
			},
			&Automation::AssetInfo);

		Json importExample = Json::object();
		importExample["source"] = "C:/Downloads/Kart/Kart.gltf";
		importExample["destDir"] = "Assets/Models";
		Json replaceExample = importExample;
		replaceExample["replace"] = true;
		methods.AddPending<EditorMethodContext, AssetImportParams, AssetImportResult>(
			{
				.Name = "asset.import",
				.Description = "Copies a file (and a glTF's buffers and images) into a folder below Assets/ with its .meta files as one "
							   "undoable command, then imports it; resolves when the import completed. With replace: true it gives an imported "
							   "asset the file's new bytes instead, keeping its handle, settings and every sub-asset handle whose key remains, and "
							   "lists the sub-assets kept, added and removed; a new file that does not import is refused (ImportFailed) and nothing "
							   "is written (dryRun previews a replace).",
				.RequiredParams = { "source", "destDir" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.TimeoutSeconds = 300,
				.Examples = { { .Description = "Import a glTF model with its textures.", .Params = importExample },
					{ .Description = "Replace the imported Assets/Models/Kart.gltf with a new version of the file.", .Params = replaceExample } },
			},
			&Automation::AssetImport);

		Json reimportExample = Json::object();
		reimportExample["asset"] = "Assets/Models/Track.glb";
		methods.AddPending<EditorMethodContext, AssetReimportParams, AssetReimportResult>(
			{
				.Name = "asset.reimport",
				.Description = "Imports an asset again, bypassing the cache; for a changed prefab, rewrites the prefabs nesting it and updates "
							   "the instances of every open scene, each scene's as an undo step of its own; resolves when the import completed.",
				.RequiredParams = { "asset" },
				.Mutates = true,
				.TimeoutSeconds = 300,
				.Examples = { { .Description = "Import a model again.", .Params = reimportExample } },
			},
			&Automation::AssetReimport);

		Json createExample = Json::object();
		createExample["type"] = "Material";
		createExample["path"] = "Assets/Materials/Red.material";
		createExample["values"] = Json::object();
		createExample["values"]["BaseColor"] = Json::array({ 0.9, 0.15, 0.15, 1.0 });
		createExample["values"]["Roughness"] = 0.45;
		Json soundExample = Json::object();
		soundExample["type"] = "SoundEffect";
		soundExample["path"] = "Assets/Audio/Coin.sfx";
		soundExample["values"] = Json::object();
		soundExample["values"]["Layers"] = Json::array({ Json::object() });
		soundExample["values"]["Layers"][0]["Wave"] = "Square";
		soundExample["values"]["Layers"][0]["Notes"] = Json::array({ "B5:0.05", "E6:0.2" });
		Json sheetExample = Json::object();
		sheetExample["type"] = "SpriteSheet";
		sheetExample["path"] = "Assets/Sprites/Hero.spritesheet";
		sheetExample["values"] = Json::object();
		sheetExample["values"]["Texture"] = "4a1c0e7d2b9f3a65";
		Json idle0 = Json::object();
		idle0["Name"] = "Idle0";
		idle0["Rect"] = Json::object();
		idle0["Rect"]["X"] = 0;
		idle0["Rect"]["Y"] = 0;
		idle0["Rect"]["Width"] = 16;
		idle0["Rect"]["Height"] = 16;
		idle0["Pivot"] = Json::array({ 0.5, 1.0 });
		Json idle1 = idle0;
		idle1["Name"] = "Idle1";
		idle1["Rect"]["X"] = 16;
		sheetExample["values"]["Frames"] = Json::array({ idle0, idle1 });
		Json idleClip = Json::object();
		idleClip["Name"] = "Idle";
		idleClip["Frames"] = Json::array({ Json::object(), Json::object() });
		idleClip["Frames"][0]["Frame"] = 0;
		idleClip["Frames"][0]["Ticks"] = 10;
		idleClip["Frames"][1]["Frame"] = 1;
		idleClip["Frames"][1]["Ticks"] = 10;
		sheetExample["values"]["Clips"] = Json::array({ idleClip });
		Json tableExample = Json::object();
		tableExample["type"] = "StringTable";
		tableExample["path"] = "Assets/Text/Menu.stringtable";
		tableExample["values"] = Json::object();
		tableExample["values"]["Locales"] = Json::array({ "en", "fr" });
		Json startEntry = Json::object();
		startEntry["Key"] = "Menu.Start";
		startEntry["Values"] = Json::object();
		startEntry["Values"]["en"] = "Start";
		startEntry["Values"]["fr"] = "Commencer";
		tableExample["values"]["Entries"] = Json::array({ startEntry });
		Json heightfieldExample = Json::object();
		heightfieldExample["type"] = "Heightfield";
		heightfieldExample["path"] = "Assets/Terrain/Ground.heightfield";
		methods.Add(
			{
				.Name = "asset.create",
				.Description = "Creates a material, a sound effect, a sprite sheet, a tilemap or a string table (with values over the defaults), an empty "
							   "scene, a prefab of one empty root entity (no values: prefab.create saves a built entity as a prefab), a flat heightfield (no "
							   "values: terrain.importHeightmap converts a heightmap into one and terrain.edit sculpts it) or a folder below Assets/, with its "
							   ".meta and any missing parent folders, as one undoable command. A folder that already exists succeeds without a command "
							   "(created: false), so a Folder op is needed only for an empty folder.",
				.RequiredParams = { "type", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Create a red material.", .Params = createExample },
					{ .Description = "Create a two-note coin pickup sound.", .Params = soundExample },
					{ .Description = "Create a sprite sheet with two 16 x 16 frames of a texture and a looping clip over them.", .Params = sheetExample },
					{ .Description = "Create a string table with an English and a French string for one key.", .Params = tableExample },
					{ .Description = "Create a flat heightfield for a Terrain.", .Params = heightfieldExample } },
			},
			&Automation::AssetCreate);

		Json getPropertiesExample = Json::object();
		getPropertiesExample["asset"] = "Assets/Materials/Red.material";
		methods.Add(
			{
				.Name = "asset.getProperties",
				.Description = "Returns every property of a native asset (a material, a sound effect, a sprite sheet, a tilemap or a string table) by registry name.",
				.RequiredParams = { "asset" },
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read a material.", .Params = getPropertiesExample } },
			},
			&Automation::AssetGetProperties);

		Json setPropertiesExample = Json::object();
		setPropertiesExample["asset"] = "Assets/Materials/Red.material";
		setPropertiesExample["values"] = Json::object();
		setPropertiesExample["values"]["Metallic"] = 1.0;
		methods.Add(
			{
				.Name = "asset.setProperties",
				.Description = "Applies an RFC 7386 merge patch to a native asset's properties (a material, a sound effect, a sprite sheet, a tilemap or "
							   "a string table; an array such as a sheet's Frames or a tilemap's Palette is replaced whole) and writes it, as one undoable "
							   "command. A running lockstep, recording, replay or test session sees the change only after it ends: asset reloads are deferred "
							   "like script reloads.",
				.RequiredParams = { "asset", "values" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Make a material metallic.", .Params = setPropertiesExample } },
			},
			&Automation::AssetSetProperties);

		Json getSettingsExample = Json::object();
		getSettingsExample["asset"] = "Assets/Textures/Wood.png";
		methods.Add(
			{
				.Name = "asset.getImportSettings",
				.Description = "Returns the importer and every import setting of an asset's .meta.",
				.RequiredParams = { "asset" },
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read a texture's import settings.", .Params = getSettingsExample } },
			},
			&Automation::AssetGetImportSettings);

		Json setSettingsExample = Json::object();
		setSettingsExample["asset"] = "Assets/Textures/WoodNormal.png";
		setSettingsExample["settings"] = Json::object();
		setSettingsExample["settings"]["Usage"] = "NormalMap";
		methods.Add(
			{
				.Name = "asset.setImportSettings",
				.Description = "Applies an RFC 7386 merge patch to an asset's import settings as one undoable command; the asset reimports. "
							   "For a glTF that an open scene instantiates or a prefab nests, the prefabs nesting it and the instances follow in the "
							   "same undo step, and the instances of the other open scenes in their own undo histories.",
				.RequiredParams = { "asset", "settings" },
				.Mutates = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Import a texture as a normal map.", .Params = setSettingsExample } },
			},
			&Automation::AssetSetImportSettings);

		Json moveExample = Json::object();
		moveExample["asset"] = "Assets/Red.material";
		moveExample["path"] = "Assets/Materials/Red.material";
		methods.Add(
			{
				.Name = "asset.move",
				.Description = "Moves an asset with its .meta (and a glTF's dependency files) to another path below Assets/ as one undoable "
							   "command; its handle and every reference to it stay valid.",
				.RequiredParams = { "asset", "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Move a material into the Materials folder.", .Params = moveExample } },
			},
			&Automation::AssetMove);

		Json deleteExample = Json::object();
		deleteExample["asset"] = "Assets/Materials/Old.material";
		methods.Add(
			{
				.Name = "asset.delete",
				.Description = "Moves an asset with its .meta (and a glTF's dependency files) to Library/Trash as one undoable command; never "
							   "a permanent delete. A folder below Assets/ that holds no files (only empty folders) is removed, also undoably; a folder "
							   "that holds files is refused (Validation, naming them): delete its assets first.",
				.RequiredParams = { "asset" },
				.ExposeAsTool = true,
				.Mutates = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Delete an unused material.", .Params = deleteExample } },
			},
			&Automation::AssetDelete);
	}

}
