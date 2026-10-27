#include "EnginePCH.h"
#include "Engine/Asset/SourceFormats.h"

#include <algorithm>
#include <array>

namespace Engine {

	namespace {

		// RegisterBuiltinImporters' importers and extensions (AssetPipeline/ImporterRegistry.cpp), sorted by (Importer,
		// Extension) byte-wise; SourceFormatsTests keeps the two lists equal.
		constexpr auto SourceFormatTable = std::to_array<SourceFormat>({
			{ .Importer = "Audio", .Extension = ".flac", .MainType = AssetType::AudioClip, .Importable = true },
			{ .Importer = "Audio", .Extension = ".mp3", .MainType = AssetType::AudioClip, .Importable = true },
			{ .Importer = "Audio", .Extension = ".ogg", .MainType = AssetType::AudioClip, .Importable = true },
			{ .Importer = "Audio", .Extension = ".wav", .MainType = AssetType::AudioClip, .Importable = true },
			// EnvironmentImporter claims .exr only to refuse it with a hint ("OpenEXR is not supported").
			{ .Importer = "Environment", .Extension = ".exr", .MainType = AssetType::Environment, .Importable = false },
			{ .Importer = "Environment", .Extension = ".hdr", .MainType = AssetType::Environment, .Importable = true },
			{ .Importer = "Font", .Extension = ".otf", .MainType = AssetType::Font, .Importable = true },
			{ .Importer = "Font", .Extension = ".ttf", .MainType = AssetType::Font, .Importable = true },
			{ .Importer = "Gltf", .Extension = ".glb", .MainType = AssetType::Prefab, .Importable = true },
			{ .Importer = "Gltf", .Extension = ".gltf", .MainType = AssetType::Prefab, .Importable = true },
			{ .Importer = "Material", .Extension = ".material", .MainType = AssetType::Material, .Importable = true },
			{ .Importer = "Prefab", .Extension = ".prefab", .MainType = AssetType::Prefab, .Importable = true },
			{ .Importer = "Replay", .Extension = ".replay", .MainType = AssetType::Replay, .Importable = true },
			{ .Importer = "Scene", .Extension = ".scene", .MainType = AssetType::Scene, .Importable = true },
			{ .Importer = "Script", .Extension = ".luau", .MainType = AssetType::Script, .Importable = true },
			{ .Importer = "SoundEffect", .Extension = ".sfx", .MainType = AssetType::AudioClip, .Importable = true },
			{ .Importer = "SpriteSheet", .Extension = ".spritesheet", .MainType = AssetType::SpriteSheet, .Importable = true },
			{ .Importer = "Texture", .Extension = ".bmp", .MainType = AssetType::Texture, .Importable = true },
			{ .Importer = "Texture", .Extension = ".jpeg", .MainType = AssetType::Texture, .Importable = true },
			{ .Importer = "Texture", .Extension = ".jpg", .MainType = AssetType::Texture, .Importable = true },
			{ .Importer = "Texture", .Extension = ".png", .MainType = AssetType::Texture, .Importable = true },
			{ .Importer = "Texture", .Extension = ".tga", .MainType = AssetType::Texture, .Importable = true },
		});

		constexpr char ToLowerAscii(char c)
		{
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
		}

	}

	std::span<const SourceFormat> GetSourceFormats()
	{
		return SourceFormatTable;
	}

	const SourceFormat* FindSourceFormat(std::string_view extension)
	{
		if (extension.size() < 2 || extension.front() != '.')
			return nullptr;
		const auto found = std::ranges::find_if(SourceFormatTable, [extension](const SourceFormat& format)
		{
			return std::ranges::equal(format.Extension, extension, [](char a, char b)
			{
				return a == ToLowerAscii(b);
			});
		});
		return found == SourceFormatTable.end() ? nullptr : &*found;
	}

}
