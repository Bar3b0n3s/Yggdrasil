#include "TestsPCH.h"

#include "EditorCore/Reference/ReferenceGenerator.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Audio/SoundSynth.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/EditorTestFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		const GeneratedReferenceFile* FindFile(const std::vector<GeneratedReferenceFile>& files, std::string_view path)
		{
			const auto found = std::ranges::find(files, path, &GeneratedReferenceFile::Path);
			return found == files.end() ? nullptr : &*found;
		}

		// The editor's registries of one fixture, frozen, as the dump builds them.
		class ReferenceRegistries
		{
		public:
			explicit ReferenceRegistries(Test::EditorTestFixture& fixture)
				: m_Fixture(fixture), m_Methods(fixture.GetEngine().GetTypeRegistry())
			{
				RegisterEditorMethods(m_Methods, EditorMethodOptions{ .TestHooks = false });
				m_Methods.Freeze();
				RegisterBuiltinImporters(m_Importers);
			}

			[[nodiscard]] ReferenceSources GetSources() const
			{
				return ReferenceSources{ .Types = &m_Fixture.GetEngine().GetTypeRegistry(),
					.Scripts = &m_Fixture.GetEngine().GetScriptApiRegistry(),
					.Methods = &m_Methods,
					.Importers = &m_Importers };
			}
		private:
			Test::EditorTestFixture& m_Fixture;
			MethodRegistry m_Methods;
			ImporterRegistry m_Importers{};
		};

		Json ParseSchema(const std::vector<GeneratedReferenceFile>& files, std::string_view format)
		{
			const GeneratedReferenceFile* file = FindFile(files, std::format("Schemas/{}.schema.json", format));
			REQUIRE_MESSAGE(file != nullptr, std::string(format));
			const auto schema = JsonReader::Parse(file->Content);
			REQUIRE(schema.has_value());
			return *schema;
		}

		// The lines of a batch file as (line number, text), without empty lines.
		std::vector<std::pair<size_t, std::string>> SplitLines(const std::string& text)
		{
			std::vector<std::pair<size_t, std::string>> lines;
			size_t start = 0;
			for (size_t line = 1; start < text.size(); ++line)
			{
				const size_t end = text.find('\n', start);
				std::string content = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
				if (!content.empty())
					lines.emplace_back(line, std::move(content));
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			return lines;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// ADR 0022 decision 13: the frozen file set, the unchanged M13 formats and the new references' coverage.
		TEST_CASE("ReferenceGenerator: the references list every component, field, enum value, method, extension and validation code")
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
			{
				CAPTURE(component->GetName());
				CHECK(components.find(std::format("## {}\n", component->GetName())) != std::string::npos);
				for (const auto& field : component->GetFields())
					CHECK(components.find(std::format("| `{}` |", field->GetName())) != std::string::npos);
			}
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

		TEST_CASE("ReferenceGenerator: missing or unfrozen registries are refused")
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
		TEST_CASE("ReferenceGenerator: WriteReferences writes every file below the directory")
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

		TEST_CASE("ReferenceGenerator: WriteReferences refuses a path outside the directory")
		{
			Test::TempDirectory directory("ReferenceWriteRefused");
			for (const std::string_view path : { "../Outside.md", "/Absolute.md", "Schemas/../../Up.md", "", "Schemas//Double.md", "C:Drive.md", "Back\\Slash.md" })
			{
				CAPTURE(path);
				const std::vector<GeneratedReferenceFile> files = { GeneratedReferenceFile{ .Path = std::string(path), .Content = "x\n" } };
				const Status written = WriteReferences(files, directory / "Out");
				REQUIRE_FALSE(written.has_value());
				CHECK(written.error().GetCode() == ErrorCode::InvalidArgument);
			}
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(directory / "Out", error));
		}

		// `Editor --dump-reference` calls only DumpReferences, which writes the whole reference set (ADR 0022 decision 13).
		TEST_CASE("ReferenceGenerator: DumpReferences writes the whole reference set")
		{
			Test::EditorTestFixture fixture("ReferenceDump");
			const Test::TempDirectory directory("ReferenceDump");
			const Status dumped = DumpReferences(fixture.GetEngine().GetTypeRegistry(), fixture.GetEngine().GetScriptApiRegistry(), directory / "Reference");
			REQUIRE_MESSAGE(dumped.has_value(), dumped.error().ToString());
			const ReferenceRegistries registries(fixture);
			const auto files = GenerateReferences(registries.GetSources());
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			for (const GeneratedReferenceFile& file : *files)
			{
				CAPTURE(file.Path);
				CHECK(FileSystem::ReadText(directory / "Reference" / FileSystem::PathFromUtf8(file.Path)).value_or(std::string()) == file.Content);
			}
		}

		// The schemas describe the files authors and agents write: every .eproj, .scene, .prefab, .material, .sfx, .meta and
		// .replay under Projects/FeatureTest and Tests/Data validates against its format's schema, except the deliberately
		// invalid or other-version fixtures: Scenes/Invalid/, Formats/ (v0.scene, NewerVersion.scene), Validation/,
		// CoverageGaps/ and Lint/. The project's Automation/Provenance.json files and every line of its batch scaffolds
		// validate against the Provenance and BatchRequest schemas too.
		TEST_CASE("ReferenceGenerator: every authored file validates against its generated format schema")
		{
			Test::EditorTestFixture fixture("ReferenceSchemas");
			const ReferenceRegistries registries(fixture);
			const auto files = GenerateReferences(registries.GetSources());
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			const std::map<std::string, std::string> formats = { { ".eproj", "Project" }, { ".scene", "Scene" }, { ".prefab", "Prefab" },
				{ ".material", "Material" }, { ".sfx", "SoundEffect" }, { ".meta", "AssetMeta" }, { ".replay", "Replay" } };
			std::map<std::string, Json> schemas;
			for (const std::string_view format : { "Project", "Scene", "Prefab", "Material", "SoundEffect", "AssetMeta", "Replay", "Provenance", "BatchRequest" })
				schemas.emplace(std::string(format), ParseSchema(*files, format));

			// (label, text, format) of every document to validate.
			std::vector<std::tuple<std::string, std::string, std::string>> documents;
			const auto addFile = [&documents](const std::filesystem::path& path, std::string label, std::string format)
			{
				const Result<std::string> text = FileSystem::ReadText(path);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				documents.emplace_back(std::move(label), *text, std::move(format));
			};
			std::vector<std::string> extensions;
			for (const auto& [extension, format] : formats)
				extensions.push_back(extension);
			const auto testData = Test::ListTestDataFiles("", extensions);
			REQUIRE_MESSAGE(testData.has_value(), testData.error().ToString());
			for (const std::string& file : *testData)
			{
				if (file.starts_with("Scenes/Invalid/") || file.starts_with("Formats/") || file.starts_with("Validation/") || file.starts_with("CoverageGaps/")
					|| file.starts_with("Lint/") || file.starts_with("BuildConfig/"))
				{
					continue;
				}
				const std::string extension = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(file).extension());
				addFile(Test::GetTestDataPath(file), "Tests/Data/" + file, formats.find(extension)->second);
			}

			const std::filesystem::path project = Test::GetRepositoryRoot() / "Projects" / "FeatureTest";
			const auto projectFiles = FileSystem::ListDirectory(project, true);
			REQUIRE_MESSAGE(projectFiles.has_value(), projectFiles.error().ToString());
			std::vector<std::string> relativePaths;
			for (const std::filesystem::path& path : *projectFiles)
			{
				std::error_code error;
				if (!std::filesystem::is_directory(path, error))
					relativePaths.push_back(FileSystem::PathToUtf8(path.lexically_relative(project)));
			}
			for (std::string& relative : relativePaths)
				std::ranges::replace(relative, '\\', '/');
			std::ranges::sort(relativePaths);
			for (const std::string& relative : relativePaths)
			{
				if (relative.starts_with("Library/") || relative.starts_with("Build/"))
					continue;
				const std::filesystem::path path = project / FileSystem::PathFromUtf8(relative);
				const std::string label = "Projects/FeatureTest/" + relative;
				const std::string extension = FileSystem::PathToUtf8(path.extension());
				if (const auto format = formats.find(extension); format != formats.end())
				{
					addFile(path, label, format->second);
				}
				else if (relative.ends_with("Provenance.json"))
				{
					addFile(path, label, "Provenance");
				}
				else if (extension == ".jsonl")
				{
					const Result<std::string> text = FileSystem::ReadText(path);
					REQUIRE(text.has_value());
					for (const auto& [line, content] : SplitLines(*text))
						documents.emplace_back(std::format("{}:{}", label, line), content, "BatchRequest");
				}
			}

			size_t checked = 0;
			std::set<std::string> checkedFormats;
			for (const auto& [label, text, format] : documents)
			{
				CAPTURE(label);
				const auto instance = JsonReader::Parse(text);
				REQUIRE_MESSAGE(instance.has_value(), instance.error().ToString());
				const Status valid = JsonSchema::Validate(schemas.find(format)->second, *instance);
				CHECK_MESSAGE(valid.has_value(), (valid ? std::string() : valid.error().ToString()));
				checkedFormats.insert(format);
				++checked;
			}
			CHECK(checked >= 100);
			for (const std::string_view format : { "Project", "Scene", "Prefab", "Material", "AssetMeta", "Provenance", "BatchRequest" })
			{
				CAPTURE(format);
				CHECK(checkedFormats.contains(std::string(format)));
			}
		}

		// M16 friction S1-06: FileFormats.md showed the SoundEffect format only by example, and the note grammar (names with
		// sharps and flats, octaves, rests, the seconds' notation) and the limits were written nowhere a builder could read.
		TEST_CASE("ReferenceGenerator: the SoundEffect section lists every field, every wave, the note grammar and the limits")
		{
			Test::EditorTestFixture fixture("ReferenceSoundEffect");
			const ReferenceRegistries registries(fixture);
			const auto files = GenerateReferences(registries.GetSources());
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			const std::string& formats = FindFile(*files, "FileFormats.md")->Content;
			const size_t begin = formats.find("### SoundEffect (`.sfx`)");
			const size_t end = formats.find("## Source formats");
			REQUIRE(begin != std::string::npos);
			REQUIRE(end != std::string::npos);
			REQUIRE(begin < end);
			const std::string section = formats.substr(begin, end - begin);
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			for (const std::string_view name : { "SoundEffect", "SoundLayer", "SoundEnvelope" })
			{
				CAPTURE(name);
				const StructInfo* type = types.FindStruct(name);
				REQUIRE(type != nullptr);
				CHECK(section.find(std::format("#### Fields of {}\n", name)) != std::string::npos);
				for (const auto& field : type->GetFields())
				{
					CAPTURE(field->GetName());
					CHECK(section.find(std::format("| `{}` |", field->GetName())) != std::string::npos);
				}
			}
			const EnumInfo* waves = types.FindEnum("SoundWave");
			REQUIRE(waves != nullptr);
			CHECK(section.find("#### Values of SoundWave\n") != std::string::npos);
			for (const EnumEntry& entry : waves->GetEntries())
				CHECK(section.find(std::format("| `{}` |", entry.Name)) != std::string::npos);

			const Json schema = ParseSchema(*files, "SoundEffect");
			const std::string schemaText = schema.dump();
			for (const std::string& text : { std::string("C, C#, Db, D, D#, Eb, E, F, F#, Gb, G, G#, Ab, A, A#, Bb or B"), std::string("Octave is one digit, 0 to 8"),
					 std::string("for a rest"), std::string("at most 7 digits before and 8 after the point"),
					 std::format("At most {} notes", MaxSoundNotesPerLayer), std::format("1 to {} layers", MaxSoundLayers),
					 std::format("at most {} seconds", MaxSoundEffectSeconds) })
			{
				CAPTURE(text);
				CHECK(section.find(text) != std::string::npos);
				CHECK(schemaText.find(text) != std::string::npos);
			}
		}

		// A schema rejects what its format's reader rejects, as far as the supported keywords can say it.
		TEST_CASE("ReferenceGenerator: the schemas reject wrong headers, unknown keys and malformed handles")
		{
			Test::EditorTestFixture fixture("ReferenceSchemaRejects");
			const ReferenceRegistries registries(fixture);
			const auto files = GenerateReferences(registries.GetSources());
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			const auto rejects = [](const Json& schema, std::string_view text)
			{
				CAPTURE(std::string(text));
				const auto instance = JsonReader::Parse(text);
				REQUIRE(instance.has_value());
				const Status valid = JsonSchema::Validate(schema, *instance);
				REQUIRE_FALSE(valid.has_value());
				CHECK(valid.error().GetCode() == ErrorCode::Validation);
			};
			const Json scene = ParseSchema(*files, "Scene");
			rejects(scene, R"({"Format":"Prefab","Version":1})");
			rejects(scene, R"({"Format":"Scene","Version":2})");
			rejects(scene, R"({"Format":"Scene","Version":1,"Unexpected":true})");
			rejects(scene, R"({"Format":"Scene","Version":1,"Entities":[{"ID":"not-a-handle"}]})");
			rejects(scene, R"({"Format":"Scene","Version":1,"Entities":[{"ID":"0000000000000001","Components":{"Transform":{"Scale":"big"}}}]})");
			rejects(ParseSchema(*files, "Prefab"), R"({"Format":"Prefab","Version":1})");
			rejects(ParseSchema(*files, "AssetMeta"), R"({"Format":"AssetMeta","Version":1,"Handle":"0000000000000001","Type":"Texture","Importer":"NoSuchImporter"})");
			rejects(ParseSchema(*files, "Replay"), R"({"Format":"Replay","Version":1})");
			rejects(ParseSchema(*files, "Project"), R"({"Format":"Project","Version":1,"Window":{"Width":"wide"}})");
			const Json batch = ParseSchema(*files, "BatchRequest");
			rejects(batch, R"({"method":"no.such","params":{}})");
			rejects(batch, R"({"method":"scene.tree","params":{},"id":1})");

			// A scene may hold components this build does not know: they are preserved verbatim (§6).
			const auto unknown = JsonReader::Parse(R"({"Format":"Scene","Version":1,"Entities":[{"ID":"0000000000000001","Components":{"FutureComponent":{"X":1}}}]})");
			REQUIRE(unknown.has_value());
			CHECK(JsonSchema::Validate(scene, *unknown).has_value());
		}

		// M16 review R20: the replay reader accepts a state hash only as 16 lowercase hex digits, while the schema reused the
		// handle pattern, which accepts either case. The schema and the reader now agree on every checkpoint and final hash.
		TEST_CASE("ReferenceGenerator: the Replay schema accepts only the lowercase state hashes the replay reader accepts")
		{
			Test::EditorTestFixture fixture("ReferenceReplayHashes");
			const ReferenceRegistries registries(fixture);
			const auto files = GenerateReferences(registries.GetSources());
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			const Json schema = ParseSchema(*files, "Replay");
			const Result<std::string> text = Test::ReadTestDataText("SettingsObservation/Project/Assets/Tests/Replays/Main.replay");
			REQUIRE(text.has_value());
			Json lowercase = JsonReader::Parse(*text).value_or(Json());
			REQUIRE(lowercase.is_object());
			lowercase["FinalTick"] = 120;
			lowercase["FinalStateHash"] = "00000000000000ff";
			lowercase["Checkpoints"] = Json::array({ Json{ { "Tick", 60 }, { "StateHash", "00000000000000ab" } } });
			Json uppercaseCheckpoint = lowercase;
			uppercaseCheckpoint["Checkpoints"][0]["StateHash"] = "00000000000000AB";
			Json uppercaseFinal = lowercase;
			uppercaseFinal["FinalStateHash"] = "00000000000000FF";

			const std::pair<Json, bool> cases[] = { { lowercase, true }, { uppercaseCheckpoint, false }, { uppercaseFinal, false } };
			for (const auto& [document, accepted] : cases)
			{
				CAPTURE(document.dump());
				ReplayLoadReport report;
				CHECK(ReplayFromJson(document, report, true).has_value() == accepted);
				const Status valid = JsonSchema::Validate(schema, document);
				CHECK(valid.has_value() == accepted);
			}
		}
	}

}
