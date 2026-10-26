#include "TestsPCH.h"
#include "Engine/Native/NativeComponents.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/NativeTestModule.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		// One entity whose preserved components hold native ones out of order, an unregistered native one and an unknown one.
		constexpr std::string_view NativeSceneText = R"({
	"Format": "Scene",
	"Version": 1,
	"Name": "Native",
	"Seed": 0,
	"ComponentVersions": {
		"Transform": 1,
		"Zeta": 2,
		"Native.TestSpinner": 1,
		"Native.Ghost": 1,
		"Native.TestMover": 1
	},
	"Entities": [
		{
			"ID": "5d1c9a7e33b04f12",
			"Name": "Thing",
			"Parent": null,
			"Active": true,
			"Tags": [],
			"Components": {
				"Transform": {
					"Translation": [0, 0, 0],
					"Rotation": [0, 0, 0, 1],
					"Scale": [1, 1, 1]
				},
				"Zeta": {
					"A": 1
				},
				"Native.TestSpinner": {
					"DegreesPerSecond": 10
				},
				"Native.Ghost": {
					"B": 2
				},
				"Native.TestMover": {
					"Speed": 3
				}
			}
		}
	]
}
)";

		constexpr UUID NativeSceneEntity{ 0x5d1c9a7e33b04f12ull };

		[[nodiscard]] Ref<const NativeModuleTypes> RequireTestTypes(NativeModuleEntryPoint entry)
		{
			Result<Ref<const NativeModuleTypes>> types = BuildNativeModuleTypes(entry, nullptr);
			REQUIRE_MESSAGE(types.has_value(), types.error().ToString());
			return std::move(*types);
		}

		[[nodiscard]] Json ParseJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			return std::move(*json);
		}

		[[nodiscard]] std::vector<std::string> GetPreservedNames(ConstEntity entity)
		{
			std::vector<std::string> names;
			if (const UnknownComponentsComponent* preserved = entity.TryGetComponent<UnknownComponentsComponent>(); preserved != nullptr)
			{
				for (const UnknownComponentData& data : preserved->Components)
					names.push_back(data.Name);
			}
			return names;
		}

	}

	TEST_SUITE("Native")
	{
		// The serializer's native-name rule (Docs/Decisions/0030-m19-contract.md decision 27), which the contract implements in
		// UnknownComponentsComponent.h, is a pure function: tested live.
		TEST_CASE("NativeComponents: a native component's registry name is the Native. prefix and a name")
		{
			CHECK(IsNativeComponentName("Native.Spinner"));
			CHECK(IsNativeComponentName("Native.A"));
			CHECK_FALSE(IsNativeComponentName("Native."));
			CHECK_FALSE(IsNativeComponentName("Native"));
			CHECK_FALSE(IsNativeComponentName("native.Spinner"));
			CHECK_FALSE(IsNativeComponentName("Spinner"));
			CHECK_FALSE(IsNativeComponentName("Transform"));
		}

		// Decision 27 (N-B): with a module loaded, a scene's native components become canonical and sorted by name ahead of the
		// unknown ones; those the module does not register are kept as written and reported, as errors under --strict; a
		// second pass changes nothing; without a module nothing is rewritten.
		TEST_CASE("NativeComponents: a scene's native components are made canonical and sorted, and unregistered ones kept as written")
		{
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			Test::SceneTestFixture setup;
			LoadReport loadReport;
			const Status loaded = SceneSerializer::LoadFromString(setup.GetScene(), NativeSceneText, LoadOptions{}, loadReport);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const Entity thing = setup.GetScene().FindEntityByID(NativeSceneEntity);
			REQUIRE(thing.IsValid());

			const Result<NativeSceneReport> report = CanonicalizeNativeComponents(setup.GetScene(), types.get(), false);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->Changed);
			CHECK(report->ComponentCount == 3);
			REQUIRE(report->Diagnostics.size() == 1);
			CHECK(report->Diagnostics[0].Code == NativeComponentUnregisteredCode);
			CHECK(report->Diagnostics[0].Severity == DiagnosticSeverity::Warning);
			CHECK(report->Diagnostics[0].Entity == NativeSceneEntity);
			CHECK(report->Diagnostics[0].JsonPointer == "/Components/Native.Ghost");
			CHECK(GetPreservedNames(thing) == std::vector<std::string>{ "Native.Ghost", "Native.TestMover", "Native.TestSpinner", "Zeta" });

			const NativeEntityComponents components = ReadNativeComponents(thing, types.get());
			REQUIRE(components.Registered.size() == 2);
			CHECK(components.Registered[0].first == "Native.TestMover");
			CHECK(components.Registered[0].second.Get() == ParseJson(R"({"Speed": 3, "Follow": null})"));
			CHECK(components.Registered[1].first == "Native.TestSpinner");
			CHECK(components.Registered[1].second.Get() == ParseJson(R"({"DegreesPerSecond": 10, "Axis": [0, 1, 0], "Turns": 0})"));
			REQUIRE(components.Unregistered.size() == 1);
			CHECK(components.Unregistered[0].first == "Native.Ghost");
			CHECK(components.Unregistered[0].second.Get() == ParseJson(R"({"B": 2})"));

			// Canonical already: nothing changes, and the saved scene loads back byte-identically.
			const Result<std::string> saved = SceneSerializer::SaveToString(setup.GetScene());
			REQUIRE(saved.has_value());
			const Result<NativeSceneReport> again = CanonicalizeNativeComponents(setup.GetScene(), types.get(), false);
			REQUIRE(again.has_value());
			CHECK_FALSE(again->Changed);
			CHECK(SceneSerializer::SaveToString(setup.GetScene()).value_or(std::string()) == *saved);

			// --strict: the unregistered component is an error, and the scene is left as it was.
			const Result<NativeSceneReport> strict = CanonicalizeNativeComponents(setup.GetScene(), types.get(), true);
			REQUIRE(strict.has_value());
			REQUIRE(strict->Diagnostics.size() == 1);
			CHECK(strict->Diagnostics[0].Severity == DiagnosticSeverity::Error);

			// Without a module every native component is unregistered and nothing is rewritten.
			Test::SceneTestFixture bare;
			LoadReport bareReport;
			REQUIRE(SceneSerializer::LoadFromString(bare.GetScene(), NativeSceneText, LoadOptions{}, bareReport).has_value());
			const Result<NativeSceneReport> unloaded = CanonicalizeNativeComponents(bare.GetScene(), nullptr, false);
			REQUIRE(unloaded.has_value());
			CHECK_FALSE(unloaded->Changed);
			CHECK(unloaded->ComponentCount == 3);
			CHECK(unloaded->Diagnostics.size() == 3);
			CHECK(GetPreservedNames(bare.GetScene().FindEntityByID(NativeSceneEntity))
				== std::vector<std::string>{ "Zeta", "Native.TestSpinner", "Native.Ghost", "Native.TestMover" });
		}

		// The merge with M19 (Docs/Decisions/0031-m20-contract.md, "Merge with M19"): CheckNativeComponents reports what
		// CanonicalizeNativeComponents would, Changed included, and leaves the scene as it is, so the editor checks every open
		// scene before a module load migrates any of them; a value its type rejects is an error and nothing would change.
		TEST_CASE("NativeComponents: a check reports what canonicalizing would and leaves the scene as it is")
		{
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			Test::SceneTestFixture setup;
			LoadReport loadReport;
			REQUIRE(SceneSerializer::LoadFromString(setup.GetScene(), NativeSceneText, LoadOptions{}, loadReport).has_value());
			const Result<std::string> before = SceneSerializer::SaveToString(setup.GetScene());
			REQUIRE(before.has_value());

			const NativeSceneReport checked = CheckNativeComponents(setup.GetScene(), types.get(), false);
			CHECK(checked.Changed);
			CHECK(checked.ComponentCount == 3);
			REQUIRE(checked.Diagnostics.size() == 1);
			CHECK(checked.Diagnostics[0].Code == NativeComponentUnregisteredCode);
			CHECK(SceneSerializer::SaveToString(setup.GetScene()).value_or(std::string()) == *before);

			const Result<NativeSceneReport> canonicalized = CanonicalizeNativeComponents(setup.GetScene(), types.get(), false);
			REQUIRE(canonicalized.has_value());
			CHECK(canonicalized->Changed == checked.Changed);
			CHECK(canonicalized->ComponentCount == checked.ComponentCount);
			CHECK(canonicalized->Diagnostics.size() == checked.Diagnostics.size());
			CHECK_FALSE(CheckNativeComponents(setup.GetScene(), types.get(), false).Changed);

			// A rejected value: an error, and nothing would change.
			Entity rejected = setup.GetScene().CreateEntity("Rejected");
			rejected.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent{ {
				UnknownComponentData{ .Name = "Native.TestMover", .Version = 1, .Data = VariantValue(ParseJson(R"({"Speed": -3})")) },
			} });
			const NativeSceneReport refused = CheckNativeComponents(setup.GetScene(), types.get(), false);
			CHECK_FALSE(refused.Changed);
			CHECK(std::ranges::any_of(refused.Diagnostics, [](const LoadDiagnostic& diagnostic)
			{
				return diagnostic.Code == NativeComponentInvalidCode && diagnostic.Severity == DiagnosticSeverity::Error;
			}));
		}

		// Decision 27 (N-B; native.setComponents, decision 32): a patch adds a component from its defaults, merges into one the
		// entity holds and removes one with null, all or nothing.
		TEST_CASE("NativeComponents: a patch adds, changes and removes native components, all or nothing")
		{
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			Test::SceneTestFixture setup;
			Entity thing = setup.GetScene().CreateEntity("Thing");

			const Result<bool> added = PatchNativeComponents(thing, *types,
				ParseJson(R"({"Native.TestSpinner": {"DegreesPerSecond": 20}, "Native.TestMover": {}})"), "/components");
			REQUIRE_MESSAGE(added.has_value(), added.error().ToString());
			CHECK(*added);
			NativeEntityComponents components = ReadNativeComponents(thing, types.get());
			REQUIRE(components.Registered.size() == 2);
			CHECK(components.Registered[0].first == "Native.TestMover");
			CHECK(components.Registered[0].second.Get() == ParseJson(R"({"Speed": 2, "Follow": null})"));
			CHECK(components.Registered[1].second.Get() == ParseJson(R"({"DegreesPerSecond": 20, "Axis": [0, 1, 0], "Turns": 0})"));

			const Result<bool> same = PatchNativeComponents(thing, *types, ParseJson(R"({"Native.TestSpinner": {"DegreesPerSecond": 20}})"), "/components");
			REQUIRE(same.has_value());
			CHECK_FALSE(*same);

			// A value its type rejects leaves every component as it was.
			const Result<bool> rejected = PatchNativeComponents(thing, *types,
				ParseJson(R"({"Native.TestMover": null, "Native.TestSpinner": {"DegreesPerSecond": 9999}})"), "/components");
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK(ReadNativeComponents(thing, types.get()).Registered.size() == 2);

			const Result<bool> unknown = PatchNativeComponents(thing, *types, ParseJson(R"({"Native.Nope": {}})"), "/components");
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(PatchNativeComponents(thing, *types, ParseJson("[]"), "/components").has_value());

			const Result<bool> removed = PatchNativeComponents(thing, *types, ParseJson(R"({"Native.TestMover": null})"), "/components");
			REQUIRE(removed.has_value());
			CHECK(*removed);
			components = ReadNativeComponents(thing, types.get());
			REQUIRE(components.Registered.size() == 1);
			CHECK(components.Registered[0].first == "Native.TestSpinner");
		}

		// Decision 27 (N-B): a value its type rejects is an error naming the field; nothing of the scene changes, the other
		// components included.
		TEST_CASE("NativeComponents: a native component whose value its type rejects is an error, and the scene is left as it was")
		{
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			Test::SceneTestFixture setup;
			Entity thing = setup.GetScene().CreateEntity("Thing");
			thing.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent{ {
				UnknownComponentData{ .Name = "Native.TestSpinner", .Version = 1, .Data = VariantValue(ParseJson(R"({"DegreesPerSecond": 5})")) },
				UnknownComponentData{ .Name = "Native.TestMover", .Version = 1, .Data = VariantValue(ParseJson(R"({"Speed": -3})")) },
			} });

			const Result<NativeSceneReport> report = CanonicalizeNativeComponents(setup.GetScene(), types.get(), false);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK_FALSE(report->Changed);
			REQUIRE(report->Diagnostics.size() == 1);
			CHECK(report->Diagnostics[0].Code == NativeComponentInvalidCode);
			CHECK(report->Diagnostics[0].Severity == DiagnosticSeverity::Error);
			CHECK(report->Diagnostics[0].Entity == thing.GetUUID());
			CHECK(report->Diagnostics[0].JsonPointer == "/Components/Native.TestMover/Speed");
			CHECK(GetPreservedNames(thing) == std::vector<std::string>{ "Native.TestSpinner", "Native.TestMover" });
			const NativeEntityComponents components = ReadNativeComponents(thing, types.get());
			REQUIRE(components.Registered.size() == 2);
			CHECK(components.Registered[0].second.Get() == ParseJson(R"({"DegreesPerSecond": 5})"));
		}

		// Decision 27 (N-B): a member of an older layout is migrated with a warning note; under --strict the note is an error
		// and nothing changes.
		TEST_CASE("NativeComponents: a migrated field is a warning, and an error that changes nothing under --strict")
		{
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModuleV2());
			for (const bool strictUnknowns : { false, true })
			{
				CAPTURE(strictUnknowns);
				Test::SceneTestFixture setup;
				Entity thing = setup.GetScene().CreateEntity("Thing");
				thing.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent{ {
					UnknownComponentData{ .Name = "Native.TestMover", .Version = 1, .Data = VariantValue(ParseJson(R"({"Speed": 3, "Follow": null})")) },
				} });
				const Result<NativeSceneReport> report = CanonicalizeNativeComponents(setup.GetScene(), types.get(), strictUnknowns);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
				REQUIRE(report->Diagnostics.size() == 1);
				CHECK(report->Diagnostics[0].Code == NativeFieldDroppedCode);
				CHECK(report->Diagnostics[0].JsonPointer == "/Components/Native.TestMover/Follow");
				CHECK(report->Diagnostics[0].Severity == (strictUnknowns ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning));
				CHECK(report->Changed == !strictUnknowns);
				const NativeEntityComponents components = ReadNativeComponents(thing, types.get());
				REQUIRE(components.Registered.size() == 1);
				CHECK(components.Registered[0].second.Get() == ParseJson(strictUnknowns ? R"({"Speed": 3, "Follow": null})" : R"({"Speed": 3})"));
			}
		}

		// Decision 32 (N-B's half): a patch is a request, so a member no field is named after is refused with the closest
		// field's name, not dropped.
		TEST_CASE("NativeComponents: a patch member that names no field is refused with a suggestion")
		{
			const Ref<const NativeModuleTypes> types = RequireTestTypes(Test::GetNativeTestModule());
			Test::SceneTestFixture setup;
			Entity thing = setup.GetScene().CreateEntity("Thing");
			const Result<bool> refused = PatchNativeComponents(thing, *types, ParseJson(R"({"Native.TestSpinner": {"DegreesPerSecnd": 20}})"), "/components");
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			REQUIRE(refused.error().GetIssues().size() == 1);
			CHECK(refused.error().GetIssues()[0].JsonPointer == "/components/Native.TestSpinner/DegreesPerSecnd");
			CHECK(refused.error().GetIssues()[0].Suggestions == std::vector<std::string>{ "DegreesPerSecond" });
			CHECK(ReadNativeComponents(thing, types.get()).Registered.empty());

			const Result<bool> unknown = PatchNativeComponents(thing, *types, ParseJson(R"({"Native.TestSpiner": {}})"), "/components");
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			REQUIRE(unknown.error().GetIssues().size() == 1);
			REQUIRE_FALSE(unknown.error().GetIssues()[0].Suggestions.empty());
			CHECK(unknown.error().GetIssues()[0].Suggestions.front() == "Native.TestSpinner");
		}
	}

}
