#include "TestsPCH.h"
#include "Editor/Drawers/ReflectedDrawers.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/Ui/EditorStyle.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/EditorTestFixture.h"

#include <imgui_internal.h>

#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <string>

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

	// Read the rectangle recorded by ImGui's navigation pass; the action itself is always real mouse/keyboard input.
	static ImRect LocateDrawerItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, ImGuiID id, const char* windowName = nullptr)
	{
		ImRect rectangle;
		const auto locate = [&draw, id, windowName, &rectangle]() -> Status
		{
			ImGui::SetNavWindow(windowName == nullptr ? ImGui::GetCurrentWindow() : ImGui::FindWindowByName(windowName));
			ImGui::SetNavID(id, ImGuiNavLayer_Main, ImGui::GetCurrentFocusScope(), ImRect());
			ImGui::GetCurrentContext()->NavIdIsAlive = false;
			ENGINE_TRY(draw());
			const auto& gui = *ImGui::GetCurrentContext();
			REQUIRE(gui.NavIdIsAlive);
			REQUIRE(gui.NavId == id);
			rectangle = ImGui::WindowRectRelToAbs(gui.NavWindow, gui.NavWindow->NavRectRel[gui.NavLayer]);
			return {};
		};
		REQUIRE(ui.Frame(locate));
		return rectangle;
	}

	static ImGuiID DrawerControlId(std::string_view path, const char* label, bool variant = false)
	{
		ImGui::PushID(static_cast<int>(FNV1a32(path)));
		if (variant)
			ImGui::PushID(static_cast<int>(FNV1a32(path)));
		ImGui::PushID("Property");
		const ImGuiID id = ImGui::GetID(label);
		ImGui::PopID();
		if (variant)
			ImGui::PopID();
		ImGui::PopID();
		return id;
	}

	static Result<ReflectedDrawerResult> DeliverInspectorAssetPayload(Test::PanelInteractionUi& ui, const FieldInfo& field, Value& value,
		const ReflectedDrawerContext& context, std::span<const std::byte> bytes)
	{
		Result<ReflectedDrawerResult> result;
		bool drag = false;
		ImGuiID target = 0;
		const auto draw = [&drag, bytes, &field, &value, &context, &result, &target]() -> Status
		{
			if (drag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceExtern))
			{
				ImGui::SetDragDropPayload("ENGINE_ASSET", bytes.data(), bytes.size());
				ImGui::EndDragDropSource();
			}
			target = DrawerControlId(context.Path, "##Reference");
			result = DrawReflectedValue(field, value, context);
			return {};
		};
		ENGINE_TRY(ui.Frame(draw));
		const ImVec2 center = LocateDrawerItem(ui, draw, target).GetCenter();
		ImGui::GetIO().AddMousePosEvent(center.x, center.y);
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		drag = true;
		ENGINE_TRY(ui.Frame(draw));
		ENGINE_TRY(ui.Frame(draw));
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		ENGINE_TRY(ui.Frame(draw));
		return result;
	}

	static ImGuiID DrawerIntId(ImGuiID parent, int key)
	{
		return ImHashData(&key, sizeof(key), parent);
	}

	// ImGui records this rectangle when it submits the requested navigation item in the named window. Tests then click
	// the real geometry; they never toggle storage or synthesize a successful widget result.
	static ImRect LocateDrawerWindowItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, const char* windowName, ImGuiID id)
	{
		ImRect rectangle;
		const auto locate = [&draw, windowName, id, &rectangle]() -> Status
		{
			ImGuiWindow* window = ImGui::FindWindowByName(windowName);
			REQUIRE(window != nullptr);
			ImGui::SetNavWindow(window);
			ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
			ImGui::GetCurrentContext()->NavIdIsAlive = false;
			ENGINE_TRY(draw());
			const auto& gui = *ImGui::GetCurrentContext();
			REQUIRE(gui.NavIdIsAlive);
			REQUIRE(gui.NavId == id);
			rectangle = ImGui::WindowRectRelToAbs(gui.NavWindow, gui.NavWindow->NavRectRel[gui.NavLayer]);
			CHECK(rectangle.GetWidth() > 0.0f);
			CHECK(rectangle.GetHeight() > 0.0f);
			return {};
		};
		REQUIRE(ui.Frame(locate));
		return rectangle;
	}

	static ImGuiWindow* DrawerPopup()
	{
		for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
		{
			if (window->Active && (window->Flags & ImGuiWindowFlags_Popup) != 0)
				return window;
		}
		return nullptr;
	}

	static void ReplaceDrawerText(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, const char* text)
	{
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_A, true);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddKeyEvent(ImGuiKey_A, false);
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddInputCharactersUTF8(text);
		REQUIRE(ui.Frame(draw));
	}

	// One axis of a vector drawer, or of a rotation's raw components under their disclosure.
	static ImGuiID DrawerAxisId(std::string_view path, int axis, bool components = false)
	{
		ImGui::PushID(static_cast<int>(FNV1a32(path)));
		ImGui::PushID("Property");
		if (components)
			ImGui::PushID("##Components");
		ImGui::PushID(axis);
		const ImGuiID id = ImGui::GetID("##Axis");
		ImGui::PopID();
		if (components)
			ImGui::PopID();
		ImGui::PopID();
		ImGui::PopID();
		return id;
	}

	// A real drag with no drag threshold or double click: each pixel moves a float by one step of the field's speed.
	static void DragDrawerItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, ImVec2 from, float pixels)
	{
		ImGui::GetIO().MouseDragThreshold = 0.0f;
		// Consecutive drags from one point must not read as a double click, which opens the text entry instead.
		ImGui::GetIO().MouseDoubleClickTime = 0.0f;
		ImGui::GetIO().AddMousePosEvent(from.x, from.y);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddMousePosEvent(from.x + pixels, from.y);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		REQUIRE(ui.Frame(draw));
	}

	// Ctrl+click turns a drag into its text entry; the typed text applies on Enter.
	static void TypeIntoDrawerItem(Test::PanelInteractionUi& ui, const std::function<Status()>& draw, ImVec2 at, const char* text)
	{
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
		REQUIRE(ui.Click(draw, at));
		ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
		REQUIRE(ui.Frame(draw));
		ReplaceDrawerText(ui, draw, text);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
		REQUIRE(ui.Frame(draw));
		ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
		REQUIRE(ui.Frame(draw));
	}

	// Draws `field` into `value` every frame, keeps the last result and the rendered text; a drawer error fails the frame.
	struct DrawerFrame
	{
		const FieldInfo& Field;
		Value& Edited;
		ReflectedDrawerContext Context;
		ReflectedDrawerResult Last{};
		std::string Text{};

		Status operator()()
		{
			ImGui::LogToBuffer(0);
			auto result = DrawReflectedValue(Field, Edited, Context);
			Text = ImGui::GetCurrentContext()->LogBuffer.c_str();
			ImGui::LogFinish();
			if (!result)
				return std::unexpected(std::move(result).error());
			Last = *result;
			return {};
		}
	};

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
				ImGuiID keyId = 0;
				const auto draw = [&types, field, &value, &result, &keyId]() -> Status
				{
					ImGui::PushID(static_cast<int>(FNV1a32("Weights")));
					ImGui::PushID("##Container");
					ImGui::PushID(static_cast<int>(FNV1a32("First")));
					keyId = ImGui::GetID("##Key");
					ImGui::PopID();
					ImGui::PopID();
					ImGui::PopID();
					result = DrawReflectedValue(*field, value, { .Types = types, .Path = "Weights" });
					return {};
				};
				REQUIRE(ui.Frame(draw));
				REQUIRE(ui.Click(draw, LocateDrawerItem(ui, draw, keyId).GetCenter()));
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
				ImGuiID checkbox = 0;
				const auto draw = [&types, &owner, ownerType, field, resolved, &value, &result, &checkbox]() -> Status
				{
					result = DrawReflectedValue(*field, value, { .Types = types, .Resolve = { .Owner = resolved ? &owner : nullptr, .OwnerType = ownerType, .Key = {} }, .Path = "Choice" });
					checkbox = DrawerControlId("Choice", "##Value", true);
					return {};
				};
				REQUIRE(ui.Frame(draw));
				if (resolved)
					REQUIRE(ui.Click(draw, LocateDrawerItem(ui, draw, checkbox).GetCenter()));
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
			ImGuiID candidateId = 0;
			std::string candidateWindow;
			uint32_t searches = 0;
			ReflectedDrawerContext context{ .Types = types, .Path = "MeshRenderer.Mesh", .Mixed = true, .FindReferences = [&query, &searches](const FieldInfo& requested, std::string_view text) -> Result<std::vector<ReflectedReferenceCandidate>>
			{
				CHECK(requested.GetKind() == FieldType::AssetRef);
				CHECK(requested.GetMeta().AssetFilter == "Mesh");
				query = text;
				++searches;
				if (text.empty() || text == "sphere")
					return std::vector<ReflectedReferenceCandidate>{ { BuiltinAssetHandles::SphereMesh, "Sphere##owned label", "engine://Meshes/Sphere" } };
				return std::vector<ReflectedReferenceCandidate>{};
			} };
			Test::PanelInteractionUi ui;
			Result<ReflectedDrawerResult> result;
			ImGuiID browse = 0;
			const auto draw = [&field, &value, &context, &result, &browse]() -> Status
			{
				result = DrawReflectedValue(*field, value, context);
				browse = DrawerControlId(context.Path, "...##Browse");
				return {};
			};
			REQUIRE(ui.Frame(draw));
			CHECK(searches == 0);
			REQUIRE(ui.Click(draw, LocateDrawerItem(ui, draw, browse).GetCenter()));
			REQUIRE(ui.Frame(draw));
			CHECK(searches > 0);
			ImGui::GetIO().AddInputCharactersUTF8("sphere");
			REQUIRE(ui.Frame(draw));
			CHECK(query == "sphere");
			REQUIRE(ui.Frame(draw));
			CHECK(query == "sphere");
			CHECK(value.AsUUID() == BuiltinAssetHandles::CubeMesh);
			for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
			{
				if (window->Active && std::string_view(window->Name).contains("/Results_"))
				{
					candidateWindow = window->Name;
					const ImGuiID owner = ImHashStr(BuiltinAssetHandles::SphereMesh.ToString().c_str(), 0, window->ID);
					candidateId = ImHashStr("##Candidate", 0, owner);
				}
			}
			REQUIRE(candidateId != 0);
			REQUIRE(ui.Click(draw, LocateDrawerItem(ui, draw, candidateId, candidateWindow.c_str()).GetCenter()));
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
			ImGuiID browse = 0;
			const auto draw = [&field, &value, &context, &result, &browse]() -> Status
			{
				result = DrawReflectedValue(*field, value, context);
				browse = DrawerControlId(context.Path, "...##Browse");
				return {};
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Click(draw, LocateDrawerItem(ui, draw, browse).GetCenter()));
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

		TEST_CASE("ReflectedDrawers: vector axes fit narrow and scaled property columns")
		{
			Test::EditorTestFixture fixture("InspectorVectorWidths");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* field = types.FindComponent<TransformComponent>()->FindField("Translation");
			REQUIRE(field != nullptr);
			for (const float scale : { 1.0f, 1.5f })
			{
				for (const float width : { 220.0f, 320.0f, 420.0f })
				{
					INFO(scale, " at ", width);
					Test::PanelInteractionUi ui(false);
					Utils::ApplyEditorStyle(scale);
					Value value = Value::FromVec3(glm::vec3(12.5f, -24.25f, 123.0f));
					const Value original = value;
					ImGuiID property = 0;
					const auto draw = [&types, field, &value, width, &property]() -> Status
					{
						ImGui::SetNextWindowSize(ImVec2(width, 700.0f));
						ImGui::Begin("Vector design");
						ImGui::PushID(static_cast<int>(FNV1a32("Transform.Translation")));
						property = ImGui::GetID("Property");
						ImGui::PopID();
						const auto result = DrawReflectedValue(*field, value, { .Types = types, .Path = "Transform.Translation", .DisplayLabel = "Position" });
						ImGui::End();
						return result ? Status{} : Status(std::unexpected(result.error()));
					};
					REQUIRE(ui.Frame(draw));
					REQUIRE(ui.Frame(draw));
					std::array<ImRect, 3> axes;
					for (int axis = 0; axis < 3; ++axis)
					{
						axes[static_cast<size_t>(axis)] = LocateDrawerWindowItem(ui, draw, "Vector design", ImHashStr("##Axis", 0, DrawerIntId(property, axis)));
						const ImGuiWindow* window = ImGui::FindWindowByName("Vector design");
						const auto& rectangle = axes[static_cast<size_t>(axis)];
						CHECK(rectangle.Min.x >= window->InnerRect.Min.x);
						CHECK(rectangle.Max.x <= window->InnerRect.Max.x);
						CHECK(rectangle.GetWidth() >= ImGui::CalcTextSize("X -1.23").x);
					}
					CHECK_FALSE(axes[0].Overlaps(axes[1]));
					CHECK_FALSE(axes[1].Overlaps(axes[2]));
					CHECK(value == original);
				}
			}
		}

		TEST_CASE("ReflectedDrawers: authored hash markers keep controls distinct and read-only values unchanged")
		{
			Test::EditorTestFixture fixture("InspectorAuthoredIds");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* field = types.FindComponent<MeshRendererComponent>()->FindField("Visible");
			REQUIRE(field != nullptr);
			for (const bool readOnly : { false, true })
			{
				Test::PanelInteractionUi ui;
				Value first = Value::FromBool(true);
				Value second = Value::FromBool(true);
				std::array<ImGuiID, 2> ids{};
				std::array<ReflectedDrawerResult, 2> results{};
				const auto draw = [&types, field, readOnly, &first, &second, &ids, &results]() -> Status
				{
					for (int index = 0; index < 2; ++index)
					{
						const std::string path = index == 0 ? "Fields[First###Enabled]" : "Fields[Second###Enabled]";
						ImGui::PushID(static_cast<int>(FNV1a32(path)));
						ImGui::PushID("Property");
						ids[static_cast<size_t>(index)] = ImGui::GetID("##Value");
						ImGui::PopID();
						ImGui::PopID();
						ENGINE_TRY_ASSIGN(results[static_cast<size_t>(index)], DrawReflectedValue(*field, index == 0 ? first : second, { .Types = types, .Path = path, .ReadOnly = readOnly, .DisplayLabel = index == 0 ? "First###Enabled" : "Second###Enabled" }));
					}
					return {};
				};
				REQUIRE(ui.Frame(draw));
				CHECK(ids[0] != ids[1]);
				const auto item = LocateDrawerWindowItem(ui, draw, "Panel interaction", ids[0]);
				REQUIRE(ui.Click(draw, item.GetCenter()));
				CHECK(first.AsBool() == readOnly);
				CHECK(second.AsBool());
				CHECK(results[0].Changed == !readOnly);
				CHECK(results[0].Committed == !readOnly);
				CHECK_FALSE(results[1].Changed);
			}
		}

		TEST_CASE("ReflectedDrawers: narrow references preserve paste and clear with readable lookup")
		{
			Test::EditorTestFixture fixture("InspectorReferenceDesign");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* field = types.FindComponent<MeshRendererComponent>()->FindField("Mesh");
			REQUIRE(field != nullptr);
			Test::PanelInteractionUi ui(false);
			Utils::ApplyEditorStyle();
			Value value = Value::FromAssetRef(BuiltinAssetHandles::CubeMesh);
			Result<ReflectedDrawerResult> result;
			ImGuiID property = 0;
			UUID described{};
			const auto draw = [&types, field, &value, &result, &property, &described]() -> Status
			{
				ImGui::SetNextWindowSize(ImVec2(220.0f, 600.0f));
				ImGui::Begin("Reference design");
				ImGui::PushID(static_cast<int>(FNV1a32("Mesh")));
				property = ImGui::GetID("Property");
				ImGui::PopID();
				result = DrawReflectedValue(*field, value, { .Types = types, .Path = "Mesh", .DescribeReference = [&described](const FieldInfo&, UUID id)
				{
					described = id;
					return std::optional(ReflectedReferenceCandidate{ id, "Readable###resource name", "engine://Meshes/Cube" });
				} });
				ImGui::End();
				return result ? Status{} : Status(std::unexpected(result.error()));
			};
			REQUIRE(ui.Frame(draw));
			REQUIRE(ui.Frame(draw));
			CHECK(described == BuiltinAssetHandles::CubeMesh);
			const ImRect browse = LocateDrawerWindowItem(ui, draw, "Reference design", ImHashStr("...##Browse", 0, property));
			const auto* window = ImGui::FindWindowByName("Reference design");
			CHECK(browse.Max.x <= window->InnerRect.Max.x);
			REQUIRE(ui.Click(draw, browse.GetCenter()));
			REQUIRE(ui.Frame(draw));
			ImGuiWindow* popup = DrawerPopup();
			REQUIRE(popup != nullptr);
			const ImRect input = LocateDrawerWindowItem(ui, draw, popup->Name, ImHashStr("##ReferenceId", 0, popup->ID));
			REQUIRE(ui.Click(draw, input.GetCenter()));
			ReplaceDrawerText(ui, draw, BuiltinAssetHandles::SphereMesh.ToString().c_str());
			CHECK(value.AsUUID() == BuiltinAssetHandles::CubeMesh);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
			REQUIRE(ui.Frame(draw));
			REQUIRE(result);
			CHECK(result->Committed);
			CHECK(value.AsUUID() == BuiltinAssetHandles::SphereMesh);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
			REQUIRE(ui.Frame(draw));
			const ImRect clear = LocateDrawerWindowItem(ui, draw, "Reference design", ImHashStr("x##Clear", 0, property));
			CHECK(clear.Max.x <= window->InnerRect.Max.x);
			REQUIRE(ui.Click(draw, clear.GetCenter()));
			REQUIRE(result);
			CHECK(result->Committed);
			CHECK_FALSE(value.AsUUID().IsValid());
		}

		TEST_CASE("ReflectedDrawers: dragging a float never rounds the stored value to its display format")
		{
			Test::EditorTestFixture fixture("DrawerFloatPrecision");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* translation = types.FindComponent<TransformComponent>()->FindField("Translation");
			const FieldInfo* intensity = types.FindComponent<DirectionalLightComponent>()->FindField("Intensity");
			REQUIRE(translation != nullptr);
			REQUIRE(intensity != nullptr);
			for (const float start : { 123.45f, 1234.5f })
			{
				CAPTURE(start);
				Test::PanelInteractionUi ui;
				Value value = Value::FromVec3({ start, -0.0f, 2.5f });
				DrawerFrame frame{ *translation, value, { .Types = types, .Path = "Translation" } };
				const std::function<Status()> draw = std::ref(frame);
				REQUIRE(ui.Frame(draw));
				// -0 shows as one zero; drawing never changes the stored value.
				CHECK(frame.Text.contains(std::format("X {}", start)));
				CHECK(frame.Text.contains("Y 0"));
				CHECK_FALSE(frame.Text.contains("-0"));
				CHECK(frame.Text.contains("Z 2.5"));
				CHECK(std::signbit(value.AsVec3().y));
				ImGuiID axis = 0;
				const std::function<Status()> locate = [&frame, &axis]()
				{
					axis = DrawerAxisId("Translation", 0);
					return frame();
				};
				REQUIRE(ui.Frame(locate));
				DragDrawerItem(ui, draw, LocateDrawerItem(ui, draw, axis).GetCenter(), 1.0f);
				// One pixel moves by exactly one step of the default speed, never to a value rounded for display.
				CHECK(value.AsVec3().x == start + 0.05f);
				CHECK(value.AsVec3().z == 2.5f);
			}
			Test::PanelInteractionUi ui;
			Value value = Value::FromFloat(0.0004f);
			DrawerFrame frame{ *intensity, value, { .Types = types, .Path = "Intensity" } };
			ImGuiID control = 0;
			const std::function<Status()> draw = [&frame, &control]()
			{
				control = DrawerControlId("Intensity", "##Value");
				return frame();
			};
			REQUIRE(ui.Frame(draw));
			CHECK(frame.Text.contains("0.0004"));
			DragDrawerItem(ui, draw, LocateDrawerItem(ui, draw, control).GetCenter(), 1.0f);
			CHECK(value.AsFloat() == 0.0004f + 0.05f);
		}

		TEST_CASE("ReflectedDrawers: numeric edits clamp to the field range and step over the minimum magnitude")
		{
			Test::EditorTestFixture fixture("DrawerNumericRange");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const auto* light = types.FindComponent<DirectionalLightComponent>();
			const FieldInfo* cascades = light->FindField("CascadeCount");
			const FieldInfo* angle = light->FindField("LightAngle");
			const FieldInfo* scale = types.FindComponent<TransformComponent>()->FindField("Scale");
			REQUIRE(cascades != nullptr);
			REQUIRE(angle != nullptr);
			REQUIRE(scale != nullptr);
			SUBCASE("an integer drag stops at Min and Max")
			{
				Test::PanelInteractionUi ui;
				Value value = Value::FromUInt32(4);
				DrawerFrame frame{ *cascades, value, { .Types = types, .Path = "CascadeCount" } };
				ImGuiID control = 0;
				const std::function<Status()> draw = [&frame, &control]()
				{
					control = DrawerControlId("CascadeCount", "##Value");
					return frame();
				};
				REQUIRE(ui.Frame(draw));
				const ImVec2 center = LocateDrawerItem(ui, draw, control).GetCenter();
				DragDrawerItem(ui, draw, center, 40.0f);
				CHECK(value.AsUInt32() == 4u);
				DragDrawerItem(ui, draw, center, -200.0f);
				CHECK(value.AsUInt32() == 1u);
			}
			SUBCASE("typed text clamps to Max")
			{
				Test::PanelInteractionUi ui;
				Value value = Value::FromFloat(10.0f);
				DrawerFrame frame{ *angle, value, { .Types = types, .Path = "LightAngle" } };
				ImGuiID control = 0;
				const std::function<Status()> draw = [&frame, &control]()
				{
					control = DrawerControlId("LightAngle", "##Value");
					return frame();
				};
				REQUIRE(ui.Frame(draw));
				TypeIntoDrawerItem(ui, draw, LocateDrawerItem(ui, draw, control).GetCenter(), "500");
				CHECK(value.AsFloat() == 90.0f);
			}
			SUBCASE("a drag through zero steps over the excluded magnitudes")
			{
				Test::PanelInteractionUi ui;
				Value value = Value::FromVec3({ 0.05f, 1.0f, 1.0f });
				DrawerFrame frame{ *scale, value, { .Types = types, .Path = "Scale" } };
				ImGuiID axis = 0;
				const std::function<Status()> draw = [&frame, &axis]()
				{
					axis = DrawerAxisId("Scale", 0);
					return frame();
				};
				REQUIRE(ui.Frame(draw));
				const ImVec2 center = LocateDrawerItem(ui, draw, axis).GetCenter();
				DragDrawerItem(ui, draw, center, -1.0f);
				CHECK(value.AsVec3().x == MinTransformScaleMagnitude);
				DragDrawerItem(ui, draw, center, -3.0f);
				CHECK(value.AsVec3().x == doctest::Approx(MinTransformScaleMagnitude - 0.15f));
				CHECK(value.AsVec3().y == 1.0f);
			}
		}

		TEST_CASE("ReflectedDrawers: a rotation edits as Euler degrees or renormalized components and stays a unit quaternion")
		{
			Test::EditorTestFixture fixture("DrawerRotation");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* rotation = types.FindComponent<BoxColliderComponent>()->FindField("Rotation");
			REQUIRE(rotation != nullptr);
			Test::PanelInteractionUi ui;
			Value value = Value::FromQuat(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			DrawerFrame frame{ *rotation, value, { .Types = types, .Path = "Rotation" } };
			ImGuiID yaw = 0;
			ImGuiID components = 0;
			ImGuiID componentX = 0;
			const std::function<Status()> draw = [&frame, &yaw, &components, &componentX]()
			{
				yaw = DrawerAxisId("Rotation", 1);
				components = DrawerControlId("Rotation", "##Components");
				componentX = DrawerAxisId("Rotation", 0, true);
				return frame();
			};
			REQUIRE(ui.Frame(draw));
			CHECK(frame.Text.contains("Quaternion components"));
			TypeIntoDrawerItem(ui, draw, LocateDrawerItem(ui, draw, yaw).GetCenter(), "30");
			const glm::quat expected = TransformSystem::QuaternionFromEulerDegrees({ 0.0f, 30.0f, 0.0f });
			CHECK(std::fabs(glm::dot(value.AsQuat(), expected)) == doctest::Approx(1.0f));
			CHECK(glm::length(value.AsQuat()) == doctest::Approx(1.0f));
			REQUIRE(ui.Click(draw, LocateDrawerItem(ui, draw, components).GetCenter()));
			REQUIRE(ui.Frame(draw));
			const glm::quat before = value.AsQuat();
			DragDrawerItem(ui, draw, LocateDrawerItem(ui, draw, componentX).GetCenter(), 5.0f);
			CHECK(frame.Last.ChangedComponents == 0u);
			CHECK(glm::length(value.AsQuat()) == doctest::Approx(1.0f).epsilon(1e-5));
			CHECK(value.AsQuat().x > before.x);
		}

		TEST_CASE("ReflectedDrawers: a mixed vector marks only differing axes and reports the axes an edit changed")
		{
			Test::EditorTestFixture fixture("DrawerMixedAxes");
			const auto& types = fixture.GetEditor().GetTypeRegistry();
			const FieldInfo* translation = types.FindComponent<TransformComponent>()->FindField("Translation");
			REQUIRE(translation != nullptr);
			Test::PanelInteractionUi ui;
			Value value = Value::FromVec3({ 1.0f, 2.0f, 3.0f });
			DrawerFrame frame{ *translation, value, { .Types = types, .Path = "Translation", .Mixed = true, .MixedComponents = 0b010u } };
			ImGuiID axis = 0;
			const std::function<Status()> draw = [&frame, &axis]()
			{
				axis = DrawerAxisId("Translation", 2);
				return frame();
			};
			REQUIRE(ui.Frame(draw));
			CHECK(frame.Text.contains("X 1"));
			CHECK(frame.Text.contains("--"));
			CHECK_FALSE(frame.Text.contains("Y 2"));
			CHECK(frame.Text.contains("Z 3"));
			ImGui::GetIO().MouseDragThreshold = 0.0f;
			const ImVec2 center = LocateDrawerItem(ui, draw, axis).GetCenter();
			ImGui::GetIO().AddMousePosEvent(center.x, center.y);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			REQUIRE(ui.Frame(draw));
			ImGui::GetIO().AddMousePosEvent(center.x + 1.0f, center.y);
			REQUIRE(ui.Frame(draw));
			CHECK(frame.Last.Changed);
			CHECK(frame.Last.ChangedComponents == 0b100u);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			REQUIRE(ui.Frame(draw));
			CHECK(value.AsVec3() == glm::vec3(1.0f, 2.0f, 3.0f + 0.05f));
		}
	}

}
