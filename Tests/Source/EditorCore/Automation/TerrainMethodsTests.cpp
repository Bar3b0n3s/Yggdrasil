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
#include "Engine/Graphics/Image.h"
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
// refusals at their pointers. terrain.edit (decision 23, stream T-E): its refusals at their pointers and a stroke that changes
// nothing, which writes nothing; its round trip against
// EditTerrain and the editor's stroke is TerrainEditingTests.cpp's.

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

		TEST_CASE("TerrainMethods: terrain.edit refuses a stroke outside its ranges, a terrain it cannot edit and a playing editor at their params, writing nothing")
		{
			Test::AutomationFixture setup("TerrainEditRefused");
			CallTerrainOrFail(setup, "asset.create", Json{ { "type", "Heightfield" }, { "path", std::string(RampPath) } });
			// A Color texture (the importer's default settings), which a splat map must not be.
			const Result<Image> pixels = CreateImage(4, 4, nvrhi::Format::RGBA8_UNORM);
			REQUIRE(pixels.has_value());
			const Result<Buffer> png = EncodePng(*pixels);
			REQUIRE(png.has_value());
			WriteProjectBytes(setup, "Assets/Terrain/Colour.png", *png);
			CallTerrainOrFail(setup, "project.refreshAssets", Json::object());
			const Json terrain{ { "Heightfield", std::string(RampPath) } };
			CallTerrainOrFail(setup, "entity.create", Json{ { "name", "Terrain" }, { "components", Json{ { "Terrain", terrain } } } });
			CallTerrainOrFail(setup, "entity.create", Json{ { "name", "Plain" } });
			CallTerrainOrFail(setup, "entity.create", Json{ { "name", "Bare" }, { "components", Json{ { "Terrain", Json::object() } } } });
			Json coloured = terrain;
			coloured["SplatMap"] = "Assets/Terrain/Colour.png";
			CallTerrainOrFail(setup, "entity.create", Json{ { "name", "Coloured" }, { "components", Json{ { "Terrain", coloured } } } });
			const Buffer before = ReadProjectBytes(setup, RampPath);
			const size_t history = setup.GetEditor().GetHistory().GetUndoCount();

			const auto refusedAt = [&setup](Json params, std::string_view code, std::string_view pointer)
			{
				CAPTURE(params.dump());
				if (!params.contains("entity"))
					params["entity"] = "/Terrain";
				if (!params.contains("points"))
					params["points"] = Json::array({ Json::array({ 32, 0, 32 }) });
				const Json refused = setup.Request("terrain.edit", params);
				REQUIRE(refused.contains("error"));
				CHECK(refused["error"]["data"]["errorCode"] == Json(std::string(code)));
				if (!pointer.empty())
					CHECK(refused["error"]["data"]["location"]["pointer"] == Json(std::string(pointer)));
			};
			Json manyPoints = Json::array();
			for (int index = 0; index < 4097; ++index)
				manyPoints.push_back(Json::array({ 32, 0, 32 }));
			refusedAt(Json{ { "points", Json::array() } }, "InvalidArgument", "/points");
			refusedAt(Json{ { "points", std::move(manyPoints) } }, "InvalidArgument", "/points");
			refusedAt(Json{ { "radius", 0 } }, "InvalidArgument", "/radius");
			refusedAt(Json{ { "strength", 1.5 } }, "InvalidArgument", "/strength");
			refusedAt(Json{ { "tool", "Flatten" }, { "height", 17 } }, "InvalidArgument", "/height");
			refusedAt(Json{ { "tool", "Paint" }, { "layer", 4 } }, "InvalidArgument", "/layer");
			refusedAt(Json{ { "entity", "/Missing" } }, "NotFound", "/entity");
			refusedAt(Json{ { "entity", "/Plain" } }, "InvalidArgument", "/entity");
			refusedAt(Json{ { "entity", "/Bare" } }, "NotFound", "/entity");
			refusedAt(Json{ { "entity", "/Coloured" }, { "tool", "Paint" } }, "InvalidArgument", "/entity");
			// No run-time edits: the edit scene's terrains wait for the play session to end.
			CallTerrainOrFail(setup, "play.start", Json{ { "lockstep", true }, { "mode", "simulate" } });
			refusedAt(Json::object(), "InvalidState", "");
			CallTerrainOrFail(setup, "play.stop", Json::object());

			CHECK(ReadProjectBytes(setup, RampPath) == before);
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history);
			// The edit succeeds once nothing stands in its way.
			const Json edited = CallTerrainOrFail(setup, "terrain.edit", Json{ { "entity", "/Terrain" }, { "points", Json::array({ Json::array({ 32, 0, 32 }) }) } });
			CHECK(edited["path"] == Json(std::string(RampPath)));
			CHECK(setup.GetEditor().GetHistory().GetUndoCount() == history + 1);
		}

		// Architecture §13.5 (W26.2's review): a stroke that changes no sample or texel writes nothing, reimports nothing and
		// records no undo step and no provenance: a Lower on flat ground, and a Paint of the layer a splat map already holds
		// everywhere.
		TEST_CASE("TerrainMethods: terrain.edit's stroke that changes nothing writes nothing and records no step")
		{
			Test::AutomationFixture setup("TerrainEditNothing");
			CallTerrainOrFail(setup, "asset.create", Json{ { "type", "Heightfield" }, { "path", std::string(RampPath) } });
			CallTerrainOrFail(setup, "entity.create",
				Json{ { "name", "Terrain" }, { "components", Json{ { "Terrain", Json{ { "Heightfield", std::string(RampPath) } } } } } });
			EditorContext& editor = setup.GetEditor();
			const auto provenance = [&editor]()
			{
				const ProvenanceRecorder* recorder = editor.GetProvenance();
				REQUIRE(recorder != nullptr);
				return ProvenanceRecorder::ToText(recorder->GetEntries());
			};
			const Json points = Json::array({ Json::array({ 32, 0, 32 }), Json::array({ 40, 0, 30 }) });
			const Buffer flat = ReadProjectBytes(setup, RampPath);
			const std::optional<AssetHandle> ramp = editor.GetAssets().Resolve(RampPath);
			REQUIRE(ramp.has_value());
			const uint64_t version = editor.GetAssets().GetVersion(*ramp);
			size_t history = editor.GetHistory().GetUndoCount();
			std::string recorded = provenance();
			const Json lowered = CallTerrainOrFail(setup, "terrain.edit", Json{ { "entity", "/Terrain" }, { "tool", "Lower" }, { "points", points }, { "strength", 1 } });
			CHECK(lowered["changed"] == Json(0));
			CHECK(lowered["createdSplatMap"] == Json(false));
			CHECK(lowered["path"] == Json(std::string(RampPath)));
			CHECK(ReadProjectBytes(setup, RampPath) == flat);
			CHECK(editor.GetAssets().GetVersion(*ramp) == version);
			CHECK(editor.GetHistory().GetUndoCount() == history);
			CHECK(provenance() == recorded);

			// The first paint makes the splat map (one step), every texel layer 0's; painting layer 0 again changes nothing.
			const Json paint{ { "entity", "/Terrain" }, { "tool", "Paint" }, { "layer", 0 }, { "points", points }, { "strength", 1 } };
			const Json made = CallTerrainOrFail(setup, "terrain.edit", paint);
			REQUIRE(made["createdSplatMap"] == Json(true));
			const Result<std::string> madePath = JsonReader(made).ReadMember<std::string>("path");
			REQUIRE(madePath.has_value());
			const std::string splatPath = *madePath;
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			const Buffer splat = ReadProjectBytes(setup, splatPath);
			history = editor.GetHistory().GetUndoCount();
			recorded = provenance();
			const Json repainted = CallTerrainOrFail(setup, "terrain.edit", paint);
			CHECK(repainted["changed"] == Json(0));
			CHECK(repainted["createdSplatMap"] == Json(false));
			CHECK(repainted["path"] == Json(splatPath));
			CHECK(ReadProjectBytes(setup, splatPath) == splat);
			CHECK(editor.GetHistory().GetUndoCount() == history);
			CHECK(provenance() == recorded);
		}
	}

}
