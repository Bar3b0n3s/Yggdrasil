#include "TestsPCH.h"

#include "EditorCore/Automation/TerrainMethods.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// terrain.importHeightmap (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 20, stream T-A): its C++ round
// trip (a 16-bit PNG and a RAW converted into one heightfield, a heightfield replaced in place, one undo step each) and its
// refusals at their pointers.

namespace Engine {

	namespace {

		constexpr std::string_view RampPath = "Assets/Terrain/Ramp.heightfield";

		Json CallTerrainOrFail(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			Result<Json> result = setup.Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		VfsPath ProjectPath(std::string_view relative)
		{
			return VfsPath::Create("project", relative).value_or(VfsPath());
		}

		// Writes `bytes` as the project file `relative`, its folder created first.
		void WriteProjectBytes(Test::AutomationFixture& setup, std::string_view relative, std::span<const std::byte> bytes)
		{
			const VfsPath path = ProjectPath(relative);
			REQUIRE(setup.GetEditor().CreateProjectDirectory(path.GetParent()).has_value());
			REQUIRE(setup.GetEditor().WriteProjectFile(path, bytes).has_value());
		}

		// The fixture ramp of Tests/Data/Assets/Heightmaps/Ramp16.png as RAW bytes: sample (x, z) = x * 4000 + z, little-endian,
		// row 0 first.
		Buffer MakeRampRaw()
		{
			Buffer raw;
			for (uint32_t z = 0; z < 17; ++z)
			{
				for (uint32_t x = 0; x < 17; ++x)
				{
					const uint32_t sample = x * 4000 + z;
					raw.push_back(static_cast<std::byte>(sample & 0xFFU));
					raw.push_back(static_cast<std::byte>(sample >> 8U));
				}
			}
			return raw;
		}

		Buffer ReadProjectBytes(Test::AutomationFixture& setup, std::string_view relative)
		{
			const Result<Buffer> bytes = setup.GetEditor().GetVfs().ReadFile(ProjectPath(relative));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return *bytes;
		}

		// The heightfield the editor's asset manager has loaded for `relative`.
		AssetRef<HeightfieldData> LoadHeightfield(Test::AutomationFixture& setup, std::string_view relative)
		{
			EditorAssetManager& assets = setup.GetEditor().GetAssets();
			const std::optional<AssetHandle> handle = assets.Resolve(relative);
			REQUIRE(handle.has_value());
			const Result<AssetRef<Asset>> loaded = assets.Load(*handle);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<HeightfieldData> heightfield = AssetCast<HeightfieldData>(*loaded);
			REQUIRE(heightfield != nullptr);
			return heightfield;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TerrainMethods: terrain.importHeightmap converts a 16-bit PNG and a RAW into one heightfield, replaces it in place and undoes")
		{
			Test::AutomationFixture setup("TerrainImportHeightmap");
			const Result<Buffer> png = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Heightmaps/Ramp16.png"));
			REQUIRE_MESSAGE(png.has_value(), png.error().ToString());
			WriteProjectBytes(setup, "Heightmaps/Ramp16.png", *png);
			const Buffer raw = MakeRampRaw();
			WriteProjectBytes(setup, "Heightmaps/Ramp.r16", raw);

			// The PNG: a new heightfield with its .meta, one undo step, imported and loaded at 17 samples a side.
			const size_t history = setup.GetEditor().GetHistory().GetUndoCount();
			const Json imported = CallTerrainOrFail(setup, "terrain.importHeightmap",
				Json{ { "source", "Heightmaps/Ramp16.png" }, { "path", std::string(RampPath) } });
			CHECK(imported["resolution"] == Json(17));
			CHECK(imported["asset"]["type"] == Json("Heightfield"));
			CHECK(imported["asset"]["path"] == Json(std::string(RampPath)));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history + 1);
			const Buffer fromPng = ReadProjectBytes(setup, RampPath);
			const Result<HeightfieldData> ramp = ReadHeightfieldSource(fromPng);
			REQUIRE_MESSAGE(ramp.has_value(), ramp.error().ToString());
			CHECK(ramp->Resolution == 17);
			CHECK(ramp->Heights[3 + 17 * 2] == 3 * 4000 + 2);
			CHECK(LoadHeightfield(setup, RampPath)->Heights == ramp->Heights);
			CHECK(setup.GetEditor().GetVfs().Exists(ProjectPath("Assets/Terrain/Ramp.heightfield.meta")));

			// The same ramp as RAW, read from its absolute path: the same source bytes.
			const std::filesystem::path native = setup.GetEditor().GetProject().GetProjectFile().parent_path() / "Heightmaps" / "Ramp.r16";
			CallTerrainOrFail(setup, "terrain.importHeightmap",
				Json{ { "source", FileSystem::PathToUtf8(native) }, { "path", "Assets/Terrain/RampRaw.heightfield" } });
			CHECK(ReadProjectBytes(setup, "Assets/Terrain/RampRaw.heightfield") == fromPng);

			// Replacing the ramp keeps its handle: a flat 33 x 33 RAW written over it, reimported at once; undo restores the ramp.
			WriteProjectBytes(setup, "Heightmaps/Flat.raw", Buffer(static_cast<size_t>(33) * 33 * 2, std::byte{ 0 }));
			const Json replaced = CallTerrainOrFail(setup, "terrain.importHeightmap",
				Json{ { "source", "Heightmaps/Flat.raw" }, { "path", std::string(RampPath) } });
			CHECK(replaced["asset"]["id"] == imported["asset"]["id"]);
			CHECK(replaced["resolution"] == Json(33));
			CHECK(LoadHeightfield(setup, RampPath)->Resolution == 33);
			CallTerrainOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadProjectBytes(setup, RampPath) == fromPng);

			// Importing the same bytes again writes nothing and records no step.
			const size_t before = setup.GetEditor().GetHistory().GetUndoCount();
			const Json same = CallTerrainOrFail(setup, "terrain.importHeightmap",
				Json{ { "source", "Heightmaps/Ramp16.png" }, { "path", std::string(RampPath) } });
			CHECK(same["asset"]["id"] == imported["asset"]["id"]);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == before);
		}

		TEST_CASE("TerrainMethods: terrain.importHeightmap refuses a missing or unreadable heightmap and a path outside Assets, writing nothing")
		{
			Test::AutomationFixture setup("TerrainImportHeightmapRefused");
			const Result<Buffer> eightBit = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Textures/Rgba.png"));
			REQUIRE(eightBit.has_value());
			WriteProjectBytes(setup, "Heightmaps/Rgba.png", *eightBit);
			Buffer odd = MakeRampRaw();
			odd.pop_back();
			WriteProjectBytes(setup, "Heightmaps/Odd.raw", odd);
			const Buffer large(static_cast<size_t>(1026) * 1026 * 2, std::byte{ 0 });
			WriteProjectBytes(setup, "Heightmaps/Large.raw", large);
			constexpr std::string_view Corrupt = "\x89PNG\r\n\x1a\n not a PNG after all";
			WriteProjectBytes(setup, "Heightmaps/Corrupt.png", std::as_bytes(std::span(Corrupt)));
			WriteProjectBytes(setup, "Heightmaps/Flat.raw", Buffer(static_cast<size_t>(3) * 3 * 2, std::byte{ 0 }));
			const size_t history = setup.GetEditor().GetHistory().GetUndoCount();

			const auto refusedAt = [&setup](std::string_view source, std::string_view path, std::string_view code, std::string_view pointer)
			{
				CAPTURE(std::string(source));
				CAPTURE(std::string(path));
				const Json refused = setup.Request("terrain.importHeightmap", Json{ { "source", std::string(source) }, { "path", std::string(path) } });
				REQUIRE(refused.contains("error"));
				CHECK(refused["error"]["data"]["errorCode"] == Json(std::string(code)));
				CHECK(refused["error"]["data"]["location"]["pointer"] == Json(std::string(pointer)));
			};
			refusedAt("Heightmaps/Missing.raw", "Assets/Terrain/A.heightfield", "NotFound", "/source");
			refusedAt("Heightmaps/Flat.txt", "Assets/Terrain/A.heightfield", "InvalidArgument", "/source");
			refusedAt("Heightmaps/Rgba.png", "Assets/Terrain/A.heightfield", "InvalidArgument", "/source");
			refusedAt("Heightmaps/Odd.raw", "Assets/Terrain/A.heightfield", "InvalidArgument", "/source");
			refusedAt("Heightmaps/Large.raw", "Assets/Terrain/A.heightfield", "InvalidArgument", "/source");
			refusedAt("Heightmaps/Corrupt.png", "Assets/Terrain/A.heightfield", "Parse", "/source");
			refusedAt("Heightmaps/Flat.raw", "Heightmaps/A.heightfield", "InvalidArgument", "/path");
			refusedAt("Heightmaps/Flat.raw", "Assets/Terrain/A.raw", "InvalidArgument", "/path");
			CHECK_FALSE(setup.GetEditor().GetVfs().Exists(ProjectPath("Assets/Terrain")));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history);

			// A .heightfield path registered as an asset of another type (a .meta naming the Texture importer, as a hand edit
			// leaves it) is AlreadyExists at /path, its bytes and .meta untouched.
			Result<AssetMetadata> texture = setup.GetEditor().GetAssets().CreateMetadata(ProjectPath("Assets/Terrain/Other.png"));
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			REQUIRE(texture->Type == AssetType::Texture);
			const auto toBytes = [](std::string_view text)
			{
				const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
				return Buffer(bytes.begin(), bytes.end());
			};
			const Buffer otherBytes = toBytes("not heightfield samples");
			const Buffer metaBytes = toBytes(SerializeAssetMetadata(*texture));
			WriteProjectBytes(setup, "Assets/Terrain/Other.heightfield", otherBytes);
			WriteProjectBytes(setup, "Assets/Terrain/Other.heightfield.meta", metaBytes);
			refusedAt("Heightmaps/Flat.raw", "Assets/Terrain/Other.heightfield", "AlreadyExists", "/path");
			CHECK(ReadProjectBytes(setup, "Assets/Terrain/Other.heightfield") == otherBytes);
			CHECK(ReadProjectBytes(setup, "Assets/Terrain/Other.heightfield.meta") == metaBytes);
			const std::optional<AssetHandle> registered = setup.GetEditor().GetAssets().Resolve("Assets/Terrain/Other.heightfield");
			REQUIRE(registered.has_value());
			CHECK(*registered == texture->Handle);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history);
		}
	}

}
