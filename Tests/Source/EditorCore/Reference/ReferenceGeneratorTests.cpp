#include "TestsPCH.h"

#include "EditorCore/Reference/ReferenceGenerator.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/EditorTestFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		const GeneratedReferenceFile* FindFile(const std::vector<GeneratedReferenceFile>& files, std::string_view path)
		{
			const auto found = std::ranges::find(files, path, &GeneratedReferenceFile::Path);
			return found == files.end() ? nullptr : &*found;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// ADR 0022 decision 13: the frozen file set, the unchanged M13 formats and the new references' coverage.
		TEST_CASE("ReferenceGenerator: the references list every component, field, enum value, method, extension and validation code" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ReferenceGenerator");
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			const ScriptApiRegistry& scripts = fixture.GetEngine().GetScriptApiRegistry();
			MethodRegistry methods(types);
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = false });
			methods.Freeze();
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const ReferenceSources sources{ .Types = &types, .Scripts = &scripts, .Methods = &methods, .Importers = &importers };

			const auto files = GenerateReferences(sources);
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			CHECK(std::ranges::is_sorted(*files, {}, &GeneratedReferenceFile::Path));
			for (const std::string_view path : { "Automation.md", "catalog.json", "Components.md", "Engine.d.luau", "FileFormats.md", "Methods.json",
					 "ScriptAPI.md", "Validation.md", "Schemas/AssetMeta.schema.json", "Schemas/BatchRequest.schema.json",
					 "Schemas/GameManifest.schema.json", "Schemas/Material.schema.json", "Schemas/Prefab.schema.json", "Schemas/Project.schema.json",
					 "Schemas/Provenance.schema.json", "Schemas/Replay.schema.json", "Schemas/Scene.schema.json", "Schemas/SoundEffect.schema.json" })
			{
				CAPTURE(path);
				CHECK(FindFile(*files, path) != nullptr);
			}
			CHECK(files->size() == 18);
			for (const GeneratedReferenceFile& file : *files)
			{
				CAPTURE(file.Path);
				CHECK_FALSE(file.Content.empty());
				CHECK(file.Content.find('\r') == std::string::npos);
				CHECK(file.Content.back() == '\n');
			}

			// The M13 outputs keep their exact formats (the MCP bridge, Test.py's method gate and the checker read them).
			const auto methodCatalog = JsonWriter::Write(methods.BuildMethodCatalog(), JsonStyle::Pretty);
			const auto definitions = scripts.GenerateDefinitions();
			REQUIRE((methodCatalog.has_value() && definitions.has_value()));
			CHECK(FindFile(*files, "Methods.json")->Content == *methodCatalog);
			CHECK(FindFile(*files, "Engine.d.luau")->Content == *definitions);

			const std::string& components = FindFile(*files, "Components.md")->Content;
			for (const auto& component : types.GetComponents())
				CHECK(components.find(std::string(component->GetName())) != std::string::npos);
			CHECK(components.find("Orthographic") != std::string::npos);
			const std::string& automation = FindFile(*files, "Automation.md")->Content;
			for (const MethodDescriptor* method : methods.GetMethods())
				CHECK(automation.find(method->Specification.Name) != std::string::npos);
			const std::string& validation = FindFile(*files, "Validation.md")->Content;
			for (const std::string_view code : ProjectValidator::GetCodes())
				CHECK(validation.find(code) != std::string::npos);
			const std::string& formats = FindFile(*files, "FileFormats.md")->Content;
			for (const std::string_view extension : { ".png", ".jpeg", ".glb", ".otf", ".hdr", ".exr", ".sfx", ".replay" })
				CHECK(formats.find(extension) != std::string::npos);

			const auto again = GenerateReferences(sources);
			REQUIRE(again.has_value());
			CHECK(std::ranges::equal(*files, *again, [](const GeneratedReferenceFile& left, const GeneratedReferenceFile& right)
			{
				return left.Path == right.Path && left.Content == right.Content;
			}));
		}

		TEST_CASE("ReferenceGenerator: missing or unfrozen registries are refused" * doctest::skip(true))
		{
			const auto missing = GenerateReferences(ReferenceSources{});
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::InvalidArgument);

			Test::EditorTestFixture fixture("ReferenceUnfrozen");
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			MethodRegistry methods(types);
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = false });
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const auto unfrozen = GenerateReferences(
				ReferenceSources{ .Types = &types, .Scripts = &fixture.GetEngine().GetScriptApiRegistry(), .Methods = &methods, .Importers = &importers });
			REQUIRE_FALSE(unfrozen.has_value());
			CHECK(unfrozen.error().GetCode() == ErrorCode::InvalidState);
		}

		// ADR 0022 decision 13: every file lands below the directory at its '/'-separated path, parents created, each written
		// atomically; rewriting replaces the content.
		TEST_CASE("ReferenceGenerator: WriteReferences writes every file below the directory" * doctest::skip(true))
		{
			Test::TempDirectory directory("ReferenceWrite");
			const std::vector<GeneratedReferenceFile> files = {
				GeneratedReferenceFile{ .Path = "Components.md", .Content = "# Components\n" },
				GeneratedReferenceFile{ .Path = "Schemas/Scene.schema.json", .Content = "{}\n" },
			};
			const Status written = WriteReferences(files, directory / "Out");
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(FileSystem::ReadText(directory / "Out" / "Components.md").value_or("") == "# Components\n");
			CHECK(FileSystem::ReadText(directory / "Out" / "Schemas" / "Scene.schema.json").value_or("") == "{}\n");
			const std::vector<GeneratedReferenceFile> again = { GeneratedReferenceFile{ .Path = "Components.md", .Content = "# Changed\n" } };
			REQUIRE(WriteReferences(again, directory / "Out").has_value());
			CHECK(FileSystem::ReadText(directory / "Out" / "Components.md").value_or("") == "# Changed\n");
		}

		// The schemas describe the files authors and agents write: every .eproj, .scene, .prefab, .material, .sfx, .meta and
		// .replay under Projects/FeatureTest and Tests/Data validates against its format's schema, except the deliberately
		// invalid or other-version fixtures: Scenes/Invalid/, Formats/ (v0.scene, NewerVersion.scene), Validation/,
		// CoverageGaps/ and Lint/.
		TEST_CASE("ReferenceGenerator: every authored file validates against its generated format schema" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ReferenceSchemas");
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			MethodRegistry methods(types);
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = false });
			methods.Freeze();
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const auto files = GenerateReferences(
				ReferenceSources{ .Types = &types, .Scripts = &fixture.GetEngine().GetScriptApiRegistry(), .Methods = &methods, .Importers = &importers });
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			const GeneratedReferenceFile* project = FindFile(*files, "Schemas/Project.schema.json");
			REQUIRE(project != nullptr);
			const auto schema = JsonReader::Parse(project->Content);
			const auto allSettings = Test::ReadTestDataText("Project/AllSettings.eproj");
			REQUIRE((schema.has_value() && allSettings.has_value()));
			const auto instance = JsonReader::Parse(*allSettings);
			REQUIRE(instance.has_value());
			const Status valid = JsonSchema::Validate(*schema, *instance);
			CHECK_MESSAGE(valid.has_value(), (valid ? std::string() : valid.error().ToString()));
			FAIL("contract skeleton: validate every authored file of the listed kinds against its schema, naming each failure");
		}
	}

}
