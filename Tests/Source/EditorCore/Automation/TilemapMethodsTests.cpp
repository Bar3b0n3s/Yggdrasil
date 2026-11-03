#include "TestsPCH.h"

#include "EditorCore/Automation/TilemapMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// tilemap.paint (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 26, stream T3): its C++ round trip.
// Skipped until stream T3 registers and implements the method (with stream T1's importer).

namespace Engine {

	namespace {

		constexpr std::string_view LevelPath = "Assets/Tilemaps/Level.tilemap";
		// Two palette entries and no cells yet.
		constexpr std::string_view EmptyLevel = R"({"Format": "Tilemap", "Version": 1, "CellSize": [1, 1], "ChunkSize": 16,
			"Palette": [{"Sheet": null, "Frame": 0, "Solid": true}, {"Sheet": null, "Frame": 1, "Solid": false}], "Chunks": []})";

		Json CallTilemapOrFail(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			Result<Json> result = setup.Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		// The level's tilemap as the file holds it.
		TilemapData ReadLevel(Test::AutomationFixture& setup)
		{
			const VfsPath path = VfsPath::Create("project", LevelPath).value_or(VfsPath());
			const Result<std::string> text = setup.GetEditor().GetVfs().ReadText(path);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			TilemapLoadReport report;
			Result<TilemapData> tilemap = TilemapFromText(*text, setup.GetEditor().GetTypeRegistry(), report, true);
			REQUIRE_MESSAGE(tilemap.has_value(), tilemap.error().ToString());
			return std::move(*tilemap);
		}

		Json Cell(int32_t x, int32_t y, uint32_t tile)
		{
			return Json{ { "x", x }, { "y", y }, { "tile", tile } };
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TilemapMethods: tilemap.paint writes cells in one undo step and refuses bad params at their pointers" * doctest::skip(true))
		{
			Test::AutomationFixture setup("TilemapPaint");
			const VfsPath path = VfsPath::Create("project", LevelPath).value_or(VfsPath());
			REQUIRE(setup.GetEditor().CreateProjectDirectory(path.GetParent()).has_value());
			REQUIRE(setup.GetEditor().WriteProjectFile(path, std::as_bytes(std::span(EmptyLevel))).has_value());
			CallTilemapOrFail(setup, "project.refreshAssets", Json::object());
			const std::string original = setup.GetEditor().GetVfs().ReadText(path).value_or(std::string());

			// Two cells of the ground row and a far decor cell: three cells changed in two chunks, one undo step.
			const Json painted = CallTilemapOrFail(setup, "tilemap.paint",
				Json{ { "tilemap", std::string(LevelPath) }, { "cells", Json::array({ Cell(0, 0, 1), Cell(1, 0, 1), Cell(40, 40, 2) }) } });
			CHECK(painted["tilemap"]["path"] == Json(std::string(LevelPath)));
			CHECK(painted["changed"] == Json(3));
			CHECK(painted["chunks"] == Json(2));
			CHECK(JsonReader(painted["undoIndex"]).ReadUInt32().value_or(0) > 0);
			TilemapData level = ReadLevel(setup);
			CHECK(GetTilemapCell(level, glm::ivec2(1, 0)) == 1);
			CHECK(GetTilemapCell(level, glm::ivec2(40, 40)) == 2);
			CHECK(level.Chunks.size() == 2);

			// Undo restores the file byte for byte; redo paints again.
			CallTilemapOrFail(setup, "edit.undo", Json::object());
			CHECK(setup.GetEditor().GetVfs().ReadText(path).value_or(std::string()) == original);
			CallTilemapOrFail(setup, "edit.redo", Json::object());
			CHECK(GetTilemapCell(ReadLevel(setup), glm::ivec2(0, 0)) == 1);

			// Erasing the far cell removes its chunk; painting a cell its value again changes nothing and writes nothing.
			const Json erased = CallTilemapOrFail(setup, "tilemap.paint", Json{ { "tilemap", std::string(LevelPath) }, { "cells", Json::array({ Cell(40, 40, 0) }) } });
			CHECK(erased["changed"] == Json(1));
			CHECK(erased["chunks"] == Json(1));
			const Json same = CallTilemapOrFail(setup, "tilemap.paint", Json{ { "tilemap", std::string(LevelPath) }, { "cells", Json::array({ Cell(0, 0, 1) }) } });
			CHECK(same["changed"] == Json(0));
			CHECK(same["undoIndex"] == Json(0));

			// Refusals, located, changing nothing.
			const std::string before = setup.GetEditor().GetVfs().ReadText(path).value_or(std::string());
			const auto refusedAt = [&setup](const Json& params, std::string_view code, std::string_view pointer)
			{
				CAPTURE(params.dump());
				const Json refused = setup.Request("tilemap.paint", params);
				CHECK(refused["error"]["data"]["errorCode"] == Json(std::string(code)));
				CHECK(refused["error"]["data"]["location"]["pointer"] == Json(std::string(pointer)));
			};
			refusedAt(Json{ { "tilemap", std::string(LevelPath) }, { "cells", Json::array({ Cell(0, 0, 3) }) } }, "InvalidArgument", "/cells/0/tile");
			refusedAt(Json{ { "tilemap", std::string(LevelPath) }, { "cells", Json::array({ Cell(0, 0, 1), Cell(MaxTilemapCellCoordinate, 0, 1) }) } },
				"InvalidArgument", "/cells/1/x");
			refusedAt(Json{ { "tilemap", std::string(LevelPath) }, { "cells", Json::array() } }, "InvalidArgument", "/cells");
			refusedAt(Json{ { "tilemap", "engine://Textures/SpriteSquare" }, { "cells", Json::array({ Cell(0, 0, 1) }) } }, "InvalidArgument", "/tilemap");
			refusedAt(Json{ { "tilemap", "Assets/Tilemaps/Missing.tilemap" }, { "cells", Json::array({ Cell(0, 0, 1) }) } }, "NotFound", "/tilemap");
			CHECK(setup.GetEditor().GetVfs().ReadText(path).value_or(std::string()) == before);
		}
	}

}
