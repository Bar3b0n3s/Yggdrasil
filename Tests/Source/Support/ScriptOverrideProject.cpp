#include "TestsPCH.h"
#include "Support/ScriptOverrideProject.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"

#include <nlohmann/json.hpp>

#include <format>
#include <string>

namespace Engine {

	namespace Test {

		namespace {

			void WriteProjectText(const std::filesystem::path& root, std::string_view relative, std::string_view text)
			{
				const std::filesystem::path path = root / FileSystem::PathFromUtf8(relative);
				REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(path, AsBytes(text), { .KeepBackup = false }).has_value());
			}

			std::string MakeMeta(UUID handle, std::string_view type)
			{
				return std::format(R"({{
	"Format": "AssetMeta",
	"Version": 1,
	"Handle": "{}",
	"Type": "{}",
	"Importer": "{}",
	"ImporterVersion": 1,
	"Settings": null,
	"SubAssets": []
}}
)",
					handle.ToString(), type, type);
			}

		}

		void WriteScriptOverrideScript(const std::filesystem::path& root, std::string_view source)
		{
			WriteProjectText(root, ScriptOverrideScript, source);
		}

		void WriteScriptOverrideProject(const std::filesystem::path& root, std::string_view field, std::string_view value)
		{
			const std::string script = ScriptOverrideScriptHandle.ToString();
			const std::string prefab = ScriptOverridePrefabHandle.ToString();
			const std::string instance = ScriptOverrideInstanceID.ToString();
			constexpr std::string_view PrefabRoot = "5c0a1e00000000b1";

			WriteProjectText(root, ScriptOverrideScript, R"(--!strict
local Brick = {}

Brick.Fields = {
	Points = Field.Integer(1, { Min = 1 }),
}

return Script.Define("Brick", Brick)
)");
			WriteProjectText(root, std::string(ScriptOverrideScript) + ".meta", MakeMeta(ScriptOverrideScriptHandle, "Script"));

			WriteProjectText(root, ScriptOverridePrefab, std::format(R"({{
	"Format": "Prefab",
	"Version": 1,
	"Name": "Brick",
	"Root": "{0}",
	"ComponentVersions": {{
		"Transform": 1,
		"Script": 2
	}},
	"Entities": [
		{{
			"ID": "{0}",
			"Name": "Brick",
			"Parent": null,
			"Active": true,
			"Tags": [],
			"Components": {{
				"Transform": {{
					"Translation": [0, 0, 0],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				}},
				"Script": {{
					"Scripts": [
						{{
							"Script": "{1}",
							"Fields": {{
								"Points": 1
							}},
							"ExecutionOrder": 0
						}}
					]
				}}
			}}
		}}
	]
}}
)",
															 PrefabRoot, script));
			WriteProjectText(root, std::string(ScriptOverridePrefab) + ".meta", MakeMeta(ScriptOverridePrefabHandle, "Prefab"));

			// The member's flattened values: the prefab's Points with the override applied, as PrefabInstantiator applies it.
			const std::string memberFields = field == "Points" ? std::format("\"Points\": {}", value) : std::format("\"Points\": 1,\n\t\t\t\t\t\t\t\t\"{}\": {}", field, value);

			WriteProjectText(root, ScriptOverrideScene, std::format(R"({{
	"Format": "Scene",
	"Version": 1,
	"Name": "Main",
	"Seed": 0,
	"ComponentVersions": {{
		"Transform": 1,
		"Prefab": 2,
		"PrefabLink": 1,
		"Script": 2
	}},
	"Entities": [
		{{
			"ID": "{0}",
			"Name": "Brick",
			"Parent": null,
			"Active": true,
			"Tags": [],
			"Components": {{
				"Transform": {{
					"Translation": [0, 0, 0],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				}},
				"Prefab": {{
					"Prefab": "{1}",
					"Overrides": [
						{{
							"Path": ["{2}"],
							"Kind": "Field",
							"Component": "Script",
							"Field": "Scripts/0/Fields",
							"Value": {{
								"{4}": {6}
							}}
						}}
					]
				}},
				"PrefabLink": {{
					"PrefabEntityID": "{2}",
					"InstanceRoot": "{0}",
					"OuterPrefabEntityID": null
				}},
				"Script": {{
					"Scripts": [
						{{
							"Script": "{3}",
							"Fields": {{
								{5}
							}},
							"ExecutionOrder": 0
						}}
					]
				}}
			}}
		}}
	]
}}
)",
															instance, prefab, PrefabRoot, script, field, memberFields, value));
			WriteProjectText(root, std::string(ScriptOverrideScene) + ".meta", MakeMeta(UUID(0x5c0a1e0000000004ull), "Scene"));
		}

		void RewriteScriptOverrideProjectAtScriptVersion1(const std::filesystem::path& root)
		{
			for (const std::string_view relative : { ScriptOverrideScene, ScriptOverridePrefab })
			{
				const Result<std::string> text = FileSystem::ReadText(root / FileSystem::PathFromUtf8(relative));
				REQUIRE_MESSAGE(text.has_value(), (text ? std::string() : text.error().ToString()));
				Result<Json> document = JsonReader::Parse(*text);
				REQUIRE(document.has_value());
				(*document)["ComponentVersions"]["Script"] = 1;
				for (Json& entity : (*document)["Entities"])
				{
					Json& components = entity["Components"];
					if (components.contains("Script"))
					{
						Json slot = components["Script"]["Scripts"][0];
						components["Script"] = std::move(slot);
					}
					if (!components.contains("Prefab"))
						continue;
					for (Json& entry : components["Prefab"]["Overrides"])
					{
						if (entry["Component"] == Json("Script") && entry["Field"] == Json("Scripts/0/Fields"))
							entry["Field"] = "Fields";
					}
				}
				const Result<std::string> written = JsonWriter::Write(*document);
				REQUIRE(written.has_value());
				WriteProjectText(root, relative, *written);
			}
		}

	}

}
