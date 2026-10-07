#include "TestsPCH.h"

#include "Engine/Asset/BuiltinAssets.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/FileSystem.h"
#include "Support/TestData.h"

#include <algorithm>
#include <format>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	static constexpr std::string_view ProceduralCubeEntry = R"({ "Handle": "0000000000000101", "Path": "engine://Meshes/Cube", "Type": "Mesh",
		"Source": "Procedural", "File": "", "Importer": "", "Settings": {}, "Generator": "" })";

	// A catalogue entry with every member; `members` is the text after "Path".
	static std::string MakeEntry(std::string_view handle, std::string_view path, std::string_view members)
	{
		return std::format(R"({{ "Handle": "{}", "Path": "{}", {} }})", handle, path, members);
	}

	// The Default font's entry with the members after "Type".
	static std::string MakeFontEntry(std::string_view members)
	{
		return MakeEntry("00000000000001c1", "engine://Fonts/Default", std::format(R"("Type": "Font", {})", members));
	}

	static std::string MakeCatalogue(std::initializer_list<std::string_view> entries)
	{
		std::string text = R"({ "Format": "EngineAssets", "Version": 1, "Assets": [ )";
		bool first = true;
		for (const std::string_view entry : entries)
		{
			text += first ? "" : ", ";
			text += entry;
			first = false;
		}
		text += " ] }";
		return text;
	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("BuiltinAssets: the handles lie in the reserved range and placeholders map to built-ins")
		{
			CHECK(IsBuiltinAssetHandle(BuiltinAssetHandles::CubeMesh));
			CHECK(IsBuiltinAssetHandle(BuiltinAssetHandles::SkyEnvironment));
			CHECK_FALSE(IsBuiltinAssetHandle(AssetHandle()));
			CHECK_FALSE(IsBuiltinAssetHandle(AssetHandle(0x400)));
			CHECK(GetPlaceholderHandle(AssetType::Mesh) == BuiltinAssetHandles::CubeMesh);
			CHECK(GetPlaceholderHandle(AssetType::Texture) == BuiltinAssetHandles::MissingTexture);
			CHECK(GetPlaceholderHandle(AssetType::Material) == BuiltinAssetHandles::ErrorMaterial);
			CHECK(GetPlaceholderHandle(AssetType::Font) == BuiltinAssetHandles::DefaultFont);
			CHECK_FALSE(GetPlaceholderHandle(AssetType::Scene).IsValid());
			CHECK_FALSE(GetPlaceholderHandle(AssetType::Script).IsValid());
		}

		TEST_CASE("BuiltinAssets: the procedural entries are the compiled-in meshes, materials and textures")
		{
			const std::span<const BuiltinAssetEntry> entries = GetProceduralBuiltinEntries();
			REQUIRE(entries.size() == 14);
			std::set<std::string> paths;
			for (size_t index = 0; index < entries.size(); ++index)
			{
				CAPTURE(entries[index].Path);
				CHECK(IsBuiltinAssetHandle(entries[index].Handle));
				CHECK(entries[index].Source == BuiltinAssetSource::Procedural);
				CHECK(entries[index].Path.starts_with("engine://"));
				CHECK(entries[index].File.empty());
				CHECK(entries[index].Importer.empty());
				CHECK(entries[index].Settings.IsNull());
				CHECK(entries[index].Generator.empty());
				CHECK(paths.insert(entries[index].Path).second);
				if (index > 0)
					CHECK(entries[index - 1].Handle < entries[index].Handle);
			}
			CHECK(entries.front().Handle == BuiltinAssetHandles::CubeMesh);
			CHECK(entries.front().Path == "engine://Meshes/Cube");
			CHECK(entries.back().Handle == BuiltinAssetHandles::MissingTexture);
		}

		TEST_CASE("BuiltinAssets: EngineAssets.json names exactly the compiled-in handles")
		{
			Result<std::string> text = FileSystem::ReadText(Test::GetRepositoryRoot() / "Resources" / BuiltinAssetCatalog::FileName);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(*text);
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			// The file is canonical (load then save is byte-identical, §6).
			CHECK(catalog->ToText() == *text);
			for (const BuiltinAssetEntry& procedural : GetProceduralBuiltinEntries())
			{
				CAPTURE(procedural.Path);
				const BuiltinAssetEntry* entry = catalog->Find(procedural.Handle);
				REQUIRE(entry != nullptr);
				CHECK(*entry == procedural);
			}
			const BuiltinAssetEntry* font = catalog->Find(BuiltinAssetHandles::DefaultFont);
			REQUIRE(font != nullptr);
			CHECK(font->Path == "engine://Fonts/Default");
			CHECK(font->Source == BuiltinAssetSource::File);
			CHECK(font->File == "Fonts/Inter-Regular.ttf");
			CHECK(font->Importer == "Font");
			CHECK(catalog->FindByPath("engine://Environments/Studio") != nullptr);
			CHECK(catalog->FindByPath("engine://Environments/Sky") != nullptr);
			// Every File entry's file is committed.
			for (const BuiltinAssetEntry& entry : catalog->GetEntries())
			{
				if (entry.Source == BuiltinAssetSource::File)
					CHECK(FileSystem::Exists(Test::GetRepositoryRoot() / "Resources" / entry.File));
			}
			CHECK(catalog->GetEntries().size() == GetProceduralBuiltinEntries().size() + 3);
		}

		TEST_CASE("BuiltinAssets: a catalogue with settings and generated entries round-trips and finds its entries")
		{
			const std::string text = MakeCatalogue({
				ProceduralCubeEntry,
				R"({ "Handle": "0000000000000186", "Path": "engine://Textures/BlueNoise", "Type": "Texture", "Source": "Generated",
				     "File": "", "Importer": "", "Settings": {}, "Generator": "BlueNoise" })",
				R"({ "Handle": "0000000000000202", "Path": "engine://Environments/Sky", "Type": "Environment", "Source": "File",
				     "File": "Environments/Sky.hdr", "Importer": "Environment", "Settings": { "ClampLuminance": true }, "Generator": "" })",
			});
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(text);
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			REQUIRE(catalog->GetEntries().size() == 3);
			const BuiltinAssetEntry* noise = catalog->Find(AssetHandle(0x186));
			REQUIRE(noise != nullptr);
			CHECK(noise->Source == BuiltinAssetSource::Generated);
			CHECK(noise->Generator == "BlueNoise");
			CHECK(noise->Settings.IsNull());
			const BuiltinAssetEntry* sky = catalog->FindByPath("engine://Environments/Sky");
			REQUIRE(sky != nullptr);
			CHECK(sky->Handle == BuiltinAssetHandles::SkyEnvironment);
			CHECK(sky->Type == AssetType::Environment);
			CHECK_FALSE(sky->Settings.IsNull());
			CHECK(catalog->Find(BuiltinAssetHandles::SphereMesh) == nullptr);
			CHECK(catalog->FindByPath("engine://Meshes/Sphere") == nullptr);

			// The canonical text parses back to the same entries and writes the same text.
			const std::string canonical = catalog->ToText();
			CHECK(canonical.contains("\t\t\t\"Settings\": {},\n"));
			Result<BuiltinAssetCatalog> again = BuiltinAssetCatalog::Parse(canonical);
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(std::ranges::equal(again->GetEntries(), catalog->GetEntries()));
			CHECK(again->ToText() == canonical);
		}

		TEST_CASE("BuiltinAssets: malformed catalogues are located errors")
		{
			struct BadCatalogue
			{
				std::string Name{};
				std::string Text{};
				ErrorCode Code = ErrorCode::Validation;
				std::string Pointer{};
			};
			const std::string sphere = MakeEntry("0000000000000102", "engine://Meshes/Sphere",
				R"("Type": "Mesh", "Source": "Procedural", "File": "", "Importer": "", "Settings": {}, "Generator": "")");
			const std::string fontMembers = R"("Source": "File", "File": "Fonts/A.ttf", "Importer": "Font", "Settings": {}, "Generator": "")";
			const std::vector<BadCatalogue> cases = {
				{ "not JSON", "{", ErrorCode::Parse, "" },
				{ "wrong format", R"({ "Format": "Project", "Version": 1, "Assets": [] })", ErrorCode::Validation, "/Format" },
				{ "newer version", R"({ "Format": "EngineAssets", "Version": 2, "Assets": [] })", ErrorCode::UnsupportedVersion, "/Version" },
				{ "unknown root member", R"({ "Format": "EngineAssets", "Version": 1, "Assets": [], "Extra": 1 })", ErrorCode::Validation, "" },
				{ "assets not an array", R"({ "Format": "EngineAssets", "Version": 1, "Assets": {} })", ErrorCode::Validation, "/Assets" },
				{ "repeated handle", MakeCatalogue({ ProceduralCubeEntry, ProceduralCubeEntry }), ErrorCode::Validation, "/Assets/1" },
				{ "unsorted", MakeCatalogue({ sphere, ProceduralCubeEntry }), ErrorCode::Validation, "/Assets/1" },
				{ "repeated path",
					MakeCatalogue({ ProceduralCubeEntry, MakeEntry("00000000000001c1", "engine://Meshes/Cube", std::format(R"("Type": "Font", {})", fontMembers)) }),
					ErrorCode::Validation, "/Assets/1" },
				{ "outside the range", MakeCatalogue({ MakeEntry("0000000000000400", "engine://Fonts/Default", std::format(R"("Type": "Font", {})", fontMembers)) }),
					ErrorCode::Validation, "/Assets/0/Handle" },
				{ "null handle", MakeCatalogue({ MakeEntry("0000000000000000", "engine://Fonts/Default", std::format(R"("Type": "Font", {})", fontMembers)) }),
					ErrorCode::Validation, "/Assets/0/Handle" },
				{ "not an engine path",
					MakeCatalogue({ MakeEntry("00000000000001c1", "Assets/Default.ttf", std::format(R"("Type": "Font", {})", fontMembers)) }),
					ErrorCode::Validation, "/Assets/0/Path" },
				{ "a sub-asset path",
					MakeCatalogue({ MakeEntry("00000000000001c1", "engine://Fonts/Default#atlas", std::format(R"("Type": "Font", {})", fontMembers)) }),
					ErrorCode::Validation, "/Assets/0/Path" },
				{ "unknown type",
					MakeCatalogue({ MakeEntry("00000000000001c1", "engine://Fonts/Default", std::format(R"("Type": "Typeface", {})", fontMembers)) }),
					ErrorCode::Validation, "/Assets/0/Type" },
				{ "type None", MakeCatalogue({ MakeEntry("00000000000001c1", "engine://Fonts/Default", std::format(R"("Type": "None", {})", fontMembers)) }),
					ErrorCode::Validation, "/Assets/0/Type" },
				{ "unknown source",
					MakeCatalogue({ MakeFontEntry(R"("Source": "Disk", "File": "Fonts/A.ttf", "Importer": "Font", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0/Source" },
				{ "file without a file", MakeCatalogue({ MakeFontEntry(R"("Source": "File", "File": "", "Importer": "Font", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0/File" },
				{ "file without importer",
					MakeCatalogue({ MakeFontEntry(R"("Source": "File", "File": "Fonts/A.ttf", "Importer": "", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0/Importer" },
				{ "file with generator",
					MakeCatalogue({ MakeFontEntry(R"("Source": "File", "File": "Fonts/A.ttf", "Importer": "Font", "Settings": {}, "Generator": "X")") }),
					ErrorCode::Validation, "/Assets/0/Generator" },
				{ "file escaping", MakeCatalogue({ MakeFontEntry(R"("Source": "File", "File": "../A.ttf", "Importer": "Font", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0/File" },
				{ "settings not an object",
					MakeCatalogue({ MakeFontEntry(R"("Source": "File", "File": "Fonts/A.ttf", "Importer": "Font", "Settings": [], "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0/Settings" },
				{ "generated without generator",
					MakeCatalogue({ MakeFontEntry(R"("Source": "Generated", "File": "", "Importer": "", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0/Generator" },
				{ "generated with a file",
					MakeCatalogue({ MakeFontEntry(R"("Source": "Generated", "File": "Fonts/A.ttf", "Importer": "", "Settings": {}, "Generator": "X")") }),
					ErrorCode::Validation, "/Assets/0/File" },
				{ "generated with settings",
					MakeCatalogue({ MakeFontEntry(R"("Source": "Generated", "File": "", "Importer": "", "Settings": { "Size": 1 }, "Generator": "X")") }),
					ErrorCode::Validation, "/Assets/0/Settings" },
				{ "procedural with an importer",
					MakeCatalogue({ MakeEntry("0000000000000101", "engine://Meshes/Cube",
						R"("Type": "Mesh", "Source": "Procedural", "File": "", "Importer": "Gltf", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0/Importer" },
				{ "procedural the engine cannot generate",
					MakeCatalogue({ MakeFontEntry(R"("Source": "Procedural", "File": "", "Importer": "", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0" },
				{ "procedural of the wrong type",
					MakeCatalogue({ MakeEntry("0000000000000101", "engine://Meshes/Cube",
						R"("Type": "Texture", "Source": "Procedural", "File": "", "Importer": "", "Settings": {}, "Generator": "")") }),
					ErrorCode::Validation, "/Assets/0" },
				{ "missing member",
					MakeCatalogue({ MakeEntry("0000000000000101", "engine://Meshes/Cube",
						R"("Type": "Mesh", "Source": "Procedural", "File": "", "Importer": "", "Settings": {})") }),
					ErrorCode::Validation, "/Assets/0" },
				{ "unknown entry member",
					MakeCatalogue({ MakeEntry("0000000000000101", "engine://Meshes/Cube",
						R"("Type": "Mesh", "Source": "Procedural", "File": "", "Importer": "", "Settings": {}, "Generator": "", "Size": 1)") }),
					ErrorCode::Validation, "/Assets/0" },
			};
			for (const BadCatalogue& bad : cases)
			{
				CAPTURE(bad.Name);
				const Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(bad.Text);
				REQUIRE_FALSE(catalog.has_value());
				CAPTURE(catalog.error().ToString());
				CHECK(catalog.error().GetCode() == bad.Code);
				if (bad.Code != ErrorCode::Parse)
				{
					REQUIRE(catalog.error().GetLocation().JsonPointer.has_value());
					CHECK(*catalog.error().GetLocation().JsonPointer == bad.Pointer);
				}
			}
		}

		TEST_CASE("BuiltinAssets: every procedural built-in is generated and valid")
		{
			for (const BuiltinAssetEntry& entry : GetProceduralBuiltinEntries())
			{
				CAPTURE(entry.Path);
				Result<AssetRef<Asset>> asset = CreateProceduralBuiltinAsset(entry.Handle);
				REQUIRE_MESSAGE(asset.has_value(), asset.error().ToString());
				CHECK((*asset)->GetAssetType() == entry.Type);
				if (const AssetRef<MeshData> mesh = AssetCast<MeshData>(*asset))
					CHECK(ValidateMeshData(*mesh).has_value());
				if (const AssetRef<TextureData> texture = AssetCast<TextureData>(*asset))
					CHECK(ValidateTextureData(*texture).has_value());
			}
			Result<AssetRef<Asset>> notProcedural = CreateProceduralBuiltinAsset(BuiltinAssetHandles::DefaultFont);
			REQUIRE_FALSE(notProcedural.has_value());
			CHECK(notProcedural.error().GetCode() == ErrorCode::NotFound);
			const MaterialData error = CreateBuiltinMaterial(BuiltinMaterial::Error);
			CHECK(error.Emissive != glm::vec3(0.0f));
			const MaterialData standard = CreateBuiltinMaterial(BuiltinMaterial::Default);
			CHECK(standard.BaseColor == glm::vec4(1.0f));
			CHECK(standard.Roughness == 0.5f);
		}
	}

}
