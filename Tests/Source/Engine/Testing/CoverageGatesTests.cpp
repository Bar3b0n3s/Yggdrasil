#include "TestsPCH.h"
#include "Engine/Testing/CoverageGates.h"

#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		constexpr std::array<TestSuiteSettings::Mode, 3> AllModes = { TestSuiteSettings::Mode::Editor, TestSuiteSettings::Mode::Release,
			TestSuiteSettings::Mode::Dist };

		std::string_view ModeName(TestSuiteSettings::Mode mode)
		{
			switch (mode)
			{
				case TestSuiteSettings::Mode::Editor:  return "Editor";
				case TestSuiteSettings::Mode::Release: return "Release";
				case TestSuiteSettings::Mode::Dist:    return "Dist";
			}
			return "Editor";
		}

		TestCoverageCounter Counter(std::string id, std::string_view kind, std::vector<TestSuiteSettings::Mode> modes, uint32_t count)
		{
			return TestCoverageCounter{ .Id = std::move(id), .Kind = std::string(kind), .Modes = std::move(modes), .Count = count };
		}

		// One covered All-mode counter for every per-mode gate.
		std::vector<TestCoverageCounter> CoveredCounters()
		{
			const std::vector<TestSuiteSettings::Mode> all(AllModes.begin(), AllModes.end());
			return {
				Counter("api:Physics.Raycast", CoverageKindFunction, all, 3),
				Counter("callback:Behaviour.OnStart", CoverageKindCallback, all, 1),
				Counter("component:Camera", CoverageKindComponent, all, 2),
				Counter("enum:ProjectionType/Orthographic", CoverageKindEnumValue, all, 1),
				Counter("field:Camera/Projection", CoverageKindField, all, 1),
				Counter("format:Texture/.png", CoverageKindFormat, all, 1),
				Counter("proxy:Camera.Primary/read", CoverageKindProxyRead, all, 4),
			};
		}

		// The counters of a seeded-gap report: coverage.counters of a TestRunResult JSON (Testing/TestResults.h).
		std::vector<TestCoverageCounter> ReadCounters(const JsonReader& report)
		{
			std::vector<TestCoverageCounter> counters;
			const auto coverage = report.GetMember("coverage");
			REQUIRE(coverage.has_value());
			const auto list = coverage->GetMember("counters");
			REQUIRE(list.has_value());
			const auto size = list->GetArraySize();
			REQUIRE(size.has_value());
			for (size_t index = 0; index < *size; ++index)
			{
				const auto element = list->GetElement(index);
				REQUIRE(element.has_value());
				TestCoverageCounter counter;
				const auto id = element->ReadMember<std::string>("id");
				const auto kind = element->ReadMember<std::string>("kind");
				const auto count = element->ReadMember<uint32_t>("count");
				const auto modes = element->GetMember("modes");
				REQUIRE((id.has_value() && kind.has_value() && count.has_value() && modes.has_value()));
				counter.Id = *id;
				counter.Kind = *kind;
				counter.Count = *count;
				const auto modeCount = modes->GetArraySize();
				REQUIRE(modeCount.has_value());
				for (size_t modeIndex = 0; modeIndex < *modeCount; ++modeIndex)
				{
					const auto mode = modes->GetElement(modeIndex);
					REQUIRE(mode.has_value());
					const auto name = mode->ReadString();
					REQUIRE(name.has_value());
					const auto found = std::ranges::find_if(AllModes, [&name](TestSuiteSettings::Mode candidate)
					{
						return ModeName(candidate) == *name;
					});
					REQUIRE(found != AllModes.end());
					counter.Modes.push_back(*found);
				}
				counters.push_back(std::move(counter));
			}
			return counters;
		}

		std::vector<std::string> ReadStrings(const JsonReader& object, std::string_view key)
		{
			std::vector<std::string> values;
			const auto list = object.GetMember(key);
			REQUIRE(list.has_value());
			const auto size = list->GetArraySize();
			REQUIRE(size.has_value());
			for (size_t index = 0; index < *size; ++index)
			{
				const auto element = list->GetElement(index);
				REQUIRE(element.has_value());
				const auto value = element->ReadString();
				REQUIRE(value.has_value());
				values.push_back(*value);
			}
			return values;
		}

		// The gates of a seeded-gap report: coverage.gates of a TestRunResult JSON.
		std::vector<TestCoverageGateResult> ReadGates(const JsonReader& report)
		{
			std::vector<TestCoverageGateResult> gates;
			const auto coverage = report.GetMember("coverage");
			REQUIRE(coverage.has_value());
			const auto list = coverage->GetMember("gates");
			REQUIRE(list.has_value());
			const auto size = list->GetArraySize();
			REQUIRE(size.has_value());
			for (size_t index = 0; index < *size; ++index)
			{
				const auto element = list->GetElement(index);
				REQUIRE(element.has_value());
				const auto gate = element->ReadMember<std::string>("gate");
				const auto evaluated = element->ReadMember<bool>("evaluated");
				const auto passed = element->ReadMember<bool>("passed");
				REQUIRE((gate.has_value() && evaluated.has_value() && passed.has_value()));
				gates.push_back(TestCoverageGateResult{ .Gate = *gate, .Evaluated = *evaluated, .Passed = *passed, .Missing = ReadStrings(*element, "missing") });
			}
			return gates;
		}

		const TestCoverageGateResult* FindGate(const std::vector<TestCoverageGateResult>& results, CoverageGate gate)
		{
			const auto found = std::ranges::find(results, CoverageGateName(gate), &TestCoverageGateResult::Gate);
			return found == results.end() ? nullptr : &*found;
		}

	}

	TEST_SUITE("Testing")
	{
		// Roadmap M14 acceptance. Tests/Data/CoverageGaps/Gate<g>/<Mode>/ holds FeatureTest-<Mode>.json (a TestRunResult JSON)
		// and Expected.json {"Format": "CoverageGap", "Version": 1, "Gate", "Mode", "Missing"} (ADR 0022 decision 6). The
		// report's own coverage.gates must equal the evaluation of its counters, so the Python feature suite, which reads the
		// gates, and this test, which re-evaluates the counters, judge the same fixture.
		TEST_CASE("CoverageGates: each seeded-gap fixture makes exactly its gate (and mode) fail and names the gap")
		{
			size_t fixtures = 0;
			for (const CoverageGate gate : PerModeCoverageGates)
			{
				for (const TestSuiteSettings::Mode mode : AllModes)
				{
					CAPTURE(CoverageGateName(gate));
					CAPTURE(ModeName(mode));
					const std::string directory = std::format("CoverageGaps/Gate{}/{}", CoverageGateName(gate), ModeName(mode));
					const auto reportText = Test::ReadTestDataText(std::format("{}/FeatureTest-{}.json", directory, ModeName(mode)));
					const auto expectedText = Test::ReadTestDataText(directory + "/Expected.json");
					REQUIRE_MESSAGE(reportText.has_value(), reportText.error().ToString());
					REQUIRE_MESSAGE(expectedText.has_value(), expectedText.error().ToString());
					const auto report = JsonReader::Parse(*reportText);
					const auto expected = JsonReader::Parse(*expectedText);
					REQUIRE((report.has_value() && expected.has_value()));
					const JsonReader expectedReader(*expected);
					CHECK(expectedReader.ReadMember<std::string>("Gate") == std::string(CoverageGateName(gate)));
					CHECK(expectedReader.ReadMember<std::string>("Mode") == std::string(ModeName(mode)));
					const std::vector<std::string> missing = ReadStrings(expectedReader, "Missing");
					REQUIRE_FALSE(missing.empty());

					const std::vector<TestCoverageCounter> counters = ReadCounters(JsonReader(*report));
					const auto results = EvaluateCoverageGates(counters, mode);
					REQUIRE_MESSAGE(results.has_value(), results.error().ToString());
					REQUIRE(results->size() == PerModeCoverageGates.size());
					for (const TestCoverageGateResult& result : *results)
					{
						CAPTURE(result.Gate);
						CHECK(result.Evaluated);
						const bool seeded = result.Gate == CoverageGateName(gate);
						CHECK(result.Passed == !seeded);
						if (seeded)
							CHECK(result.Missing == missing);
						else
							CHECK(result.Missing.empty());
					}
					const std::vector<TestCoverageGateResult> recorded = ReadGates(JsonReader(*report));
					REQUIRE(recorded.size() == results->size());
					for (size_t index = 0; index < recorded.size(); ++index)
					{
						CAPTURE(recorded[index].Gate);
						CHECK(recorded[index].Gate == (*results)[index].Gate);
						CHECK(recorded[index].Evaluated == (*results)[index].Evaluated);
						CHECK(recorded[index].Passed == (*results)[index].Passed);
						CHECK(recorded[index].Missing == (*results)[index].Missing);
					}
					++fixtures;
				}
			}
			CHECK(fixtures == PerModeCoverageGates.size() * AllModes.size());
		}

		TEST_CASE("CoverageGates: an EditorOnly entry counts only in the editor run")
		{
			std::vector<TestCoverageCounter> counters = CoveredCounters();
			counters.push_back(Counter("api:Test.ReloadScript", CoverageKindFunction, { TestSuiteSettings::Mode::Editor }, 0));
			std::ranges::sort(counters, {}, &TestCoverageCounter::Id);

			const auto editor = EvaluateCoverageGates(counters, TestSuiteSettings::Mode::Editor);
			REQUIRE(editor.has_value());
			const TestCoverageGateResult* api = FindGate(*editor, CoverageGate::ScriptApi);
			REQUIRE(api != nullptr);
			CHECK_FALSE(api->Passed);
			CHECK(api->Missing == std::vector<std::string>{ "api:Test.ReloadScript" });

			for (const TestSuiteSettings::Mode mode : { TestSuiteSettings::Mode::Release, TestSuiteSettings::Mode::Dist })
			{
				const auto exported = EvaluateCoverageGates(counters, mode);
				REQUIRE(exported.has_value());
				CHECK(std::ranges::all_of(*exported, [](const TestCoverageGateResult& result)
				{
					return result.Evaluated && result.Passed;
				}));
			}

			counters = CoveredCounters();
			counters.push_back(Counter("api:Physics.Raycast/argument3/QueryMode/All", CoverageKindArgumentValue,
				std::vector<TestSuiteSettings::Mode>(AllModes.begin(), AllModes.end()), 0));
			const auto dist = EvaluateCoverageGates(counters, TestSuiteSettings::Mode::Dist);
			REQUIRE(dist.has_value());
			const TestCoverageGateResult* distApi = FindGate(*dist, CoverageGate::ScriptApi);
			REQUIRE(distApi != nullptr);
			CHECK(distApi->Missing == std::vector<std::string>{ "api:Physics.Raycast/argument3/QueryMode/All" });
		}

		TEST_CASE("CoverageGates: a gate without counters fails instead of passing vacuously")
		{
			std::vector<TestCoverageCounter> counters = CoveredCounters();
			std::erase_if(counters, [](const TestCoverageCounter& counter)
			{
				return counter.Kind == CoverageKindFormat;
			});
			const auto results = EvaluateCoverageGates(counters, TestSuiteSettings::Mode::Release);
			REQUIRE(results.has_value());
			const TestCoverageGateResult* formats = FindGate(*results, CoverageGate::Formats);
			REQUIRE(formats != nullptr);
			CHECK(formats->Evaluated);
			CHECK_FALSE(formats->Passed);
			CHECK(formats->Missing == std::vector<std::string>{ "<no counters>" });
		}

		TEST_CASE("CoverageGates: an unknown kind, an invalid mode list, a duplicate id or a foreign prefix is rejected")
		{
			const std::vector<TestSuiteSettings::Mode> all(AllModes.begin(), AllModes.end());
			const std::array<TestCoverageCounter, 7> invalid = {
				Counter("api:Physics.Raycast", "call", all, 1),
				Counter("api:Physics.Raycast", CoverageKindFunction, {}, 1),
				Counter("component:Camera", CoverageKindComponent, all, 2),
				Counter("api:Camera", CoverageKindComponent, all, 1),
				// Every entry exists in the Editor; components, enum values and formats exist in all three modes.
				Counter("api:Physics.Overlap", CoverageKindFunction, { TestSuiteSettings::Mode::Release, TestSuiteSettings::Mode::Dist }, 1),
				Counter("component:Text", CoverageKindComponent, { TestSuiteSettings::Mode::Editor }, 1),
				Counter("format:Font/.otf", CoverageKindFormat, { TestSuiteSettings::Mode::Editor, TestSuiteSettings::Mode::Release }, 1),
			};
			for (size_t index = 0; index < invalid.size(); ++index)
			{
				CAPTURE(index);
				std::vector<TestCoverageCounter> counters = CoveredCounters();
				counters.push_back(invalid[index]);
				const auto results = EvaluateCoverageGates(counters, TestSuiteSettings::Mode::Editor);
				REQUIRE_FALSE(results.has_value());
				CHECK(results.error().GetCode() == ErrorCode::Validation);
				CHECK(results.error().ToString().find(invalid[index].Id) != std::string::npos);
			}
		}

		TEST_CASE("CoverageGates: unevaluated gates never pass")
		{
			const std::vector<TestCoverageGateResult> gates = MakeUnevaluatedCoverageGates();
			REQUIRE(gates.size() == PerModeCoverageGates.size());
			for (size_t index = 0; index < gates.size(); ++index)
			{
				CHECK(gates[index].Gate == CoverageGateName(PerModeCoverageGates[index]));
				CHECK_FALSE(gates[index].Evaluated);
				CHECK_FALSE(gates[index].Passed);
				CHECK(gates[index].Missing.empty());
			}
		}

		TEST_CASE("CoverageGates: gate names, counter kinds and run modes map as documented")
		{
			constexpr std::array<std::pair<CoverageGate, std::string_view>, 10> Names = { {
				{ CoverageGate::Components, "1" },
				{ CoverageGate::EnumValues, "1b" },
				{ CoverageGate::ScriptApi, "2" },
				{ CoverageGate::Callbacks, "3" },
				{ CoverageGate::ProxyFields, "4" },
				{ CoverageGate::AutomationMethods, "5" },
				{ CoverageGate::ValidationCodes, "6" },
				{ CoverageGate::Descriptions, "7" },
				{ CoverageGate::ProjectSettings, "8" },
				{ CoverageGate::Formats, "9" },
			} };
			for (const auto& [gate, name] : Names)
			{
				CHECK(CoverageGateName(gate) == name);
				CHECK(ParseCoverageGateName(name) == gate);
				CHECK(IsPerModeCoverageGate(gate) == (std::ranges::find(PerModeCoverageGates, gate) != PerModeCoverageGates.end()));
			}
			CHECK_FALSE(ParseCoverageGateName("1B").has_value());

			constexpr std::array<std::pair<std::string_view, CoverageGate>, 14> Kinds = { {
				{ CoverageKindComponent, CoverageGate::Components },
				{ CoverageKindField, CoverageGate::Components },
				{ CoverageKindEnumValue, CoverageGate::EnumValues },
				{ CoverageKindFunction, CoverageGate::ScriptApi },
				{ CoverageKindMethod, CoverageGate::ScriptApi },
				{ CoverageKindOperator, CoverageGate::ScriptApi },
				{ CoverageKindConstructor, CoverageGate::ScriptApi },
				{ CoverageKindPropertyRead, CoverageGate::ScriptApi },
				{ CoverageKindPropertyWrite, CoverageGate::ScriptApi },
				{ CoverageKindArgumentValue, CoverageGate::ScriptApi },
				{ CoverageKindCallback, CoverageGate::Callbacks },
				{ CoverageKindProxyRead, CoverageGate::ProxyFields },
				{ CoverageKindProxyWrite, CoverageGate::ProxyFields },
				{ CoverageKindFormat, CoverageGate::Formats },
			} };
			for (const auto& [kind, gate] : Kinds)
				CHECK(GetCoverageGateOfKind(kind) == gate);
			CHECK_FALSE(GetCoverageGateOfKind("call").has_value());

			CHECK(ToTestModes(RunModes::All) == std::vector<TestSuiteSettings::Mode>(AllModes.begin(), AllModes.end()));
			CHECK(ToTestModes(RunModes::EditorOnly) == std::vector<TestSuiteSettings::Mode>{ TestSuiteSettings::Mode::Editor });
			CHECK(ToTestModes(RunModes::None).empty());
			CHECK(ToRunModes(TestSuiteSettings::Mode::Release) == RunModes::Release);
		}

		TEST_CASE("CoverageGates: script API counters keep distinct kinds for gates 2, 3 and 4")
		{
			ScriptApiCoverage baseline{ .Mode = RunModes::Editor, .Members = {} };
			baseline.Members.push_back(ScriptApiCounter{ .Owner = "Behaviour", .Member = "OnHotReload", .Kind = ScriptApiMemberKind::Callback, .Modes = RunModes::EditorOnly, .Calls = 0, .Reads = 0, .Writes = 0, .RequiresRead = false, .RequiresWrite = false, .EnumValues = {} });
			baseline.Members.push_back(ScriptApiCounter{ .Owner = "Camera", .Member = "Primary", .Kind = ScriptApiMemberKind::ProxyField, .Modes = RunModes::All, .Calls = 0, .Reads = 2, .Writes = 0, .RequiresRead = true, .RequiresWrite = true, .EnumValues = {} });
			baseline.Members.push_back(ScriptApiCounter{ .Owner = "Entity", .Member = "Name", .Kind = ScriptApiMemberKind::Property, .Modes = RunModes::All, .Calls = 0, .Reads = 0, .Writes = 0, .RequiresRead = true, .RequiresWrite = false, .EnumValues = {} });
			baseline.Members.push_back(ScriptApiCounter{ .Owner = "Input", .Member = "SetCursorMode", .Kind = ScriptApiMemberKind::Function, .Modes = RunModes::All, .Calls = 1, .Reads = 0, .Writes = 0, .RequiresRead = false, .RequiresWrite = false, .EnumValues = { ScriptEnumCounter{ .ArgumentIndex = 1, .EnumName = "CursorMode", .ValueName = "Locked", .Count = 0 } } });
			ScriptApiCoverage current = baseline;
			current.Members[0].Calls = 1;
			current.Members[1].Reads = 5;
			current.Members[2].Reads = 1;
			current.Members[3].Calls = 3;
			current.Members[3].EnumValues[0].Count = 2;

			const auto counters = MakeScriptApiCoverageCounters(current, baseline);
			REQUIRE_MESSAGE(counters.has_value(), counters.error().ToString());
			const std::vector<std::pair<std::string, std::string>> expected = {
				{ "api:Entity.Name/read", std::string(CoverageKindPropertyRead) },
				{ "api:Input.SetCursorMode", std::string(CoverageKindFunction) },
				{ "api:Input.SetCursorMode/argument1/CursorMode/Locked", std::string(CoverageKindArgumentValue) },
				{ "callback:Behaviour.OnHotReload", std::string(CoverageKindCallback) },
				{ "proxy:Camera.Primary/read", std::string(CoverageKindProxyRead) },
				{ "proxy:Camera.Primary/write", std::string(CoverageKindProxyWrite) },
			};
			REQUIRE(counters->size() == expected.size());
			const std::array<uint32_t, 6> counts = { 1, 2, 2, 1, 3, 0 };
			for (size_t index = 0; index < expected.size(); ++index)
			{
				CAPTURE(index);
				CHECK((*counters)[index].Id == expected[index].first);
				CHECK((*counters)[index].Kind == expected[index].second);
				CHECK((*counters)[index].Count == counts[index]);
			}
			CHECK((*counters)[3].Modes == std::vector<TestSuiteSettings::Mode>{ TestSuiteSettings::Mode::Editor });

			ScriptApiCoverage other = baseline;
			other.Mode = RunModes::Dist;
			const auto mismatch = MakeScriptApiCoverageCounters(current, other);
			REQUIRE_FALSE(mismatch.has_value());
			CHECK(mismatch.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("SceneCoverage: components, non-default fields and enum values of simulated scenes are counted")
		{
			Test::SceneTestFixture fixture;
			SceneCoverageRecorder recorder(fixture.GetRegistry());
			const auto initial = recorder.GetCounters();
			REQUIRE_FALSE(initial.empty());
			CHECK(std::ranges::is_sorted(initial, {}, &TestCoverageCounter::Id));
			CHECK(std::ranges::all_of(initial, [](const TestCoverageCounter& counter)
			{
				return counter.Count == 0;
			}));

			Entity camera = fixture.GetScene().CreateEntity("Camera");
			CameraComponent& component = camera.AddComponent<CameraComponent>();
			component.Projection = ProjectionType::Orthographic;
			REQUIRE(recorder.Record(fixture.GetScene()).has_value());

			const auto counters = recorder.GetCounters();
			const auto count = [&counters](std::string_view id) -> std::optional<uint32_t>
			{
				const auto found = std::ranges::find(counters, id, &TestCoverageCounter::Id);
				return found == counters.end() ? std::nullopt : std::optional<uint32_t>(found->Count);
			};
			CHECK(count("component:Camera") == 1u);
			CHECK(count("field:Camera/Projection") == 1u);
			CHECK(count("field:Camera/VerticalFov") == 0u);
			CHECK(count("enum:ProjectionType/Orthographic") == 1u);
			CHECK(count("enum:ProjectionType/Perspective") == 0u);
			CHECK(count("component:Transform") >= 1u);
			CHECK(count("field:Prefab/Overrides/*/Kind").has_value());

			recorder.Reset();
			CHECK(std::ranges::all_of(recorder.GetCounters(), [](const TestCoverageCounter& counter)
			{
				return counter.Count == 0;
			}));
		}

		// ADR 0022 decision 4: a leaf inside an array element compares with the element struct's default (the component's
		// default has no elements), each element counts, and arrays of non-structs are leaves compared whole.
		TEST_CASE("SceneCoverage: array element fields compare with the element default and containers of values compare whole")
		{
			Test::SceneTestFixture fixture;
			SceneCoverageRecorder recorder(fixture.GetRegistry());
			PrefabInstanceComponent prefab;
			prefab.Overrides.push_back(PrefabOverride{ .PrefabEntityID = UUID(0x71), .Kind = PrefabOverrideKind::RemoveComponent, .Component = "Camera", .Field = {}, .Value = {} });
			prefab.Overrides.push_back(PrefabOverride{ .PrefabEntityID = UUID(0x72), .Kind = PrefabOverrideKind::RemoveComponent, .Component = "Text", .Field = {}, .Value = {} });
			fixture.GetScene().CreateEntity("Instance").AddComponent<PrefabInstanceComponent>(prefab);
			MeshRendererComponent renderer;
			renderer.Materials.emplace_back();
			renderer.Materials.back().SetHandle(UUID(0x81));
			fixture.GetScene().CreateEntity("Renderer").AddComponent<MeshRendererComponent>(renderer);
			fixture.GetScene().CreateEntity("Default").AddComponent<MeshRendererComponent>();
			REQUIRE(recorder.Record(fixture.GetScene()).has_value());

			const auto counters = recorder.GetCounters();
			const auto find = [&counters](std::string_view id) -> const TestCoverageCounter*
			{
				const auto found = std::ranges::find(counters, id, &TestCoverageCounter::Id);
				return found == counters.end() ? nullptr : &*found;
			};
			const auto count = [&find](std::string_view id) -> std::optional<uint32_t>
			{
				const TestCoverageCounter* counter = find(id);
				return counter == nullptr ? std::nullopt : std::optional<uint32_t>(counter->Count);
			};
			CHECK(count("component:Prefab") == 1u);
			CHECK(count("field:Prefab/Overrides/*/Kind") == 2u);
			CHECK(count("field:Prefab/Overrides/*/Component") == 2u);
			CHECK(count("field:Prefab/Overrides/*/Field") == 0u);
			CHECK(count("field:Prefab/Overrides/*/PrefabEntityID") == 2u);
			CHECK(count("field:Prefab/Prefab") == 0u);
			CHECK_FALSE(count("field:Prefab/Overrides").has_value());
			CHECK(count("enum:PrefabOverrideKind/RemoveComponent") == 2u);
			CHECK(count("enum:PrefabOverrideKind/Field") == 0u);
			CHECK(count("component:MeshRenderer") == 2u);
			CHECK(count("field:MeshRenderer/Materials") == 1u);
			CHECK_FALSE(count("field:MeshRenderer/Materials/*").has_value());
			// Kinds and modes follow decision 2: fields follow FieldMeta::Modes, enum values exist in every mode.
			const TestCoverageCounter* kind = find("field:Prefab/Overrides/*/Kind");
			REQUIRE(kind != nullptr);
			CHECK(kind->Kind == CoverageKindField);
			CHECK(kind->Modes == std::vector<TestSuiteSettings::Mode>(AllModes.begin(), AllModes.end()));
			const TestCoverageCounter* value = find("enum:PrefabOverrideKind/EntityKey");
			REQUIRE(value != nullptr);
			CHECK(value->Kind == CoverageKindEnumValue);
			CHECK(value->Modes == std::vector<TestSuiteSettings::Mode>(AllModes.begin(), AllModes.end()));
			// Every counter of a recorder is valid input for the evaluation in every mode.
			for (const TestSuiteSettings::Mode mode : AllModes)
				CHECK(EvaluateCoverageGates(counters, mode).has_value());
		}

		TEST_CASE("CoverageGates: a mode listed twice or an id that names no entry after its prefix is rejected")
		{
			const std::array<TestCoverageCounter, 2> invalid = {
				Counter("api:Physics.Overlap", CoverageKindFunction, { TestSuiteSettings::Mode::Editor, TestSuiteSettings::Mode::Editor }, 1),
				Counter("callback:", CoverageKindCallback, std::vector<TestSuiteSettings::Mode>(AllModes.begin(), AllModes.end()), 1),
			};
			for (const TestCoverageCounter& counter : invalid)
			{
				CAPTURE(counter.Id);
				std::vector<TestCoverageCounter> counters = CoveredCounters();
				counters.push_back(counter);
				const auto results = EvaluateCoverageGates(counters, TestSuiteSettings::Mode::Release);
				REQUIRE_FALSE(results.has_value());
				CHECK(results.error().GetCode() == ErrorCode::Validation);
				CHECK(results.error().ToString().find(counter.Id) != std::string::npos);
			}
		}

		TEST_CASE("SceneCoverage: a scene of another type registry is refused")
		{
			Test::SceneTestFixture first;
			Test::SceneTestFixture second;
			SceneCoverageRecorder recorder(first.GetRegistry());
			const Status recorded = recorder.Record(second.GetScene());
			REQUIRE_FALSE(recorded.has_value());
			CHECK(recorded.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("CoverageGates: script API snapshots of different registries or a count going backwards are refused")
		{
			ScriptApiCoverage baseline{ .Mode = RunModes::Release, .Members = {} };
			baseline.Members.push_back(ScriptApiCounter{ .Owner = "Physics", .Member = "Raycast", .Kind = ScriptApiMemberKind::Function, .Modes = RunModes::All, .Calls = 4, .Reads = 0, .Writes = 0, .RequiresRead = false, .RequiresWrite = false, .EnumValues = {} });
			baseline.Members.push_back(ScriptApiCounter{ .Owner = "Math", .Member = "Pi", .Kind = ScriptApiMemberKind::Constant, .Modes = RunModes::All, .Calls = 0, .Reads = 0, .Writes = 0, .RequiresRead = false, .RequiresWrite = false, .EnumValues = {} });
			ScriptApiCoverage current = baseline;
			current.Members[0].Calls = 9;
			const auto counters = MakeScriptApiCoverageCounters(current, baseline);
			REQUIRE(counters.has_value());
			// Constants are not call targets: no counter.
			REQUIRE(counters->size() == 1);
			CHECK((*counters)[0].Id == "api:Physics.Raycast");
			CHECK((*counters)[0].Count == 5u);

			ScriptApiCoverage backwards = baseline;
			backwards.Members[0].Calls = 3;
			const auto decreased = MakeScriptApiCoverageCounters(backwards, baseline);
			REQUIRE_FALSE(decreased.has_value());
			CHECK(decreased.error().GetCode() == ErrorCode::InvalidArgument);

			ScriptApiCoverage other = current;
			other.Members[0].Member = "Overlap";
			CHECK_FALSE(MakeScriptApiCoverageCounters(other, baseline).has_value());
			ScriptApiCoverage shorter = current;
			shorter.Members.pop_back();
			CHECK_FALSE(MakeScriptApiCoverageCounters(shorter, baseline).has_value());

			// Counts saturate to uint32 (TestResults.h).
			ScriptApiCoverage huge = current;
			huge.Members[0].Calls = 1ull << 40;
			const auto saturated = MakeScriptApiCoverageCounters(huge, baseline);
			REQUIRE(saturated.has_value());
			CHECK((*saturated)[0].Count == std::numeric_limits<uint32_t>::max());
		}

		// ADR 0022 decision 5: each loaded handle counts once, by the extension of its reference path before '#', ASCII
		// case-folded; engine assets, unknown handles and extensions without a source format are ignored.
		TEST_CASE("FormatCoverage: each loaded handle counts once by its source extension")
		{
			Test::InMemoryAssetManager assets;
			const AssetHandle photo(0x5101);
			const AssetHandle second(0x5102);
			const AssetHandle mesh(0x5103);
			const AssetHandle notes(0x5104);
			const AssetHandle engine(0x5105);
			assets.Publish(photo, CreateRef<TextureData>(), "Assets/Formats/Photo.JPEG");
			assets.Publish(second, CreateRef<TextureData>(), "Assets/Formats/Other.jpeg");
			assets.Publish(mesh, CreateRef<MeshData>(), "Assets/Formats/Box.glb#mesh:0:Box");
			assets.Publish(notes, CreateRef<TextureData>(), "Assets/Formats/Notes.txt");
			assets.Publish(engine, CreateRef<MeshData>(), "engine://Meshes/Cube");

			FormatCoverageRecorder recorder;
			const auto count = [&recorder](std::string_view id) -> std::optional<uint32_t>
			{
				const auto counters = recorder.GetCounters();
				const auto found = std::ranges::find(counters, id, &TestCoverageCounter::Id);
				return found == counters.end() ? std::nullopt : std::optional<uint32_t>(found->Count);
			};
			const auto initial = recorder.GetCounters();
			REQUIRE_FALSE(initial.empty());
			CHECK(std::ranges::is_sorted(initial, {}, &TestCoverageCounter::Id));
			CHECK(std::ranges::none_of(initial, [](const TestCoverageCounter& counter)
			{
				return counter.Id == "format:Environment/.exr";
			}));

			for (int frame = 0; frame < 3; ++frame)
				recorder.RecordLoad(assets, photo);
			recorder.RecordLoad(assets, second);
			recorder.RecordLoad(assets, mesh);
			recorder.RecordLoad(assets, notes);
			recorder.RecordLoad(assets, engine);
			recorder.RecordLoad(assets, AssetHandle(0x5199));
			CHECK(count("format:Texture/.jpeg") == 2u);
			CHECK(count("format:Gltf/.glb") == 1u);
			CHECK(count("format:Texture/.png") == 0u);
			CHECK_FALSE(count("format:Texture/.txt").has_value());

			recorder.Reset();
			CHECK(count("format:Texture/.jpeg") == 0u);
			recorder.RecordLoad(assets, photo);
			CHECK(count("format:Texture/.jpeg") == 1u);
		}
	}

}
