#include "TestsPCH.h"
#include "Editor/Drawers/ReflectedDrawers.h"

#include "Editor/PanelInteractionFixture.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"

#include <array>
#include <cstring>
#include <map>

namespace Engine {

	namespace Test {

		struct DrawerInteractionValues
		{
			bool Enabled = true;
			std::map<std::string, float> Weights{};
			VariantValue Choice{ Json(true) };
		};

	}

	static Result<const FieldInfo*> ResolveDrawerInteractionChoice(const ResolveContext& context)
	{
		if (context.Owner == nullptr || context.OwnerType == nullptr || context.OwnerType->GetName() != "DrawerInteractionValues")
			return MakeError(ErrorCode::NotFound, "missing drawer interaction owner");
		return context.OwnerType->FindField("Enabled");
	}

	static void RegisterDrawerInteractionTypes(TypeRegistry& types)
	{
		types.Struct<Test::DrawerInteractionValues>("DrawerInteractionValues", "Owned values for real drawer interaction tests.")
			.Field("Enabled", &Test::DrawerInteractionValues::Enabled, "A checkbox used to resolve the variant.")
			.Field("Weights", &Test::DrawerInteractionValues::Weights, "A map of editable named weights.")
			.VariantField("Choice", &Test::DrawerInteractionValues::Choice, "An owner-resolved boolean.", &ResolveDrawerInteractionChoice);
		types.Freeze();
	}

	static Result<ReflectedDrawerResult> DeliverInspectorAssetPayload(Test::PanelInteractionUi& ui, const FieldInfo& field, Value& value,
		const ReflectedDrawerContext& context, std::span<const std::byte> bytes)
	{
		Result<ReflectedDrawerResult> result;
		bool drag = false;
		const auto draw = [&drag, bytes, &field, &value, &context, &result]() -> Status
		{
			if (drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
			{
				ImGui::SetDragDropPayload("ENGINE_ASSET", bytes.data(), bytes.size());
				ImGui::EndDragDropSource();
			}
			ImGui::SetCursorScreenPos(ImVec2(40.0f, 100.0f));
			ImGui::SetNextItemWidth(200.0f);
			result = DrawReflectedValue(field, value, context);
			return {};
		};
		ENGINE_TRY(ui.Frame(draw));
		const float targetY = 108.0f + (context.Mixed ? ImGui::GetTextLineHeightWithSpacing() : 0.0f);
		ImGui::GetIO().AddMousePosEvent(100.0f, targetY);
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		drag = true;
		ENGINE_TRY(ui.Frame(draw));
		ENGINE_TRY(ui.Frame(draw));
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		ENGINE_TRY(ui.Frame(draw));
		return result;
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("ReflectedDrawers: new script array elements use authored defaults recursively")
		{
			TypeRegistry types;
			RegisterDrawerInteractionTypes(types);
			auto number = CreateRef<ScriptFieldSchema>();
			number->Type = FieldType::Float;
			number->DefaultValue = VariantValue(Json(4));
			number->Meta.Min = 3;
			number->Meta.Max = 5;
			auto row = CreateRef<ScriptFieldSchema>();
			row->Type = FieldType::Array;
			row->DefaultValue = VariantValue(Json::array({ 4 }));
			row->Element = number;
			auto script = CreateRef<ScriptData>();
			script->Kind = ScriptKind::Behaviour;
			script->Name = "Rows";
			ScriptFieldSchema rows;
			rows.Name = "Rows";
			rows.Type = FieldType::Array;
			rows.DefaultValue = VariantValue(Json::array());
			rows.Element = row;
			script->Fields.push_back(rows);
			const AssetHandle handle(7);
			const auto schemas = ScriptFieldSchemaSource::Create({ { handle, script } });
			REQUIRE(schemas);
			const auto field = (*schemas)->FindField(handle, "Rows");
			REQUIRE(field);
			Test::PanelInteractionUi ui;
			Value value = Value::FromArray({});
			Result<ReflectedDrawerResult> result;
			ImVec2 add{};
			const auto draw = [&types, &rows, &field, &value, &result, &add]() -> Status
			{
				result = DrawReflectedValue(**field, value, { .Types = types, .Path = "Fields.Rows", .ScriptSchema = &rows });
				add = Test::PanelInteractionUi::LastItemCenter();
				return result ? Status{} : Status(std::unexpected(result.error()));
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, add));
			REQUIRE(value.GetElements().size() == 1);
			REQUIRE(value.GetElements()[0].GetElements().size() == 1);
			CHECK(value.GetElements()[0].GetElements()[0].AsFloat() == 4.0f);
			// Draw the inner array with the exact descriptor/context that recursive descent uses.
			const FieldInfo* inner = (*field)->GetType().GetElementSchema();
			REQUIRE(inner != nullptr);
			Value innerValue = Value::FromArray({});
			const auto drawInner = [&types, &row, inner, &innerValue, &add]() -> Status
			{
				const auto drawn = DrawReflectedValue(*inner, innerValue, { .Types = types, .Path = "Fields.Rows[0]", .ScriptSchema = row.get() });
				add = Test::PanelInteractionUi::LastItemCenter();
				return drawn ? Status{} : Status(std::unexpected(drawn.error()));
			};
			REQUIRE(ui.Frame(drawInner));
			REQUIRE(ui.Click(drawInner, add));
			REQUIRE(innerValue.GetElements().size() == 1);
			CHECK(innerValue.GetElements()[0].AsFloat() == 4.0f);
		}

		TEST_CASE("ReflectedDrawers: map key entry rejects duplicates atomically and sorts successful renames")
		{
			TypeRegistry types;
			RegisterDrawerInteractionTypes(types);
			const auto* field = types.FindStruct<Test::DrawerInteractionValues>()->FindField("Weights");
			for (const std::string renamed : { "Second", "Zulu" })
			{
				INFO(renamed);
				Test::PanelInteractionUi ui;
				Value value = Value::FromMap({ "First", "Second" }, { Value::FromFloat(1.0f), Value::FromFloat(2.0f) });
				const Value original = value;
				Result<ReflectedDrawerResult> result;
				ImVec2 origin{};
				const auto draw = [&types, field, &value, &result, &origin]() -> Status
				{
					origin = ImGui::GetCursorScreenPos();
					result = DrawReflectedValue(*field, value, { .Types = types, .Path = "Weights" });
					return {};
				};
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Click(draw, ImVec2(origin.x + ImGui::GetStyle().IndentSpacing + 40.0f, origin.y + ImGui::GetTextLineHeightWithSpacing() + 8.0f)));
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
				ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
				ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
				REQUIRE(ui.Frame(draw));
				ImGui::GetIO().AddInputCharactersUTF8(renamed.c_str());
				REQUIRE(ui.Frame(draw));
				CHECK(value == original);
				ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
				REQUIRE(ui.Frame(draw));
				if (renamed == "Second")
				{
					REQUIRE_FALSE(result);
					CHECK(result.error().GetCode() == ErrorCode::Validation);
					CHECK(value == original);
				}
				else
				{
					REQUIRE(result);
					CHECK(result->Changed);
					CHECK(result->Committed);
					REQUIRE(value.GetKeys().size() == 2);
					CHECK(value.GetKeys()[0] == "Second");
					CHECK(value.GetKeys()[1] == "Zulu");
					CHECK(value.FindMember("Zulu")->AsFloat() == 1.0f);
				}
			}
		}

		TEST_CASE("ReflectedDrawers: a resolved variant edits through the owner schema and unresolved data stays unchanged")
		{
			TypeRegistry types;
			RegisterDrawerInteractionTypes(types);
			const auto* ownerType = types.FindStruct<Test::DrawerInteractionValues>();
			const auto* field = ownerType->FindField("Choice");
			for (const bool resolved : { true, false })
			{
				Test::DrawerInteractionValues owner;
				Test::PanelInteractionUi ui;
				Value value = Value::FromVariant(owner.Choice);
				Result<ReflectedDrawerResult> result;
				ImVec2 checkbox{};
				const auto draw = [&types, &owner, ownerType, field, resolved, &value, &result, &checkbox]() -> Status
				{
					result = DrawReflectedValue(*field, value, { .Types = types, .Resolve = { .Owner = resolved ? &owner : nullptr, .OwnerType = ownerType, .Key = {} }, .Path = "Choice" });
					checkbox = Test::PanelInteractionUi::LastItemCenter();
					return {};
				};
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Click(draw, checkbox));
				REQUIRE(result);
				CHECK(result->Changed == resolved);
				CHECK(result->Committed == resolved);
				CHECK(value.AsVariant().Get() == Json(!resolved));
				CHECK(owner.Choice.Get() == Json(true));
			}
		}

		TEST_CASE("ReflectedDrawers: searchable references own their query and selecting a result commits the mixed value")
		{
			Test::EditorTestFixture fixture("DrawerSearch");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const auto* field = types.FindComponent<MeshRendererComponent>()->FindField("Mesh");
			Value value = Value::FromAssetRef(BuiltinAssetHandles::CubeMesh);
			std::string query;
			ImVec2 candidate{};
			uint32_t searches = 0;
			ReflectedDrawerContext context{ .Types = types, .Path = "MeshRenderer.Mesh", .Mixed = true, .FindReferences = [&query, &candidate, &searches](const FieldInfo& requested, std::string_view text) -> Result<std::vector<ReflectedReferenceCandidate>>
			{
				CHECK(requested.GetKind() == FieldType::AssetRef);
				CHECK(requested.GetMeta().AssetFilter == "Mesh");
				query = text;
				++searches;
				candidate = ImGui::GetCursorScreenPos();
				if (text.empty() || text == "sphere")
					return std::vector<ReflectedReferenceCandidate>{ { BuiltinAssetHandles::SphereMesh, "Sphere##owned label", "engine://Meshes/Sphere" } };
				return std::vector<ReflectedReferenceCandidate>{};
			} };
			Test::PanelInteractionUi ui;
			Result<ReflectedDrawerResult> result;
			ImVec2 browse{};
			const auto draw = [&field, &value, &context, &result, &browse]() -> Status
			{
				result = DrawReflectedValue(*field, value, context);
				browse = Test::PanelInteractionUi::LastItemCenter();
				return {};
			};
			REQUIRE(ui.Frame(draw));
			CHECK(searches == 0);
			REQUIRE(ui.Click(draw, browse));
			REQUIRE(ui.Frame(draw));
			CHECK(searches > 0);
			ImGui::GetIO().AddInputCharactersUTF8("sphere");
			REQUIRE(ui.Frame(draw));
			CHECK(query == "sphere");
			REQUIRE(ui.Frame(draw));
			CHECK(query == "sphere");
			CHECK(value.AsUUID() == BuiltinAssetHandles::CubeMesh);
			REQUIRE(ui.Click(draw, ImVec2(candidate.x + 30.0f, candidate.y + 8.0f)));
			REQUIRE(result);
			CHECK(result->Activated);
			CHECK(result->Changed);
			CHECK(result->Committed);
			CHECK(value.AsUUID() == BuiltinAssetHandles::SphereMesh);
		}

		TEST_CASE("ReflectedDrawers: failed reference searches return visible diagnostics without changing the value")
		{
			Test::EditorTestFixture fixture("DrawerSearchFailure");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const auto* field = types.FindComponent<MeshRendererComponent>()->FindField("Mesh");
			Value value = Value::FromAssetRef(BuiltinAssetHandles::CubeMesh);
			ReflectedDrawerContext context{ .Types = types, .Path = "MeshRenderer.Mesh", .FindReferences = [](const FieldInfo&, std::string_view) -> Result<std::vector<ReflectedReferenceCandidate>>
			{
				return MakeError(ErrorCode::InvalidState, "the addressed project is unavailable");
			} };
			Test::PanelInteractionUi ui;
			Result<ReflectedDrawerResult> result;
			ImVec2 browse{};
			const auto draw = [&field, &value, &context, &result, &browse]() -> Status
			{
				result = DrawReflectedValue(*field, value, context);
				browse = Test::PanelInteractionUi::LastItemCenter();
				return {};
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, browse));
			REQUIRE_FALSE(result);
			CHECK(result.error().GetCode() == ErrorCode::InvalidState);
			CHECK(result.error().GetMessageText() == "the addressed project is unavailable");
			CHECK(value.AsUUID() == BuiltinAssetHandles::CubeMesh);
		}

		TEST_CASE("ReflectedDrawers: canonical asset drops replace mixed values and commit one edit")
		{
			Test::EditorTestFixture fixture("DrawerAssetDrop");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const auto* component = types.FindComponent<MeshRendererComponent>();
			REQUIRE(component);
			const auto* field = component->FindField("Mesh");
			REQUIRE(field);
			Value value = Value::FromAssetRef(BuiltinAssetHandles::CubeMesh);
			const std::string payload = BuiltinAssetHandles::SphereMesh.ToString();
			Test::PanelInteractionUi ui;
			const auto result = DeliverInspectorAssetPayload(ui, *field, value, { .Types = types, .Path = "MeshRenderer.Mesh", .Mixed = true },
				std::as_bytes(std::span(payload.c_str(), payload.size() + 1)));
			REQUIRE(result);
			CHECK(result->Activated);
			CHECK(result->Changed);
			CHECK(result->Committed);
			CHECK(value.AsUUID() == BuiltinAssetHandles::SphereMesh);
		}

		TEST_CASE("ReflectedDrawers: malformed asset drops never change the owned value")
		{
			Test::EditorTestFixture fixture("DrawerBadDrop");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const auto* field = types.FindComponent<MeshRendererComponent>()->FindField("Mesh");
			const std::string valid = BuiltinAssetHandles::SphereMesh.ToString();
			std::vector<std::string> malformed;
			malformed.push_back(valid);
			malformed.push_back(valid + "x");
			malformed.push_back(valid + std::string(2, '\0'));
			malformed.push_back(std::string("0000000000000000\0", 17));
			malformed.push_back(std::string("00000000000ABCD1\0", 17));
			malformed.push_back(std::string("00000000000000gg\0", 17));
			malformed.push_back(std::string("00000000000000\xc3\xa9\0", 17));
			malformed.push_back(std::string(8, '\x02')); // legacy binary UUID payload
			for (const auto& payload : malformed)
			{
				INFO("payload byte count: ", payload.size());
				Value value = Value::FromAssetRef(BuiltinAssetHandles::CubeMesh);
				Test::PanelInteractionUi ui;
				const auto result = DeliverInspectorAssetPayload(ui, *field, value, { .Types = types, .Path = "MeshRenderer.Mesh" }, AsBytes(payload));
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::Validation);
				CHECK(value.AsUUID() == BuiltinAssetHandles::CubeMesh);
			}
		}

		TEST_CASE("ReflectedDrawers: read-only contexts and fields reject asset drops")
		{
			Test::EditorTestFixture fixture("DrawerReadonlyDrop");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const auto* mesh = types.FindComponent<MeshRendererComponent>()->FindField("Mesh");
			FieldInfo frozen({ .Name = "Mesh", .Description = "Read-only mesh", .Type = &mesh->GetType(), .Meta = { .ReadOnly = true }, .Accessor = {} });
			for (const bool readOnlyContext : { true, false })
			{
				Test::PanelInteractionUi ui;
				Value value = Value::FromAssetRef(BuiltinAssetHandles::CubeMesh);
				const std::string payload = BuiltinAssetHandles::SphereMesh.ToString();
				const auto result = DeliverInspectorAssetPayload(ui, readOnlyContext ? *mesh : frozen, value,
					{ .Types = types, .Path = "MeshRenderer.Mesh", .ReadOnly = readOnlyContext }, std::as_bytes(std::span(payload.c_str(), payload.size() + 1)));
				REQUIRE(result);
				CHECK_FALSE(result->Changed);
				CHECK_FALSE(result->Committed);
				CHECK(value.AsUUID() == BuiltinAssetHandles::CubeMesh);
			}
		}
	}

}
