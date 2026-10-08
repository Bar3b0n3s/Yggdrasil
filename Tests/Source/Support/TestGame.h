#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <filesystem>
#include <string>

// A game laid out the way project.export packages one (Architecture §14.1; EditorCore/Export/Exporter.h), written by the
// Tests themselves, so the Runtime's tests run against exported data without the editor: Game.json, Data/Engine.pak and
// Data/Game.pak in one directory, the Runtime run on it with --manifest (Runtime/RuntimeApp.h).

namespace Engine {

	namespace Test {

		struct TestGameSpecification
		{
			// The manifest's and the project's Name: the user-data folder of the run.
			std::string Name = "RuntimeTestGame";
			// The window (the game view's size headless).
			uint32_t Width = 160;
			uint32_t Height = 90;
			uint32_t FixedHz = 60;
			uint32_t Seed = 7; // the project half of the session seed
			// Engine.pak with this configuration's compiled shaders (every .spv and .refl.json of ENGINE_SHADER_DIRECTORY under
			// "Shaders/", as the exporter writes them), for runs that render; without, a pak with no files.
			bool Shaders = false;
		};

		// The start scene of a test game: "Main", a primary perspective camera at (0, 1.5, 5), a directional light and the
		// built-in cube.
		inline constexpr UUID TestGameStartScene{ 0x5ce9e00000000001ULL };

		// Writes the test game into `directory` (created): Game.pak holds the cooked start scene (CookDocument of its canonical
		// document) and the project's settings in its Metadata ({"Project": ...}); Engine.pak per `specification`; Game.json
		// names both with their XXH64. Returns the manifest's path. Errors: those of the writes.
		[[nodiscard]] Result<std::filesystem::path> WriteTestGame(const std::filesystem::path& directory, const TestGameSpecification& specification = {});

	}

}
