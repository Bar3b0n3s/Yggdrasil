#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Project/ProjectSettings.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The exported game's manifest, Game.json (Architecture §14.1, §14.3), frozen by the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 10). The exporter writes it next to the executable (EditorCore/Export); the
// Runtime reads it before anything else, because its Name is the application name (the user-data folder of logs, crash
// reports and user://, §4.4) and its Window and Simulation set up the process.
//
//     { "Format": "GameManifest", "Version": 1, "Name": "Tetris", "EngineVersion": "0.1.0",
//       "StartScene": "8a61c0d2e4f31b77",
//       "Paks": [ { "Path": "Data/Engine.pak", "XXH64": "<16 hex>" }, { "Path": "Data/Game.pak", "XXH64": "<16 hex>" } ],
//       "Window": { "Title": "Tetris", "Width": 720, "Height": 900, "VSync": true, "Fullscreen": false, "Resizable": true },
//       "Simulation": { "FixedHz": 60, "MaxStepsPerFrame": 5, "Seed": 1, "MaxEntities": 65536 },
//       "Testing": false }
//
// Keys in this order, written by the canonical JsonWriter (Pretty, LF, tabs; §6). Window and Simulation are the project's
// WindowSettings and SimulationSettings with every field (Window carries Resizable too, which §14.1's example omits) and
// the project loader's rules. Paths are relative to the manifest's directory, forward slashes, VfsPath rules (no "..", no
// absolute paths, no reserved names); Paks holds exactly two entries, Engine.pak then Game.pak (§14.1). Hashes and the
// start scene are 16 lowercase hex digits (§4.8: never JSON numbers). "Testing": true marks a testing export, which
// arrives with M15: until then the serializer refuses it rather than accept a manifest the Runtime would ignore
// (Docs/Decisions/0012-m7-decisions.md decision 16).

namespace Engine {

	// One pak the game mounts, with the XXH64 (seed 0) of its whole file.
	struct GameManifestPak
	{
		std::string Path{};
		uint64_t Hash = 0;

		bool operator==(const GameManifestPak&) const = default;
	};

	struct GameManifest
	{
		static constexpr std::string_view FormatName = "GameManifest";
		static constexpr uint32_t CurrentVersion = 1;
		// The manifest's file name next to the executable (§14.1).
		static constexpr std::string_view FileName = "Game.json";

		std::string Name{};          // the project's Name: non-empty, a valid file name (the executable's and the user-data folder's)
		std::string EngineVersion{}; // the exporting engine's version (EngineVersionString)
		UUID StartScene{};           // the Scene asset the game starts with (in Game.pak)
		std::vector<GameManifestPak> Paks{};
		WindowSettings Window{};
		SimulationSettings Simulation{};
		bool Testing = false;
	};

	// Reads and writes Game.json (see the file comment). Static functions only; pure apart from the file reads; thread-safe.
	// They need no EngineContext: the Runtime reads the manifest in its ApplicationFactory, before the ProcessContext exists,
	// so they never log (errors are values).
	class GameManifestSerializer
	{
	public:
		// The canonical text. Errors: Validation or Unsupported for a manifest that LoadFromString would reject (located at
		// its JSON pointer), so a written manifest always reads back equal.
		[[nodiscard]] static Result<std::string> SaveToString(const GameManifest& manifest);

		// Reads a manifest strictly: unknown keys, wrong types and out-of-range values are Validation errors located at their
		// JSON pointer ("/Simulation/FixedHz"); a wrong Format, a Version below 1, an empty or invalid Name, an invalid
		// StartScene, a Paks array that does not hold exactly two entries (located at /Paks), an invalid pak path or hash are
		// Validation errors; "Testing": true is Unsupported located at /Testing (testing exports, M15); a newer Version is
		// UnsupportedVersion naming both versions; invalid JSON is Parse (line and column).
		[[nodiscard]] static Result<GameManifest> LoadFromString(std::string_view text);

		// Reads `path` (a native path; UTF-8 validated) and LoadFromString. Errors: NotFound naming the path when the file is
		// missing (the Runtime exits 3, §4.1, "test_runtime_missing_manifest_exits_3"); Io; those of LoadFromString with the
		// path as the location's file.
		[[nodiscard]] static Result<GameManifest> LoadFromFile(const std::filesystem::path& path);
	};

}
