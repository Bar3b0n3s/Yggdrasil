#include "TestsPCH.h"

#include "Support/TestGame.h"

#include "Engine/Asset/PakReader.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Project/GameManifest.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Project/ProjectSettings.h"
#include "Support/SceneTestFixture.h"
#include "Support/TempDirectory.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace Engine {

	namespace {

		// The XXH64 of a file the test game wrote.
		uint64_t HashWrittenFile(const std::filesystem::path& path)
		{
			const Result<Buffer> bytes = FileSystem::ReadFile(path);
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return XXH64(std::span<const std::byte>(bytes->data(), bytes->size()));
		}

		// How many entries of `pak` lie under Shaders/.
		size_t CountShaderEntries(const PakReader& pak)
		{
			size_t count = 0;
			for (const PakEntry& entry : pak.GetEntries())
				count += entry.Path.starts_with("Shaders/") ? 1 : 0;
			return count;
		}

	}

	TEST_SUITE("Support")
	{
		TEST_CASE("TestGame: writes a manifest that reads back, names both paks with their hashes and carries the project")
		{
			const Test::TempDirectory directory("TestGame");
			const Test::TestGameSpecification specification{ .Name = "TestGameCheck", .Width = 64, .Height = 48, .FixedHz = 30, .Seed = 11, .Shaders = false };
			const Result<std::filesystem::path> written = Test::WriteTestGame(directory.GetPath(), specification);
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(*written == directory / std::string(GameManifest::FileName));

			const Result<GameManifest> manifest = GameManifestSerializer::LoadFromFile(*written);
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			CHECK(manifest->Name == "TestGameCheck");
			CHECK(manifest->StartScene == Test::TestGameStartScene);
			CHECK(manifest->Window.Width == 64u);
			CHECK(manifest->Window.Height == 48u);
			CHECK(manifest->Simulation.FixedHz == 30u);
			CHECK(manifest->Simulation.Seed == 11u);
			REQUIRE(manifest->Paks.size() == 2);
			CHECK(manifest->Paks[0].Path == "Data/Engine.pak");
			CHECK(manifest->Paks[1].Path == "Data/Game.pak");
			for (const GameManifestPak& pak : manifest->Paks)
			{
				INFO(pak.Path);
				CHECK(pak.Hash == HashWrittenFile(directory / pak.Path));
			}

			// Game.pak: the start scene, and the project settings in its Metadata as the Runtime reads them (§14.1).
			const Result<Ref<const PakReader>> gamePak = PakReader::Open(directory / "Data" / "Game.pak");
			REQUIRE_MESSAGE(gamePak.has_value(), gamePak.error().ToString());
			const PakEntry* scene = (*gamePak)->FindByHandle(Test::TestGameStartScene);
			REQUIRE(scene != nullptr);
			CHECK(scene->Type == "Scene");
			CHECK((*gamePak)->ReadEntry(*scene).has_value());
			const Json& metadata = (*gamePak)->GetMetadata().Get();
			REQUIRE(metadata.is_object());
			CHECK(metadata.size() == 1);
			REQUIRE(metadata.contains("Project"));
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ProjectLoadReport report;
			const Result<ProjectSettings> project = ProjectSerializer::FromJson(metadata["Project"], *registry, {}, report);
			REQUIRE_MESSAGE(project.has_value(), project.error().ToString());
			CHECK(project->Name == "TestGameCheck");
			CHECK(project->StartScene == "Assets/Scenes/Main.scene");
			CHECK(project->Simulation.FixedHz == manifest->Simulation.FixedHz);
			CHECK(project->Simulation.Seed == manifest->Simulation.Seed);
			CHECK(project->Window.Width == manifest->Window.Width);
			CHECK(project->Window.Height == manifest->Window.Height);

			// Without Shaders, Engine.pak holds no file.
			const Result<Ref<const PakReader>> enginePak = PakReader::Open(directory / "Data" / "Engine.pak");
			REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
			CHECK(CountShaderEntries(**enginePak) == 0);
			CHECK((*enginePak)->GetMetadata().Get().contains("Configuration"));
		}

		TEST_CASE("TestGame: with Shaders, Engine.pak holds this configuration's compiled shaders and their reflection")
		{
			const Test::TempDirectory directory("TestGameShaders");
			const Result<std::filesystem::path> written = Test::WriteTestGame(directory.GetPath(), { .Name = "TestGameShaders", .Shaders = true });
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			const Result<Ref<const PakReader>> enginePak = PakReader::Open(directory / "Data" / "Engine.pak");
			REQUIRE_MESSAGE(enginePak.has_value(), enginePak.error().ToString());
			CHECK(CountShaderEntries(**enginePak) > 0);
			bool hasSpirv = false;
			bool hasReflection = false;
			for (const PakEntry& entry : (*enginePak)->GetEntries())
			{
				INFO(entry.Path);
				CHECK(entry.Path.starts_with("Shaders/"));
				CHECK((entry.Path.ends_with(".spv") || entry.Path.ends_with(".refl.json")));
				hasSpirv = hasSpirv || entry.Path.ends_with(".spv");
				hasReflection = hasReflection || entry.Path.ends_with(".refl.json");
			}
			CHECK(hasSpirv);
			CHECK(hasReflection);
			CHECK((*enginePak)->FindByPath("Shaders/Scene/VSMain.spv") != nullptr);
		}
	}

}
