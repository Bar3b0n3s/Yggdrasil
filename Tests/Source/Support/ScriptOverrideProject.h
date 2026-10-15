#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <filesystem>
#include <string_view>

// A project whose scene overrides a script field of a prefab instance, as the Breakout dry run built it through automation
// (Docs/Decisions/0026-m16-contract.md, friction item S1-04): a Behaviour script declaring Points, a prefab whose root runs
// it with Points 1, and a scene holding one instance of the prefab whose root overrides Points with 5, both in its
// PrefabInstanceComponent and in its flattened Script component. The strict open, project.validate and the exporter read
// it; the override names another field to make it invalid.

namespace Engine {

	namespace Test {

		// The project's files (project-relative) and fixed ids.
		inline constexpr std::string_view ScriptOverrideScene = "Assets/Scenes/Main.scene";
		inline constexpr std::string_view ScriptOverridePrefab = "Assets/Prefabs/Brick.prefab";
		inline constexpr std::string_view ScriptOverrideScript = "Assets/Scripts/Brick.luau";
		inline constexpr UUID ScriptOverrideScriptHandle = UUID(0x5c0a1e0000000001ull);
		inline constexpr UUID ScriptOverridePrefabHandle = UUID(0x5c0a1e0000000002ull);
		inline constexpr UUID ScriptOverrideInstanceID = UUID(0x5c0a1e0000000003ull); // the instance root, the scene's only entity
		// The JSON pointer of the instance root's script field values in the scene (entity 0), without the field name.
		inline constexpr std::string_view ScriptOverrideFieldsPointer = "/Entities/0/Components/Script/Fields/";

		// Writes the script, the prefab and the scene, each with its .meta, into the project at `root` (one made from the
		// Empty template, before or while it is open: an open editor registers them at its next asset refresh). The instance
		// overrides the script field `field` with `value` (JSON text): "Points" is the declared field, another name an unknown
		// one; a value that is not an integer of at least 1 mismatches the declaration. Fails the running test case when a file
		// cannot be written.
		void WriteScriptOverrideProject(const std::filesystem::path& root, std::string_view field = "Points", std::string_view value = "5");

		// Sources of the project's script whose entities' field values project.validate never checks, because the script
		// declares none it can resolve: one that fails to import (a syntax error) and a Module (no Script.Define).
		inline constexpr std::string_view ScriptOverrideBrokenSource = "--!strict\nlocal Brick = {\n\nreturn Script.Define(\"Brick\", Brick)\n";
		inline constexpr std::string_view ScriptOverrideModuleSource = "--!strict\nlocal Brick = {}\n\nfunction Brick.Points(): number\n\treturn 1\nend\n\nreturn Brick\n";

		// Replaces the project's script (ScriptOverrideScript) with `source`. Fails the running test case when it cannot be
		// written.
		void WriteScriptOverrideScript(const std::filesystem::path& root, std::string_view source);

	}

}
