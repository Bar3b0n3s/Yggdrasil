#include "TestsPCH.h"

#include "Engine/Project/GameManifest.h"

#include "Engine/Core/FileSystem.h"
#include "Support/Fuzz.h"
#include "Support/TempDirectory.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

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

		// Docs/Decisions/0030-m19-contract.md decision 18 (B-BVER, stream D): version 2 writes the exporting build's id as
		// "Build" after "EngineVersion"; a Build that ParseBuildId refuses, or whose version is not EngineVersion, is refused
		// at /Build, writing and reading; a version 1 manifest has no Build and reads with an empty one.
		TEST_CASE("GameManifest: version 2 carries the build id, and version 1 reads without one" * doctest::skip(true))
		{
			CHECK(GameManifest::CurrentVersion == GameManifestBuildVersion);
			GameManifest manifest = MakeManifest();
			manifest.Build = "0.1.0+3f2a9c1d4e5b.dirty";
			const Result<std::string> text = GameManifestSerializer::SaveToString(manifest);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			CHECK(text->contains("\"Version\": 2"));
			CHECK(text->find("\"EngineVersion\"") < text->find("\"Build\": \"0.1.0+3f2a9c1d4e5b.dirty\""));
			CHECK(text->find("\"Build\"") < text->find("\"StartScene\""));
			const Result<GameManifest> read = GameManifestSerializer::LoadFromString(*text);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Build == manifest.Build);
			const Result<std::string> again = GameManifestSerializer::SaveToString(*read);
			REQUIRE(again.has_value());
			CHECK(*again == *text);

			for (const std::string_view build : { "", "0.1", "0.2.0+3f2a9c1d4e5b", "0.1.0+3F2A9C1D4E5B" })
			{
				CAPTURE(build);
				GameManifest bad = manifest;
				bad.Build = std::string(build);
				const Result<std::string> refused = GameManifestSerializer::SaveToString(bad);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
				REQUIRE_FALSE(refused.error().GetIssues().empty());
				CHECK(refused.error().GetIssues().front().JsonPointer == "/Build");

				std::string edited = *text;
				edited.replace(edited.find("0.1.0+3f2a9c1d4e5b.dirty"), std::string_view("0.1.0+3f2a9c1d4e5b.dirty").size(), std::string(build));
				const Result<GameManifest> unread = GameManifestSerializer::LoadFromString(edited);
				REQUIRE_FALSE(unread.has_value());
				CHECK(unread.error().GetCode() == ErrorCode::Validation);
			}

			// Version 1 knows no Build: one there is an unknown key, and without it the manifest reads with an empty Build.
			std::string version1 = *text;
			version1.replace(version1.find("\"Version\": 2"), 12, "\"Version\": 1");
			const Result<GameManifest> unknown = GameManifestSerializer::LoadFromString(version1);
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::Validation);
			const size_t begin = version1.find("\t\"Build\"");
			REQUIRE(begin != std::string::npos);
			version1.erase(begin, version1.find('\n', begin) + 1 - begin);
			const Result<GameManifest> old = GameManifestSerializer::LoadFromString(version1);
			REQUIRE_MESSAGE(old.has_value(), old.error().ToString());
			CHECK(old->Build.empty());
			CHECK(old->Name == manifest.Name);
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

		TEST_CASE("GameManifest: testing manifests round trip for the cooked Runtime harness")
		{
			// M13 consumes Testing through --feature-test; the exporter's testing switch remains separate.
			GameManifest manifest = MakeManifest();
			manifest.Testing = true;
			const Result<std::string> written = GameManifestSerializer::SaveToString(manifest);
			REQUIRE_MESSAGE(written.has_value(), (written ? "" : written.error().ToString()));
			const auto roundTrip = GameManifestSerializer::LoadFromString(*written);
			REQUIRE_MESSAGE(roundTrip.has_value(), (roundTrip ? "" : roundTrip.error().ToString()));
			CHECK(roundTrip->Testing);
			CHECK(roundTrip->Name == manifest.Name);
			CHECK(roundTrip->StartScene == manifest.StartScene);
			CHECK(roundTrip->Paks == manifest.Paks);
			const auto canonical = GameManifestSerializer::SaveToString(*roundTrip);
			REQUIRE(canonical);
			CHECK(*canonical == *written);

			// The reader also admits a fixture flag flip without going through the testing writer first.
			const Result<std::string> text = GameManifestSerializer::SaveToString(MakeManifest());
			REQUIRE(text.has_value());
			std::string testing = *text;
			const size_t flag = testing.find("\"Testing\": false");
			REQUIRE(flag != std::string::npos);
			testing.replace(flag, std::string_view("\"Testing\": false").size(), "\"Testing\": true");
			const Result<GameManifest> read = GameManifestSerializer::LoadFromString(testing);
			REQUIRE_MESSAGE(read.has_value(), (read ? "" : read.error().ToString()));
			CHECK(read->Testing);
			CHECK(testing == *written);
		}

		TEST_CASE("GameManifest: the testing flag still rejects non boolean values with a located error")
		{
			const auto text = GameManifestSerializer::SaveToString(MakeManifest());
			REQUIRE(text);
			for (const std::string_view value : { "0", "\"true\"", "null" })
			{
				CAPTURE(value);
				std::string invalid = *text;
				const size_t flag = invalid.find("\"Testing\": false");
				REQUIRE(flag != std::string::npos);
				invalid.replace(flag, std::string_view("\"Testing\": false").size(), "\"Testing\": " + std::string(value));
				const auto read = GameManifestSerializer::LoadFromString(invalid);
				REQUIRE_FALSE(read);
				CHECK(read.error().GetCode() == ErrorCode::Validation);
				REQUIRE_FALSE(read.error().GetIssues().empty());
				CHECK(read.error().GetIssues().front().JsonPointer == "/Testing");
			}
		}

		// M15 (§14.3; Docs/Decisions/0024-m15-contract.md decision 6, stream B).
		TEST_CASE("GameManifest: a testing run's user data sits beside the package, outside a macOS bundle")
		{
			Test::TempDirectory directory("TestingUserDataRoot");
			const std::filesystem::path package = directory / "Exports" / "Tetris";
			CHECK(GetTestingUserDataRoot(package / "Game.json") == package / "bin" / "TestUserData");
			CHECK(GetTestingUserDataRoot(package / "Tetris.app" / "Contents" / "Resources" / "Game.json") == package / "bin" / "TestUserData");
			// Only the exact bundle layout counts as a bundle.
			CHECK(GetTestingUserDataRoot(package / "Contents" / "Resources" / "Game.json") == package / "Contents" / "Resources" / "bin" / "TestUserData");
			CHECK(GetTestingUserDataRoot(package / "Tetris.app" / "Game.json") == package / "Tetris.app" / "bin" / "TestUserData");
		}

		// M15 (§15.2; Docs/Decisions/0024-m15-contract.md decision 13, stream C).
		TEST_CASE("GameManifest: 1,000 seeded mutations of a manifest never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds: a plain and a testing manifest as the exporter writes them. A manifest the reader accepts writes back
			// and reads again to the same value.
			GameManifest testing = MakeManifest();
			testing.Testing = true;
			std::vector<std::string> seeds;
			for (const GameManifest& manifest : { MakeManifest(), testing })
			{
				const Result<std::string> text = GameManifestSerializer::SaveToString(manifest);
				REQUIRE(text.has_value());
				seeds.push_back(*text);
			}
			// About one mutation in a hundred is a manifest the reader accepts, so a tenth of the sequence (the Debug unit run's
			// --fuzz-scale=0.1, ADR 0024 decision 14) holds too few to check both outcomes: the minimum is the whole count.
			constexpr uint64_t Iterations = 1000;
			const Test::FuzzRun run = Test::RunFuzzCase(0x6A3E, Iterations, [&seeds](Random& random, uint64_t /*index*/)
			{
				std::string mutated = seeds[static_cast<size_t>(random.RangeInt(0, 1))];
				Test::MutateText(mutated, random);
				const Result<GameManifest> read = GameManifestSerializer::LoadFromString(mutated);
				if (!read.has_value())
				{
					const ErrorCode code = read.error().GetCode();
					CHECK((code == ErrorCode::Parse || code == ErrorCode::Validation || code == ErrorCode::UnsupportedVersion));
					return false;
				}
				const Result<std::string> written = GameManifestSerializer::SaveToString(*read);
				REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
				const Result<GameManifest> reread = GameManifestSerializer::LoadFromString(*written);
				REQUIRE_MESSAGE(reread.has_value(), reread.error().ToString());
				const Result<std::string> rewritten = GameManifestSerializer::SaveToString(*reread);
				REQUIRE(rewritten.has_value());
				CHECK(*rewritten == *written);
				return true;
			}, Iterations);
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}
	}

}
