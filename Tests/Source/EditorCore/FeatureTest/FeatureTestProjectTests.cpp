#include "TestsPCH.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/PathGlob.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"
#include "Support/TestTimeout.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// A bound on the cases that open the whole project without a Library cache (every script imported and type-checked)
		// and run it: tens of seconds in Debug, far below this; only a hang reaches it.
		constexpr double ProjectRunTimeoutSeconds = 600.0;

		// Projects/FeatureTest without its Library/, copied into the fixture's temporary directory, so a run never writes the
		// checkout.
		std::filesystem::path CopyFeatureTestProject(Test::EditorTestFixture& fixture)
		{
			const std::filesystem::path source = Test::GetRepositoryRoot() / "Projects" / "FeatureTest";
			const std::filesystem::path target = fixture.GetDirectory().GetPath() / "FeatureTest";
			std::error_code error;
			std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
			REQUIRE_MESSAGE(!error, error.message());
			std::filesystem::remove_all(target / "Library", error);
			REQUIRE_MESSAGE(!error, error.message());
			return target;
		}

		// The JSON of the checkout's Projects/FeatureTest/<path>, parsed into `document`, which the reader refers to.
		JsonReader ReadProjectFile(Json& document, std::string_view path)
		{
			const std::filesystem::path file = Test::GetRepositoryRoot() / "Projects" / "FeatureTest" / std::filesystem::path(std::string(path));
			const Result<std::string> text = FileSystem::ReadText(file);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			Result<Json> parsed = JsonReader::Parse(*text);
			REQUIRE_MESSAGE(parsed.has_value(), parsed.error().ToString());
			document = std::move(*parsed);
			return JsonReader(document);
		}

		// Calls `visit` with each entity of a scene or prefab document, in file order.
		void ForEachEntity(const JsonReader& document, const std::function<void(const JsonReader&)>& visit)
		{
			const auto entities = document.GetMember("Entities");
			REQUIRE(entities.has_value());
			const auto count = entities->GetArraySize();
			REQUIRE(count.has_value());
			for (size_t index = 0; index < *count; ++index)
			{
				const auto entity = entities->GetElement(index);
				REQUIRE(entity.has_value());
				visit(*entity);
			}
		}

		// An automation client of the fixture's editor whose results stay inline (a test run's coverage report is larger
		// than the offload threshold), with the copied project open.
		Scope<Test::AutomationTestClient> OpenProject(Test::EditorTestFixture& fixture, const std::filesystem::path& project)
		{
			auto client = CreateScope<Test::AutomationTestClient>(fixture.GetEditor(), Test::MakeTestServerSpecification(), false);
			const Result<Json> opened = client->Call("project.open", Json{ { "path", FileSystem::PathToUtf8(project) } });
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			return client;
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Roadmap M14 acceptance (ADR 0022 decision 9). The Timing suite (Assets/Tests/Timing/Timing.test.luau, Clock
		// [0.004, 0.016, 0.0334, 0.083, 0.5, 0.0] at the project's 64 Hz) asserts the 0, 1, 2 and 5 steps of its frames,
		// clamping and dropped time against analytic expectations; this runs it in process, without a GPU and without a
		// Library cache, in every configuration of the unit suite. The filter is a case-insensitive substring of
		// "<suite>/<case>" (and of replay paths), so "Timing/" selects exactly the Timing suite as long as no other suite name
		// ends in "Timing" and no replay path contains "timing/" (decision 9); the run checks that.
		TEST_CASE("FeatureTest: the Timing suite passes with 0, 1, 2 and 5 steps per frame")
		{
			Test::SetTestCaseTimeLimit(ProjectRunTimeoutSeconds);
			Test::EditorTestFixture fixture("FeatureTestTiming");
			const std::filesystem::path project = CopyFeatureTestProject(fixture);
			std::error_code error;
			REQUIRE(std::filesystem::exists(project / "Assets" / "Tests" / "Timing" / "Timing.test.luau", error));
			const Scope<Test::AutomationTestClient> client = OpenProject(fixture, project);
			const Result<Json> run = client->Call("test.run", Json{ { "filter", "Timing/" } });
			REQUIRE_MESSAGE(run.has_value(), run.error().ToString());
			const JsonReader result(*run);
			CHECK(result.ReadMember<bool>("passed") == true);
			const auto suites = result.GetMember("suites");
			REQUIRE(suites.has_value());
			REQUIRE(suites->GetArraySize() == static_cast<size_t>(1));
			const auto suite = suites->GetElement(0);
			REQUIRE(suite.has_value());
			CHECK(suite->ReadMember<std::string>("suite") == std::string("Timing"));
			CHECK(suite->ReadMember<std::string>("scene") == std::string("Assets/Scenes/Timing.scene"));
			// The suite's single case waits for 20 scripted frames, more than two cycles of the clock (13 and 14 steps).
			const auto ticks = suite->ReadMember<uint64_t>("ticks");
			REQUIRE(ticks.has_value());
			CHECK(*ticks > static_cast<uint64_t>(26));
			const auto cases = result.GetMember("cases");
			REQUIRE(cases.has_value());
			const auto count = cases->GetArraySize();
			REQUIRE(count.has_value());
			REQUIRE(*count > static_cast<size_t>(0));
			for (size_t index = 0; index < *count; ++index)
			{
				const auto row = cases->GetElement(index);
				REQUIRE(row.has_value());
				const auto name = row->ReadMember<std::string>("case");
				const auto status = row->ReadMember<std::string>("status");
				REQUIRE(name.has_value());
				REQUIRE(status.has_value());
				const std::string description = *name + ": " + row->ReadMember<std::string>("message").value_or(std::string());
				CHECK_MESSAGE(*status == std::string("passed"), description);
				CHECK(row->ReadMember<std::string>("suite") == std::string("Timing"));
			}
		}

		// ADR 0022 decision 9 (gate 1b, stream C): every PrefabOverrideKind occurs in a simulated FeatureTest scene. The
		// Prefabs suite simulates Prefabs.scene, whose first Crate instance carries an override of each kind; this checks
		// the committed scene against the registered enumeration without a GPU, so a new kind needs its override.
		TEST_CASE("FeatureTest: Prefabs.scene keeps an override of every PrefabOverrideKind")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const EnumInfo* kinds = registry->FindEnum<PrefabOverrideKind>();
			REQUIRE(kinds != nullptr);
			Json document;
			const JsonReader scene = ReadProjectFile(document, "Assets/Scenes/Prefabs.scene");
			std::set<std::string> found;
			ForEachEntity(scene, [&found](const JsonReader& entity)
			{
				const auto components = entity.GetMember("Components");
				REQUIRE(components.has_value());
				const std::optional<JsonReader> prefab = components->FindMember("Prefab");
				if (!prefab)
					return;
				const auto overrides = prefab->GetMember("Overrides");
				REQUIRE(overrides.has_value());
				const auto count = overrides->GetArraySize();
				REQUIRE(count.has_value());
				for (size_t index = 0; index < *count; ++index)
				{
					const auto entry = overrides->GetElement(index);
					REQUIRE(entry.has_value());
					const auto kind = entry->ReadMember<std::string>("Kind");
					REQUIRE(kind.has_value());
					found.insert(*kind);
				}
			});
			for (const EnumEntry& kind : kinds->GetEntries())
				CHECK_MESSAGE(found.contains(kind.Name), "no override of kind ", kind.Name);
		}

		// The nested prefab of §15.5: Cart.prefab was saved from an entity holding two Wheel.prefab instances, which prefab.create
		// keeps live, outermost in the new document (Docs/Decisions/0031-m20-contract.md decision 8): each wheel is an instance
		// root of Wheel.prefab, each hub a member of its wheel's instance, and the cart's own entities carry no instance
		// components, since the cart's instance is unpacked in the copy.
		TEST_CASE("FeatureTest: Cart.prefab keeps its nested Wheel instances live")
		{
			Json document;
			const JsonReader prefab = ReadProjectFile(document, "Assets/Prefabs/Cart.prefab");
			CHECK(prefab.ReadFormatHeader("Prefab", 1, 1).has_value());
			Json wheelMeta;
			const auto wheelHandle = ReadProjectFile(wheelMeta, "Assets/Prefabs/Wheel.prefab.meta").ReadMember<std::string>("Handle");
			REQUIRE(wheelHandle.has_value());
			std::vector<std::string> names;
			std::string wheel;
			ForEachEntity(prefab, [&names, &wheel, &wheelHandle](const JsonReader& entity)
			{
				const auto name = entity.ReadMember<std::string>("Name");
				const auto id = entity.ReadMember<std::string>("ID");
				REQUIRE(name.has_value());
				REQUIRE(id.has_value());
				names.push_back(*name);
				const auto components = entity.GetMember("Components");
				REQUIRE(components.has_value());
				const std::optional<JsonReader> instance = components->FindMember("Prefab");
				const std::optional<JsonReader> link = components->FindMember("PrefabLink");
				if (*name == "Cart" || *name == "Body")
				{
					CHECK_MESSAGE(!instance.has_value(), *name, " is an instance root");
					CHECK_MESSAGE(!link.has_value(), *name, " is linked to a prefab");
					return;
				}
				REQUIRE_MESSAGE(link.has_value(), *name, " is not linked to Wheel.prefab");
				const auto outer = link->GetMember("OuterPrefabEntityID");
				REQUIRE(outer.has_value());
				CHECK_MESSAGE(outer->IsNull(), *name, " names an enclosing instance in the document that holds it outermost");
				if (*name == "Hub")
				{
					CHECK_MESSAGE(!instance.has_value(), "a hub is a member, not an instance root");
					CHECK(link->ReadMember<std::string>("InstanceRoot") == wheel);
					return;
				}
				REQUIRE_MESSAGE(instance.has_value(), *name, " is not a live Wheel.prefab instance");
				CHECK(instance->ReadMember<std::string>("Prefab") == *wheelHandle);
				CHECK(link->ReadMember<std::string>("InstanceRoot") == *id);
				wheel = *id;
			});
			CHECK(names == std::vector<std::string>{ "Cart", "Body", "FrontWheel", "Hub", "BackWheel", "Hub" });
		}

		// Review finding M08 (ADR 0022 decision 9, "Integration"): Export.Exclude leaves Assets/Tests/** out of a plain export,
		// so no asset an export includes may reference an asset below it; Exporter::CheckReferences would refuse such an
		// export. Scripting.scene referenced Assets/Tests/Api/Beep.sfx until the sound moved to Assets/Audio/. This walks the
		// committed documents (every string of every scene, prefab, material and sound effect outside the excluded globs)
		// against the handles of the excluded assets' .meta files, without a GPU.
		TEST_CASE("FeatureTest: no asset a plain export includes references one that Export.Exclude leaves out")
		{
			const std::filesystem::path root = Test::GetRepositoryRoot() / "Projects" / "FeatureTest";
			Json projectDocument;
			const JsonReader project = ReadProjectFile(projectDocument, "FeatureTest.eproj");
			const auto exclude = project.GetMember("Export").and_then([](const JsonReader& exported)
			{
				return exported.GetMember("Exclude");
			});
			REQUIRE_MESSAGE(exclude.has_value(), exclude.error().ToString());
			std::vector<std::string> globs;
			for (size_t index = 0; index < exclude->GetArraySize().value_or(0); ++index)
			{
				const auto glob = exclude->GetElement(index).and_then([](const JsonReader& element)
				{
					return element.ReadString();
				});
				REQUIRE(glob.has_value());
				globs.push_back(*glob);
			}
			REQUIRE_FALSE(globs.empty());
			const auto excluded = [&globs](std::string_view path)
			{
				return std::ranges::any_of(globs, [path](const std::string& glob)
				{
					return MatchesPathGlob(glob, path);
				});
			};

			const Result<std::vector<std::filesystem::path>> files = FileSystem::ListDirectory(root / "Assets", true);
			REQUIRE_MESSAGE(files.has_value(), files.error().ToString());
			std::map<std::string, std::string> excludedHandles; // handle -> project-relative source path
			std::vector<std::pair<std::string, std::filesystem::path>> documents;
			for (const std::filesystem::path& file : *files)
			{
				std::string relative = FileSystem::PathToUtf8(file.lexically_relative(root));
				std::ranges::replace(relative, '\\', '/');
				const std::string extension = FileSystem::PathToUtf8(file.extension());
				if (extension == ".meta")
				{
					const std::string source = relative.substr(0, relative.size() - extension.size());
					if (!excluded(source))
						continue;
					Json meta;
					const JsonReader reader = ReadProjectFile(meta, relative);
					const auto handle = reader.ReadMember<std::string>("Handle");
					REQUIRE(handle.has_value());
					excludedHandles.emplace(*handle, source);
					const std::optional<JsonReader> subAssets = reader.FindMember("SubAssets");
					for (size_t index = 0; subAssets && index < subAssets->GetArraySize().value_or(0); ++index)
					{
						const auto subHandle = subAssets->GetElement(index).and_then([](const JsonReader& element)
						{
							return element.ReadMember<std::string>("Handle");
						});
						if (subHandle)
							excludedHandles.emplace(*subHandle, source);
					}
				}
				else if ((extension == ".scene" || extension == ".prefab" || extension == ".material" || extension == ".sfx") && !excluded(relative))
				{
					documents.emplace_back(relative, file);
				}
			}
			REQUIRE_FALSE(excludedHandles.empty());
			REQUIRE(documents.size() >= 20);

			std::vector<std::string> references;
			for (const auto& [relative, file] : documents)
			{
				Json document;
				const JsonReader reader = ReadProjectFile(document, relative);
				std::vector<JsonReader> pending{ reader };
				while (!pending.empty())
				{
					const JsonReader value = std::move(pending.back());
					pending.pop_back();
					if (value.IsObject())
					{
						for (const std::string& name : value.GetMemberNames().value_or(std::vector<std::string>{}))
							pending.push_back(*value.GetMember(name));
					}
					else if (value.IsArray())
					{
						for (size_t index = 0; index < value.GetArraySize().value_or(0); ++index)
							pending.push_back(*value.GetElement(index));
					}
					else if (const Result<std::string> text = value.ReadString(); text)
					{
						if (const auto found = excludedHandles.find(*text); found != excludedHandles.end())
							references.push_back(std::format("{} {} references {}", relative, value.GetPointer(), found->second));
					}
				}
			}
			std::string report;
			for (const std::string& reference : references)
				report += reference + "\n";
			INFO(report);
			CHECK(references.empty());
		}

		// Roadmap M14 acceptance (ADR 0022 decision 9): the committed FinalStateHash of Assets/Tests/Replays/Determinism.replay
		// is reproduced by strict replay verification; the unit suite runs this in Debug and Release, without a GPU.
		TEST_CASE("FeatureTest: Determinism.replay matches its committed hash in both editor configurations")
		{
			Test::SetTestCaseTimeLimit(ProjectRunTimeoutSeconds);
			Test::EditorTestFixture fixture("FeatureTestDeterminism");
			const std::filesystem::path project = CopyFeatureTestProject(fixture);
			const Result<std::string> replay = FileSystem::ReadText(project / "Assets" / "Tests" / "Replays" / "Determinism.replay");
			REQUIRE_MESSAGE(replay.has_value(), replay.error().ToString());
			const auto document = JsonReader::Parse(*replay);
			REQUIRE(document.has_value());
			const JsonReader header(*document);
			const auto committed = header.ReadMember<std::string>("FinalStateHash");
			REQUIRE(committed.has_value());
			CHECK(header.ReadMember<uint64_t>("FinalTick") == static_cast<uint64_t>(600));

			const Scope<Test::AutomationTestClient> client = OpenProject(fixture, project);
			const Result<Json> verified = client->Call("input.replay",
				Json{ { "path", "Assets/Tests/Replays/Determinism.replay" }, { "verify", true }, { "strictHash", true } });
			REQUIRE_MESSAGE(verified.has_value(), verified.error().ToString());
			const JsonReader result(*verified);
			CHECK(result.ReadMember<std::string>("stateHash") == *committed);
			CHECK(result.ReadMember<bool>("passed") == true);
			CHECK(result.ReadMember<bool>("hashMatched") == true);
			CHECK(result.ReadMember<uint64_t>("finalTick") == static_cast<uint64_t>(600));
		}
	}

}
