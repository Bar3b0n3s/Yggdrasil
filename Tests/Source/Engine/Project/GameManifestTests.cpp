#include "TestsPCH.h"

#include "Engine/Project/GameManifest.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

#include <array>
#include <string>
#include <string_view>

// Game.json (Architecture §14.1, §14.3; Docs/Decisions/0012-m7-decisions.md decision 10).

namespace Engine {

	namespace {

		GameManifest MakeManifest()
		{
			GameManifest manifest;
			manifest.Name = "Tetris";
			manifest.EngineVersion = "0.1.0";
			manifest.StartScene = UUID(0x8a61c0d2e4f31b77ULL);
			manifest.Paks = { GameManifestPak{ .Path = "Data/Engine.pak", .Hash = 0x0123456789abcdefULL },
				GameManifestPak{ .Path = "Data/Game.pak", .Hash = 0xfedcba9876543210ULL } };
			manifest.Window.Title = "Tetris";
			manifest.Window.Width = 720;
			manifest.Window.Height = 900;
			manifest.Simulation.Seed = 1;
			return manifest;
		}

	}

	TEST_SUITE("Project")
	{
		TEST_CASE("GameManifest: a written manifest reads back equal, in the documented key order")
		{
			const GameManifest manifest = MakeManifest();
			const Result<std::string> text = GameManifestSerializer::SaveToString(manifest);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(text->find("\"Format\": \"GameManifest\"") < text->find("\"Name\""));
			CHECK(text->find("\"StartScene\": \"8a61c0d2e4f31b77\"") != std::string::npos);
			CHECK(text->find("\"XXH64\": \"0123456789abcdef\"") != std::string::npos);

			const Result<GameManifest> read = GameManifestSerializer::LoadFromString(*text);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Name == manifest.Name);
			CHECK(read->StartScene == manifest.StartScene);
			CHECK(read->Paks == manifest.Paks);
			CHECK(read->Window.Width == 720);
			CHECK(read->Simulation.Seed == 1);
			CHECK_FALSE(read->Testing);
			// Writing what was read gives the same bytes.
			const Result<std::string> again = GameManifestSerializer::SaveToString(*read);
			REQUIRE(again.has_value());
			CHECK(*again == *text);
		}

		TEST_CASE("GameManifest: invalid, newer and missing manifests are located errors")
		{
			GameManifest manifest = MakeManifest();
			manifest.Simulation.FixedHz = 0;
			const Result<std::string> invalid = GameManifestSerializer::SaveToString(manifest);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
			REQUIRE_FALSE(invalid.error().GetIssues().empty());
			CHECK(invalid.error().GetIssues().front().JsonPointer == "/Simulation/FixedHz");

			const Result<std::string> text = GameManifestSerializer::SaveToString(MakeManifest());
			REQUIRE(text.has_value());
			std::string newer = *text;
			newer.replace(newer.find("\"Version\": 1"), 12, "\"Version\": 9");
			const Result<GameManifest> future = GameManifestSerializer::LoadFromString(newer);
			REQUIRE_FALSE(future.has_value());
			CHECK(future.error().GetCode() == ErrorCode::UnsupportedVersion);

			const Result<GameManifest> broken = GameManifestSerializer::LoadFromString("{\"Format\": \"GameManifest\"");
			REQUIRE_FALSE(broken.has_value());
			CHECK(broken.error().GetCode() == ErrorCode::Parse);

			const Test::TempDirectory directory("GameManifestMissing");
			const Result<GameManifest> missing = GameManifestSerializer::LoadFromFile(directory.GetPath() / "Game.json");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetMessageText().contains("Game.json"));
		}

		TEST_CASE("GameManifest: Paks holds exactly Engine.pak and Game.pak")
		{
			// Nothing is accepted and ignored (ADR 0012 decision 16): the Runtime mounts exactly these two paks.
			const Result<std::string> text = GameManifestSerializer::SaveToString(MakeManifest());
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			for (const std::string_view replacement : { std::string_view(R"("Paks": [ { "Path": "Data/Engine.pak", "XXH64": "0123456789abcdef" } ],)"),
					 std::string_view(R"("Paks": [ { "Path": "Data/Engine.pak", "XXH64": "0123456789abcdef" }, { "Path": "Data/Game.pak", "XXH64": "fedcba9876543210" }, { "Path": "Data/Extra.pak", "XXH64": "0000000000000001" } ],)") })
			{
				std::string edited = *text;
				const size_t begin = edited.find("\"Paks\"");
				const size_t end = edited.find("\"Window\"");
				REQUIRE(begin != std::string::npos);
				REQUIRE(end != std::string::npos);
				edited.replace(begin, end - begin, std::string(replacement));
				INFO(edited);
				const Result<GameManifest> read = GameManifestSerializer::LoadFromString(edited);
				REQUIRE_FALSE(read.has_value());
				CHECK(read.error().GetCode() == ErrorCode::Validation);
				REQUIRE_FALSE(read.error().GetIssues().empty());
				CHECK(read.error().GetIssues().front().JsonPointer == "/Paks");
			}

			GameManifest three = MakeManifest();
			three.Paks.push_back(GameManifestPak{ .Path = "Data/Extra.pak", .Hash = 1 });
			const Result<std::string> refused = GameManifestSerializer::SaveToString(three);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("GameManifest: unknown keys, bad names, pak paths and hashes are located Validation errors")
		{
			const Result<std::string> text = GameManifestSerializer::SaveToString(MakeManifest());
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			struct Case
			{
				std::string_view From;
				std::string_view To;
				std::string_view Pointer;
			};
			const std::array<Case, 7> cases = { {
				{ "\"Testing\": false", "\"Testing\": false, \"Extra\": 1", "/Extra" },
				{ "\"Name\": \"Tetris\"", "\"Name\": \"Te/tris\"", "/Name" },
				{ "\"Name\": \"Tetris\"", "\"Name\": \"CON\"", "/Name" },
				{ "\"Path\": \"Data/Engine.pak\"", "\"Path\": \"../Engine.pak\"", "/Paks/0/Path" },
				{ "\"Path\": \"Data/Engine.pak\"", "\"Path\": \"Data/Game.pak\"", "/Paks/0/Path" },
				{ "\"XXH64\": \"0123456789abcdef\"", "\"XXH64\": \"0123456789ABCDEF\"", "/Paks/0/XXH64" },
				{ "\"StartScene\": \"8a61c0d2e4f31b77\"", "\"StartScene\": \"0000000000000000\"", "/StartScene" },
			} };
			for (const Case& edit : cases)
			{
				std::string edited = *text;
				const size_t position = edited.find(edit.From);
				REQUIRE(position != std::string::npos);
				edited.replace(position, edit.From.size(), edit.To);
				INFO(std::string(edit.To));
				const Result<GameManifest> read = GameManifestSerializer::LoadFromString(edited);
				REQUIRE_FALSE(read.has_value());
				CHECK(read.error().GetCode() == ErrorCode::Validation);
				REQUIRE_FALSE(read.error().GetIssues().empty());
				CHECK(read.error().GetIssues().front().JsonPointer == edit.Pointer);
			}
		}

		TEST_CASE("GameManifest: a file is read with its path as the error's file")
		{
			const Test::TempDirectory directory("GameManifestFile");
			const Result<std::string> text = GameManifestSerializer::SaveToString(MakeManifest());
			REQUIRE(text.has_value());
			const std::filesystem::path path = directory / "Game.json";
			REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text->data(), text->size()))).has_value());
			const Result<GameManifest> read = GameManifestSerializer::LoadFromFile(path);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Name == "Tetris");

			std::string broken = *text;
			broken.replace(broken.find("\"Width\": 720"), std::string_view("\"Width\": 720").size(), "\"Width\": 0");
			REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(broken.data(), broken.size()))).has_value());
			const Result<GameManifest> invalid = GameManifestSerializer::LoadFromFile(path);
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::Validation);
			CHECK(invalid.error().GetLocation().File == FileSystem::PathToUtf8(path));
			REQUIRE_FALSE(invalid.error().GetIssues().empty());
			CHECK(invalid.error().GetIssues().front().JsonPointer == "/Window/Width");
		}

		TEST_CASE("GameManifest: a testing manifest is refused until testing exports exist")
		{
			// "Testing": true needs the testing runner of M15; the M7 Runtime would ignore it (ADR 0012 decision 16).
			GameManifest manifest = MakeManifest();
			manifest.Testing = true;
			const Result<std::string> written = GameManifestSerializer::SaveToString(manifest);
			REQUIRE_FALSE(written.has_value());
			CHECK(written.error().GetCode() == ErrorCode::Unsupported);

			const Result<std::string> text = GameManifestSerializer::SaveToString(MakeManifest());
			REQUIRE(text.has_value());
			std::string testing = *text;
			const size_t flag = testing.find("\"Testing\": false");
			REQUIRE(flag != std::string::npos);
			testing.replace(flag, std::string_view("\"Testing\": false").size(), "\"Testing\": true");
			const Result<GameManifest> read = GameManifestSerializer::LoadFromString(testing);
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Unsupported);
			REQUIRE_FALSE(read.error().GetIssues().empty());
			CHECK(read.error().GetIssues().front().JsonPointer == "/Testing");
		}
	}

}
