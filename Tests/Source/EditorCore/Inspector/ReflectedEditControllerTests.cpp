#include "TestsPCH.h"
#include "EditorCore/Inspector/ReflectedEditController.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentRegistration.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"
#include "Support/AssetTestFixture.h"
#include "Support/EditorTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <map>

namespace Engine {

	namespace Test {

		struct InspectorFixtureComponent
		{
			float Limit = 1.0f;
			std::vector<float> Samples{ 1.0f, 2.0f };
			std::map<std::string, float> Weights{ { "First", 1.0f } };
			std::map<std::string, VariantValue> Fields{ { "Limit", VariantValue(Json(1.0f)) } };
			UUID Target{};
			UUID FrozenTarget{};
			std::map<std::string, UUID> Targets{ { "Follow", UUID{} } };
			TypedAssetHandle<AssetType::Material> Material{};
		};

	}

	static Result<const FieldInfo*> ResolveInspectorFixtureField(const ResolveContext& context)
	{
		if (context.OwnerType == nullptr || context.OwnerType->GetName() != "InspectorFixture" || context.Key != "Limit")
			return MakeError(ErrorCode::NotFound, "unknown inspector fixture owner or key");
		if (context.Owner == nullptr && context.OwnerJson == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "inspector fixture resolution requires its real owner");
		return context.OwnerType->FindField("Limit");
	}

	static void RegisterInspectorFixtureTypes(TypeRegistry& types)
	{
		RegisterEditorMethodTypes(types);
		RegisterComponent<Test::InspectorFixtureComponent>(types, "InspectorFixture", "Reflected inspector test values.")
			.Category("Tests")
			.Version(1)
			.Field("Limit", &Test::InspectorFixtureComponent::Limit, "Positive limited value.", { .Min = 0.0, .Max = 10.0 })
			.Field("Samples", &Test::InspectorFixtureComponent::Samples, "Positive samples.", { .Min = 0.0 })
			.Field("Weights", &Test::InspectorFixtureComponent::Weights, "Positive named weights.", { .Min = 0.0 })
			.VariantField("Fields", &Test::InspectorFixtureComponent::Fields, "Owner-resolved variants.", &ResolveInspectorFixtureField)
			.Field("Target", &Test::InspectorFixtureComponent::Target, "Entity picked by UUID.")
			.Field("FrozenTarget", &Test::InspectorFixtureComponent::FrozenTarget, "Read-only entity reference.", { .ReadOnly = true })
			.Field("Targets", &Test::InspectorFixtureComponent::Targets, "Nested entity references.")
			.Field("Material", &Test::InspectorFixtureComponent::Material, "Material picked by asset handle.")
			.VirtualField<float>("GuardedLimit", "Owner-dependent virtual setter.", [](const FieldContext& context)
		{
			return Value::FromFloat(static_cast<const Test::InspectorFixtureComponent*>(context.Object)->Limit);
		}, [](const FieldContext& context, const Value& value) -> Status
		{
			if (context.Owner == nullptr || static_cast<const Test::InspectorFixtureComponent*>(context.Object)->Limit >= 5.0f)
				return MakeError(ErrorCode::Validation, "guarded owner refuses this edit");
			context.Owner->Patch<Test::InspectorFixtureComponent>([candidate = value.AsFloat()](Test::InspectorFixtureComponent& component)
			{
				component.Limit = candidate;
			});
			return {};
		}, { .Min = 0.0, .Max = 10.0 })
			.Validate([](const Test::InspectorFixtureComponent& component, ValidationContext& validation)
		{
			if (!component.Samples.empty() && component.Samples.front() > component.Limit)
				validation.Error("Samples", "first sample exceeds Limit");
			for (const float sample : component.Samples)
			{
				if (sample < 0.0f)
					validation.Error("Samples", "samples must be nonnegative");
			}
			for (const auto& [key, weight] : component.Weights)
			{
				if (weight < 0.0f)
					validation.Error("Weights", "weights must be nonnegative: " + key);
			}
		}).Generate([](Test::InspectorFixtureComponent& component, Random& /*random*/)
		{
			if (!component.Samples.empty() && component.Samples.front() > component.Limit)
				component.Samples.front() = component.Limit;
			for (float& sample : component.Samples)
				sample = std::max(0.0f, sample);
			for (auto& entry : component.Weights)
				entry.second = std::max(0.0f, entry.second);
		});
	}

	static UUID AddInspectorFixture(Test::EditorTestFixture& fixture)
	{
		fixture.CreateAndOpenProject();
		fixture.CreateAndOpenScene();
		const Entity entity = fixture.GetEditor().GetScene().CreateEntity("Inspectable");
		REQUIRE(ComponentAccess::AddComponent(entity, "InspectorFixture", nullptr));
		return entity.GetUUID();
	}

	static Value ReadInspectorFixture(EditorContext& editor, UUID id, std::string_view field)
	{
		auto value = ComponentAccess::GetFieldValue(editor.GetScene().FindEntityByID(id), "InspectorFixture", field);
		REQUIRE(value);
		return *value;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ReflectedEditController: a continuous edit commits once on release")
		{
			Test::EditorTestFixture fixture("InspectorGesture", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Limit" }));
			const uint64_t revision = editor.GetRevision();
			for (float candidate : { 2.0f, 3.0f, 4.0f })
				REQUIRE(edits.Preview(Value::FromFloat(candidate)));
			CHECK(editor.GetRevision() == revision);
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 1.0f);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			auto committed = edits.Commit();
			REQUIRE(committed);
			CHECK(*committed != 0);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 4.0f);
			REQUIRE(editor.GetHistory().Undo(editor));
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 1.0f);
			REQUIRE(editor.GetHistory().Redo(editor));
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 4.0f);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Limit" }));
			REQUIRE(edits.Preview(Value::FromFloat(4.0f)));
			auto unchanged = edits.Commit();
			REQUIRE(unchanged);
			CHECK(*unchanged == 0);
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Limit" }));
			REQUIRE(edits.Preview(Value::FromFloat(6.0f)));
			edits.Cancel();
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 4.0f);
			CHECK_FALSE(edits.GetPreview());
		}

		TEST_CASE("ReflectedEditController: committing records a user command and restores ambient attribution")
		{
			Test::EditorTestFixture fixture("InspectorUserOrigin", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Limit" }));
			REQUIRE(edits.Preview(Value::FromFloat(3.0f)));
			editor.SetWriteAttribution(WriteAttribution{ .Method = "entity.update", .Client = "test-agent" });
			REQUIRE(edits.Commit());
			const auto history = editor.GetHistory().GetEntries(1);
			REQUIRE(history.size() == 1);
			CHECK(history.front().Origin == CommandOrigin::User);
			CHECK(editor.GetCommandOrigin() == CommandOrigin::Agent);
			CHECK(editor.GetWriteAttribution().Client == "test-agent");
			editor.SetWriteAttribution(std::nullopt);
		}

		TEST_CASE("ReflectedEditController: array and map edits validate before one undo step")
		{
			Test::EditorTestFixture fixture("InspectorContainers", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Samples[1]" }));
			CHECK_FALSE(edits.Preview(Value::FromFloat(-1.0f)));
			REQUIRE(edits.Preview(Value::FromFloat(3.0f)));
			REQUIRE(edits.Commit());
			CHECK(ReadInspectorFixture(editor, id, "Samples").GetElements()[1].AsFloat() == 3.0f);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Weights" }));
			CHECK_FALSE(edits.Preview(Value::FromMap({ "Negative" }, { Value::FromFloat(-1.0f) })));
			REQUIRE(edits.Preview(Value::FromMap({ "Renamed", "Second" }, { Value::FromFloat(2.0f), Value::FromFloat(3.0f) })));
			REQUIRE(edits.Commit());
			CHECK(editor.GetHistory().GetUndoCount() == 2);
			REQUIRE(editor.GetHistory().Undo(editor));
			CHECK(ReadInspectorFixture(editor, id, "Weights").FindMember("First") != nullptr);
		}

		TEST_CASE("ReflectedEditController: duplicate map keys are rejected")
		{
			Test::EditorTestFixture fixture("InspectorDuplicateKeys", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			ReflectedEditController edits(fixture.GetEditor());
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Weights" }));
			const auto before = edits.GetPreview();
			REQUIRE(before);
			// Value::FromMap requires unique sorted keys; the strict JSON boundary rejects duplicates before conversion.
			const auto duplicate = JsonReader::Parse(R"({"First":1,"First":2})");
			REQUIRE_FALSE(duplicate);
			CHECK(duplicate.error().GetCode() == ErrorCode::Parse);
			CHECK(edits.GetPreview().value() == *before);
			CHECK(fixture.GetEditor().GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("ReflectedEditController: resolved variants use the owner and map key")
		{
			Test::EditorTestFixture fixture("InspectorResolved", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Fields[Limit]" }));
			CHECK_FALSE(edits.Preview(Value::FromVariant(VariantValue(Json(11.0f)))));
			CHECK_FALSE(edits.Preview(Value::FromVariant(VariantValue(Json("wrong")))));
			REQUIRE(edits.Preview(Value::FromVariant(VariantValue(Json(3.0f)))));
			REQUIRE(edits.Commit());
			CHECK(ReadInspectorFixture(editor, id, "Fields").FindMember("Limit")->AsVariant().Get() == Json(3.0f));
			REQUIRE(editor.GetHistory().Undo(editor));
			CHECK(ReadInspectorFixture(editor, id, "Fields").FindMember("Limit")->AsVariant().Get() == Json(1.0f));
		}

		TEST_CASE("ReflectedEditController: unresolved variants preserve raw data read-only")
		{
			Test::EditorTestFixture fixture("InspectorUnresolved", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			const Value raw = Value::FromMap({ "Unknown" }, { Value::FromVariant(VariantValue(Json{ { "keep", 42 } })) });
			// Simulate a scene load that preserved an unresolved field; authored setters deliberately reject it.
			editor.GetScene().FindEntityByID(id).Patch<Test::InspectorFixtureComponent>([](Test::InspectorFixtureComponent& component)
			{
				component.Fields = { { "Unknown", VariantValue(Json{ { "keep", 42 } }) } };
			});
			ReflectedEditController edits(editor);
			const auto result = edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Fields[Unknown]" });
			REQUIRE_FALSE(result);
			CHECK(result.error().GetCode() == ErrorCode::Unsupported);
			CHECK_FALSE(edits.IsEditing());
			CHECK(ReadInspectorFixture(editor, id, "Fields") == raw);
		}

		TEST_CASE("ReflectedEditController: entity asset and import fields use their existing commands")
		{
			Test::AutomationFixture fixture("InspectorAssetCommands");
			auto& editor = fixture.GetEditor();
			auto created = fixture.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Panel.material" } });
			REQUIRE(created);
			auto id = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(id);
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Kind = InspectorTargetKind::AssetProperties, .Asset = AssetHandle(*id), .FieldPath = "Roughness" }));
			REQUIRE(edits.Preview(Value::FromFloat(0.25f)));
			const size_t before = editor.GetHistory().GetUndoCount();
			REQUIRE(edits.Commit());
			CHECK(editor.GetHistory().GetUndoCount() == before + 1);
			auto properties = fixture.Call("asset.getProperties", Json{ { "asset", id->ToString() } });
			REQUIRE(properties);
			CHECK((*properties)["values"]["Roughness"] == Json(0.25f));
			REQUIRE(editor.GetHistory().Undo(editor));
			const VfsPath png = VfsPath::Create("project", "Assets/Inspector.png").value();
			REQUIRE(editor.WriteProjectFile(png, Test::MakeTestPng(8, 8)));
			REQUIRE(fixture.Call("project.refreshAssets", Json::object()));
			auto settings = fixture.Call("asset.getImportSettings", Json{ { "asset", "Assets/Inspector.png" } });
			REQUIRE(settings);
			auto textureId = JsonReader((*settings)["asset"]["id"]).ReadUUID();
			REQUIRE(textureId);
			REQUIRE(edits.Begin({ .Kind = InspectorTargetKind::AssetImportSettings, .Asset = AssetHandle(*textureId), .FieldPath = "GenerateMips" }));
			REQUIRE(edits.Preview(Value::FromBool(false)));
			const size_t beforeImport = editor.GetHistory().GetUndoCount();
			REQUIRE(edits.Commit());
			CHECK(editor.GetHistory().GetUndoCount() == beforeImport + 1);
			settings = fixture.Call("asset.getImportSettings", Json{ { "asset", textureId->ToString() } });
			REQUIRE(settings);
			CHECK((*settings)["settings"]["GenerateMips"] == Json(false));
			REQUIRE(editor.GetHistory().Undo(editor));
			REQUIRE(edits.Begin({ .Kind = InspectorTargetKind::ProjectSettings, .FieldPath = "Simulation.FixedHz" }));
			REQUIRE(edits.Preview(Value::FromUInt32(120)));
			REQUIRE(edits.Commit());
			CHECK(editor.GetProject().GetSettings().Simulation.FixedHz == 120);
		}

		TEST_CASE("ReflectedEditController: concurrent revisions and asset versions reject stale edits")
		{
			Test::EditorTestFixture fixture("InspectorConflict", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Limit" }));
			REQUIRE(edits.Preview(Value::FromFloat(3.0f)));
			REQUIRE(ComponentAccess::SetFieldValue(editor.GetScene().FindEntityByID(id), "InspectorFixture", "Limit", Value::FromFloat(2.0f)));
			const auto conflict = edits.Commit();
			REQUIRE_FALSE(conflict);
			CHECK(conflict.error().GetCode() == ErrorCode::Conflict);
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 2.0f);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("ReflectedEditController: an asset replacement rejects the captured version and preserves agent edits")
		{
			Test::AutomationFixture fixture("InspectorAssetConflict");
			auto& editor = fixture.GetEditor();
			auto created = fixture.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Concurrent.material" } });
			REQUIRE(created);
			auto id = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(id);
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Kind = InspectorTargetKind::AssetProperties, .Asset = AssetHandle(*id), .FieldPath = "Roughness" }));
			REQUIRE(edits.Preview(Value::FromFloat(0.25f)));
			REQUIRE(fixture.Call("asset.setProperties", Json{ { "asset", id->ToString() }, { "values", Json{ { "Roughness", 0.75f } } } }));
			const size_t undoCount = editor.GetHistory().GetUndoCount();
			auto committed = edits.Commit();
			REQUIRE_FALSE(committed);
			CHECK(committed.error().GetCode() == ErrorCode::Conflict);
			CHECK(editor.GetHistory().GetUndoCount() == undoCount);
			auto properties = fixture.Call("asset.getProperties", Json{ { "asset", id->ToString() } });
			REQUIRE(properties);
			CHECK((*properties)["values"]["Roughness"] == Json(0.75f));
		}

		TEST_CASE("ReflectedEditController: runtime edits are transient and a replaced session rejects the gesture")
		{
			Test::EditorTestFixture fixture("InspectorRuntime", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			ReflectedEditController edits(editor);
			const InspectorEditTarget target{ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Limit", .Target = SceneTarget::Play };
			REQUIRE(edits.Begin(target));
			REQUIRE(edits.Preview(Value::FromFloat(3.0f)));
			auto committed = edits.Commit();
			REQUIRE(committed);
			CHECK(*committed == 0);
			auto runtime = ComponentAccess::GetFieldValue(editor.GetPlay().GetSession()->GetScene().FindEntityByID(id), "InspectorFixture", "Limit");
			REQUIRE(runtime);
			CHECK(runtime->AsFloat() == 3.0f);
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 1.0f);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
			REQUIRE(edits.Begin(target));
			REQUIRE(edits.Preview(Value::FromFloat(4.0f)));
			REQUIRE(editor.GetPlay().Stop());
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			auto stale = edits.Commit();
			REQUIRE_FALSE(stale);
			CHECK(stale.error().GetCode() == ErrorCode::Conflict);
			CHECK(ReadInspectorFixture(editor, id, "Limit").AsFloat() == 1.0f);
		}

		TEST_CASE("ReflectedEditController: multi-edit validates every target before any mutation")
		{
			Test::EditorTestFixture fixture("InspectorAtomic", {}, &RegisterInspectorFixtureTypes);
			const UUID first = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			const Entity secondEntity = editor.GetScene().CreateEntity("Second");
			REQUIRE(ComponentAccess::AddComponent(secondEntity, "InspectorFixture", nullptr));
			const UUID second = secondEntity.GetUUID();
			REQUIRE(ComponentAccess::SetFieldValue(secondEntity, "InspectorFixture", "Limit", Value::FromFloat(5.0f)));
			REQUIRE(ComponentAccess::SetFieldValue(secondEntity, "InspectorFixture", "Samples", Value::FromArray({ Value::FromFloat(4.0f) })));
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { first, second }, .Component = "InspectorFixture", .FieldPath = "Limit" }));
			CHECK_FALSE(edits.Preview(Value::FromFloat(2.0f)));
			CHECK(ReadInspectorFixture(editor, first, "Limit").AsFloat() == 1.0f);
			CHECK(ReadInspectorFixture(editor, second, "Limit").AsFloat() == 5.0f);
			REQUIRE(edits.Preview(Value::FromFloat(7.0f)));
			REQUIRE(edits.Commit());
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			REQUIRE(editor.GetHistory().Undo(editor));
			CHECK(ReadInspectorFixture(editor, first, "Limit").AsFloat() == 1.0f);
			CHECK(ReadInspectorFixture(editor, second, "Limit").AsFloat() == 5.0f);
		}

		TEST_CASE("ReflectedEditController: a picked entity replaces mixed references atomically and undo restores each value")
		{
			Test::EditorTestFixture fixture("InspectorReferenceMulti", {}, &RegisterInspectorFixtureTypes);
			const UUID first = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			const Entity second = editor.GetScene().CreateEntity("Second");
			REQUIRE(ComponentAccess::AddComponent(second, "InspectorFixture", nullptr));
			REQUIRE(ComponentAccess::SetFieldValue(second, "InspectorFixture", "Target", Value::FromEntityRef(first)));
			const UUID picked = editor.GetScene().CreateEntity("Picked##Target").GetUUID();
			ReflectedEditController edits(editor);
			const auto readOnly = edits.Begin({ .Entities = { first }, .Component = "InspectorFixture", .FieldPath = "FrozenTarget" });
			REQUIRE_FALSE(readOnly);
			CHECK(readOnly.error().GetCode() == ErrorCode::Unsupported);
			REQUIRE(edits.Begin({ .Entities = { first, second.GetUUID() }, .Component = "InspectorFixture", .FieldPath = "Target" }));
			const uint64_t revision = editor.GetRevision();
			CHECK_FALSE(edits.Preview(Value::FromAssetRef(picked)));
			REQUIRE(edits.Preview(Value::FromEntityRef(picked)));
			CHECK(editor.GetRevision() == revision);
			CHECK(ReadInspectorFixture(editor, first, "Target").AsUUID() == UUID{});
			CHECK(ReadInspectorFixture(editor, second.GetUUID(), "Target").AsUUID() == first);
			REQUIRE(edits.Commit());
			CHECK(editor.GetHistory().GetUndoCount() == 1);
			CHECK(ReadInspectorFixture(editor, first, "Target").AsUUID() == picked);
			CHECK(ReadInspectorFixture(editor, second.GetUUID(), "Target").AsUUID() == picked);
			REQUIRE(editor.GetHistory().Undo(editor));
			CHECK(ReadInspectorFixture(editor, first, "Target").AsUUID() == UUID{});
			CHECK(ReadInspectorFixture(editor, second.GetUUID(), "Target").AsUUID() == first);
			REQUIRE(edits.Begin({ .Entities = { first }, .Component = "InspectorFixture", .FieldPath = "Targets[Follow]" }));
			REQUIRE(edits.Preview(Value::FromEntityRef(picked)));
			edits.Cancel();
			CHECK(ReadInspectorFixture(editor, first, "Targets").FindMember("Follow")->AsUUID() == UUID{});
		}

		TEST_CASE("ReflectedEditController: a runtime-only picked reference stays in the addressed play scene")
		{
			Test::EditorTestFixture fixture("InspectorReferencePlay", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetPlay().Start(PlayStartOptions{ .Lockstep = true }));
			auto picked = editor.GetPlay().GetSession()->CreateEntity("Runtime target");
			REQUIRE(picked);
			CHECK_FALSE(editor.GetScene().FindEntityByID(picked->GetUUID()).IsValid());
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Targets[Follow]", .Target = SceneTarget::Play }));
			REQUIRE(edits.Preview(Value::FromEntityRef(picked->GetUUID())));
			auto committed = edits.Commit();
			REQUIRE(committed);
			CHECK(*committed == 0);
			auto runtime = ComponentAccess::GetFieldValue(editor.GetPlay().GetSession()->GetScene().FindEntityByID(id), "InspectorFixture", "Targets");
			REQUIRE(runtime);
			CHECK(runtime->FindMember("Follow")->AsUUID() == picked->GetUUID());
			CHECK(ReadInspectorFixture(editor, id, "Targets").FindMember("Follow")->AsUUID() == UUID{});
			REQUIRE(editor.GetPlay().Stop());
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}

		TEST_CASE("ReflectedEditController: a picked material commits its stable handle with undo and clear")
		{
			Test::EditorTestFixture fixture("InspectorReferenceAsset", {}, &RegisterInspectorFixtureTypes);
			const UUID id = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			Test::AutomationTestClient client(editor);
			auto created = client.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Picked.material" } });
			REQUIRE(created);
			auto handle = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(handle);
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Material" }));
			REQUIRE(edits.Preview(Value::FromAssetRef(*handle)));
			CHECK(ReadInspectorFixture(editor, id, "Material").AsUUID() == UUID{});
			REQUIRE(edits.Commit());
			CHECK(ReadInspectorFixture(editor, id, "Material").AsUUID() == *handle);
			REQUIRE(editor.GetHistory().Undo(editor));
			CHECK(ReadInspectorFixture(editor, id, "Material").AsUUID() == UUID{});
			REQUIRE(editor.GetHistory().Redo(editor));
			CHECK(ReadInspectorFixture(editor, id, "Material").AsUUID() == *handle);
			REQUIRE(edits.Begin({ .Entities = { id }, .Component = "InspectorFixture", .FieldPath = "Material" }));
			REQUIRE(edits.Preview(Value::FromAssetRef({})));
			REQUIRE(edits.Commit());
			CHECK(ReadInspectorFixture(editor, id, "Material").AsUUID() == UUID{});
			REQUIRE(editor.GetHistory().Undo(editor));
			CHECK(ReadInspectorFixture(editor, id, "Material").AsUUID() == *handle);
		}

		TEST_CASE("ReflectedEditController: virtual multi-edit validates owner-dependent setters before live patches")
		{
			Test::EditorTestFixture fixture("InspectorVirtualAtomic", {}, &RegisterInspectorFixtureTypes);
			const UUID first = AddInspectorFixture(fixture);
			auto& editor = fixture.GetEditor();
			const Entity second = editor.GetScene().CreateEntity("Guarded");
			REQUIRE(ComponentAccess::AddComponent(second, "InspectorFixture", nullptr));
			REQUIRE(ComponentAccess::SetFieldValue(second, "InspectorFixture", "Limit", Value::FromFloat(5.0f)));
			ReflectedEditController edits(editor);
			REQUIRE(edits.Begin({ .Entities = { first, second.GetUUID() }, .Component = "InspectorFixture", .FieldPath = "GuardedLimit" }));
			const uint64_t revision = editor.GetScene().GetRevision();
			auto preview = edits.Preview(Value::FromFloat(3.0f));
			REQUIRE_FALSE(preview);
			CHECK(preview.error().GetCode() == ErrorCode::Validation);
			CHECK(editor.GetScene().GetRevision() == revision);
			CHECK(ReadInspectorFixture(editor, first, "Limit").AsFloat() == 1.0f);
			CHECK(ReadInspectorFixture(editor, second.GetUUID(), "Limit").AsFloat() == 5.0f);
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}
	}

}
