#include "EditorPCH.h"
#include "Editor/Drawers/ReflectedDrawers.h"

#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace Engine {

	namespace Utils {

		static void MergeDrawerResult(ReflectedDrawerResult& into, const ReflectedDrawerResult& child)
		{
			into.Activated |= child.Activated;
			into.Changed |= child.Changed;
			into.Committed |= child.Committed;
			into.Cancelled |= child.Cancelled;
		}

		static ReflectedDrawerResult DrawerItemResult(bool changed, bool immediate = false)
		{
			return { .Activated = ImGui::IsItemActivated(), .Changed = changed, .Committed = ImGui::IsItemDeactivatedAfterEdit() || (changed && immediate), .Cancelled = ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape) };
		}

		static Result<Value> DefaultDrawerValue(const TypeInfo& type)
		{
			if (!type.HasOps())
				return MakeError(ErrorCode::Unsupported, "this field has no default value");
			ObjectPtr object = type.GetOps().Create();
			if (type.GetKind() == FieldType::Struct)
			{
				ENGINE_TRY_ASSIGN(Json json, type.GetStruct()->ToJson(object.get()));
				return ValueFromJson(JsonReader(json), type);
			}
			if (type.GetKind() == FieldType::Array)
				return Value::FromArray({});
			if (type.GetKind() == FieldType::Map)
				return Value::FromMap({}, {});
			return type.GetOps().Read(object.get());
		}

		static Status DrawInspectorReferenceSearch(const FieldInfo& field, Value& value,
			const ReflectedDrawerContext& context, ReflectedDrawerResult& result)
		{
			if (!context.FindReferences)
				return {};
			ImGui::SameLine();
			if (ImGui::SmallButton("Browse"))
				ImGui::OpenPopup("ReferenceSearch");
			ImGui::SetNextWindowSize(ImVec2(520.0f, 0.0f), ImGuiCond_Appearing);
			if (!ImGui::BeginPopup("ReferenceSearch", ImGuiWindowFlags_AlwaysAutoResize))
				return {};

			// Query bytes live in this popup's ImGui storage. No process static, heap-owned UI pointer or borrowed source
			// survives Draw. Each popup has the complete reflected path in its ID, including array indices/map keys.
			std::array<char, 512> query{};
			ImGuiStorage* storage = ImGui::GetStateStorage();
			const ImGuiID lengthId = ImGui::GetID("QueryLength");
			if (ImGui::IsWindowAppearing())
			{
				storage->SetInt(lengthId, 0);
				ImGui::SetKeyboardFocusHere();
			}
			const int length = std::clamp(storage->GetInt(lengthId), 0, static_cast<int>(query.size()) - 1);
			for (int index = 0; index < length; ++index)
			{
				ImGui::PushID(index);
				query[static_cast<size_t>(index)] = static_cast<char>(storage->GetInt(ImGui::GetID("QueryByte")));
				ImGui::PopID();
			}
			if (ImGui::InputTextWithHint("##Query", "Search name, path or UUID", query.data(), query.size()))
			{
				const size_t size = std::char_traits<char>::length(query.data());
				storage->SetInt(lengthId, static_cast<int>(size));
				for (size_t index = 0; index < size; ++index)
				{
					ImGui::PushID(static_cast<int>(index));
					storage->SetInt(ImGui::GetID("QueryByte"), static_cast<unsigned char>(query[index]));
					ImGui::PopID();
				}
			}
			auto candidates = context.FindReferences(field, query.data());
			if (!candidates)
			{
				ImGui::TextWrapped("%s", candidates.error().GetMessageText().c_str());
				ImGui::EndPopup();
				return std::unexpected(std::move(candidates).error());
			}
			if (candidates->empty())
				ImGui::TextDisabled("No matching references.");
			if (ImGui::BeginChild("Results", ImVec2(0.0f, 240.0f)))
			{
				for (const auto& candidate : *candidates)
				{
					const std::string id = candidate.Id.ToString();
					ImGui::PushID(id.c_str());
					ImGui::BeginDisabled(!candidate.Id.IsValid());
					// Render names as text, not label IDs: authored names containing ## remain readable and distinct.
					const ImVec2 position = ImGui::GetCursorScreenPos();
					const float lineHeight = ImGui::GetTextLineHeight();
					const bool selected = ImGui::Selectable("##Candidate", !context.Mixed && candidate.Id == value.AsUUID(), 0,
						ImVec2(0.0f, lineHeight * 2.0f + ImGui::GetStyle().ItemSpacing.y));
					ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), candidate.Label.c_str());
					ImGui::GetWindowDrawList()->AddText(ImVec2(position.x, position.y + lineHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled), candidate.Path.c_str());
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s\n%s", candidate.Path.c_str(), id.c_str());
					ImGui::EndDisabled();
					ImGui::PopID();
					if (selected && !context.ReadOnly && !field.IsReadOnly())
					{
						value = field.GetKind() == FieldType::EntityRef ? Value::FromEntityRef(candidate.Id) : Value::FromAssetRef(candidate.Id);
						result.Activated = result.Changed = result.Committed = true;
						ImGui::CloseCurrentPopup();
						break;
					}
				}
			}
			ImGui::EndChild();
			ImGui::EndPopup();
			return {};
		}

		static Result<ReflectedDrawerResult> DrawInspectorContainer(const FieldInfo& field, Value& value, const ReflectedDrawerContext& context)
		{
			ReflectedDrawerResult result;
			if (!ImGui::TreeNodeEx(field.GetName().c_str(), ImGuiTreeNodeFlags_DefaultOpen))
				return result;
			std::vector<Value> elements(value.GetElements().begin(), value.GetElements().end());
			std::vector<std::string> keys(value.GetKeys().begin(), value.GetKeys().end());
			const bool isMap = field.GetKind() == FieldType::Map;
			const bool isStruct = field.GetKind() == FieldType::Struct;
			Result<Json> ownerJson = ValueToJson(value, field.GetType());
			if (!ownerJson)
			{
				ImGui::TreePop();
				return std::unexpected(std::move(ownerJson).error());
			}
			const JsonReader ownerReader(*ownerJson);
			Status status;
			for (size_t i = 0; i < elements.size(); ++i)
			{
				const std::string key = isStruct || isMap ? keys[i] : std::to_string(i);
				ImGui::PushID(key.c_str());
				ReflectedDrawerContext child = context;
				child.Path += isStruct ? "." + key : "[" + key + "]";
				Scope<FieldInfo> synthetic;
				const FieldInfo* childField = nullptr;
				if (isStruct)
				{
					childField = field.GetType().GetStruct()->FindField(key);
					child.Resolve.Owner = nullptr;
					child.Resolve.OwnerType = field.GetType().GetStruct();
					child.Resolve.OwnerJson = &ownerReader;
					child.Resolve.Key = {};
				}
				else
				{
					FieldMeta childMeta = field.GetMeta();
					const TypeInfo* elementType = field.GetType().GetElement();
					if (elementType->GetKind() == FieldType::AssetRef)
						childMeta.AssetFilter = elementType->GetAssetTypeName();
					synthetic = CreateScope<FieldInfo>(FieldInfo::Specification{ .Name = key, .Description = field.GetDescription(), .Type = elementType, .Meta = std::move(childMeta), .Accessor = {}, .Resolver = field.GetResolver() });
					childField = synthetic.get();
					if (isMap)
						child.Resolve.Key = key;
				}
				if (isMap)
				{
					std::string renamed = key;
					if (ImGui::InputText("Key", &renamed, ImGuiInputTextFlags_EnterReturnsTrue) && renamed != key)
					{
						if (renamed.empty() || std::find(keys.begin(), keys.end(), renamed) != keys.end())
							status = MakeError(ErrorCode::Validation, "map keys must be nonempty and unique");
						else
						{
							keys[i] = std::move(renamed);
							result.Activated = result.Changed = result.Committed = true;
						}
					}
				}
				if (childField != nullptr && !childField->GetMeta().Hidden && status)
				{
					auto drawn = DrawReflectedValue(*childField, elements[i], child);
					if (drawn)
						MergeDrawerResult(result, *drawn);
					else
						status = std::unexpected(std::move(drawn).error());
				}
				bool remove = false;
				if (!isStruct)
				{
					ImGui::SameLine();
					remove = ImGui::SmallButton("Remove");
				}
				ImGui::PopID();
				if (!status)
					break;
				if (remove)
				{
					elements.erase(elements.begin() + static_cast<ptrdiff_t>(i));
					if (isMap)
						keys.erase(keys.begin() + static_cast<ptrdiff_t>(i));
					result.Activated = result.Changed = result.Committed = true;
					break;
				}
			}
			if (!isStruct && ImGui::SmallButton(isMap ? "Add key" : "Add element"))
			{
				auto added = DefaultDrawerValue(*field.GetType().GetElement());
				if (!added)
					status = std::unexpected(std::move(added).error());
				else
				{
					if (isMap)
					{
						std::string key = "NewKey";
						for (size_t suffix = 1; std::find(keys.begin(), keys.end(), key) != keys.end(); ++suffix)
							key = std::format("NewKey{}", suffix);
						keys.push_back(std::move(key));
					}
					elements.push_back(std::move(*added));
					result.Activated = result.Changed = result.Committed = true;
				}
			}
			ImGui::TreePop();
			ENGINE_TRY(status);
			if (result.Changed)
			{
				if (isStruct)
					value = Value::FromStruct(std::move(keys), std::move(elements));
				else if (isMap)
				{
					std::vector<std::pair<std::string, Value>> entries;
					for (size_t i = 0; i < keys.size(); ++i)
						entries.emplace_back(std::move(keys[i]), std::move(elements[i]));
					std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
					{
						return left.first < right.first;
					});
					keys.clear();
					elements.clear();
					for (auto& [key, element] : entries)
					{
						keys.push_back(std::move(key));
						elements.push_back(std::move(element));
					}
					value = Value::FromMap(std::move(keys), std::move(elements));
				}
				else
					value = Value::FromArray(std::move(elements));
			}
			return result;
		}

		static Result<ReflectedDrawerResult> DrawInspectorScalar(const FieldInfo& field, Value& value, const ReflectedDrawerContext& context)
		{
			const char* label = field.GetName().c_str();
			const FieldType kind = field.GetKind();
			const float speed = static_cast<float>(field.GetMeta().Step.value_or(0.05));
			const float minimum = static_cast<float>(field.GetMeta().Min.value_or(-std::numeric_limits<float>::max()));
			const float maximum = static_cast<float>(field.GetMeta().Max.value_or(std::numeric_limits<float>::max()));
			bool changed = false;
			bool immediate = false;
			switch (kind)
			{
				case FieldType::Bool:
				{
					bool candidate = value.AsBool();
					changed = ImGui::Checkbox(label, &candidate);
					value = Value::FromBool(candidate);
					immediate = true;
					break;
				}
				case FieldType::Int32:
				{
					int32_t candidate = value.AsInt32();
					changed = ImGui::DragScalar(label, ImGuiDataType_S32, &candidate, speed);
					value = Value::FromInt32(candidate);
					break;
				}
				case FieldType::UInt32:
				{
					uint32_t candidate = value.AsUInt32();
					changed = ImGui::DragScalar(label, ImGuiDataType_U32, &candidate, speed);
					value = Value::FromUInt32(candidate);
					break;
				}
				case FieldType::Float:
				{
					float candidate = value.AsFloat();
					changed = ImGui::DragFloat(label, &candidate, speed, minimum, maximum);
					value = Value::FromFloat(candidate);
					break;
				}
				case FieldType::Vec2:
				{
					glm::vec2 candidate = value.AsVec2();
					changed = ImGui::DragFloat2(label, &candidate.x, speed, minimum, maximum);
					value = Value::FromVec2(candidate);
					break;
				}
				case FieldType::Vec3:
				case FieldType::Color3:
				{
					glm::vec3 candidate = value.AsVec3();
					changed = kind == FieldType::Color3 ? ImGui::ColorEdit3(label, &candidate.x) : ImGui::DragFloat3(label, &candidate.x, speed, minimum, maximum);
					value = kind == FieldType::Color3 ? Value::FromColor3(candidate) : Value::FromVec3(candidate);
					break;
				}
				case FieldType::Vec4:
				case FieldType::Color4:
				{
					glm::vec4 candidate = value.AsVec4();
					changed = kind == FieldType::Color4 ? ImGui::ColorEdit4(label, &candidate.x) : ImGui::DragFloat4(label, &candidate.x, speed, minimum, maximum);
					value = kind == FieldType::Color4 ? Value::FromColor4(candidate) : Value::FromVec4(candidate);
					break;
				}
				case FieldType::Quat:
				{
					const glm::quat current = value.AsQuat();
					std::array candidate{ current.x, current.y, current.z, current.w };
					changed = ImGui::DragFloat4(label, candidate.data(), speed, minimum, maximum);
					value = Value::FromQuat(glm::quat(candidate[3], candidate[0], candidate[1], candidate[2]));
					break;
				}
				case FieldType::Bool3:
				{
					glm::bvec3 candidate = value.AsBool3();
					ReflectedDrawerResult result;
					ImGui::TextUnformatted(label);
					for (int i = 0; i < 3; ++i)
					{
						ImGui::SameLine();
						ImGui::PushID(i);
						const bool toggled = ImGui::Checkbox(i == 0 ? "X" : i == 1 ? "Y"
																				   : "Z",
							&candidate[i]);
						MergeDrawerResult(result, DrawerItemResult(toggled, true));
						ImGui::PopID();
					}
					value = Value::FromBool3(candidate);
					return result;
				}
				case FieldType::String:
				{
					std::string candidate = value.AsString();
					changed = ImGui::InputText(label, &candidate);
					value = Value::FromString(std::move(candidate));
					break;
				}
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				{
					std::string candidate = value.AsUUID().ToString();
					changed = ImGui::InputText(label, &candidate, ImGuiInputTextFlags_EnterReturnsTrue);
					if (changed)
					{
						const auto parsed = UUID::FromString(candidate);
						if (!parsed)
							return MakeError(ErrorCode::Validation, "enter a 16-digit UUID");
						const UUID id = *parsed;
						value = kind == FieldType::EntityRef ? Value::FromEntityRef(id) : Value::FromAssetRef(id);
					}
					ReflectedDrawerResult result = DrawerItemResult(changed, true);
					std::optional<Error> dropError;
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kind == FieldType::EntityRef ? "ENGINE_ENTITY" : "ENGINE_ASSET"))
						{
							std::optional<UUID> id;
							if (kind == FieldType::AssetRef && payload->Data != nullptr && payload->DataSize == UUID::TextLength + 1)
							{
								const char* text = static_cast<const char*>(payload->Data);
								if (text[UUID::TextLength] == '\0')
								{
									const std::string_view token(text, UUID::TextLength);
									id = UUID::FromString(token);
									if (id && (!id->IsValid() || id->ToString() != token))
										id.reset();
								}
							}
							else if (kind == FieldType::EntityRef && payload->Data != nullptr && payload->DataSize == sizeof(UUID))
							{
								UUID entity;
								std::memcpy(&entity, payload->Data, sizeof(entity));
								if (entity.IsValid())
									id = entity;
							}
							if (!id)
								dropError = Error(ErrorCode::Validation, kind == FieldType::AssetRef ? "asset drop requires a canonical 16-digit nonzero UUID followed by NUL" : "entity drop requires a nonzero UUID");
							else if (!context.ReadOnly && !field.IsReadOnly())
							{
								value = kind == FieldType::EntityRef ? Value::FromEntityRef(*id) : Value::FromAssetRef(*id);
								result.Activated = result.Changed = result.Committed = true;
							}
						}
						ImGui::EndDragDropTarget();
					}
					if (dropError)
						return std::unexpected(*dropError);
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear"))
					{
						value = kind == FieldType::EntityRef ? Value::FromEntityRef({}) : Value::FromAssetRef({});
						result.Activated = result.Changed = result.Committed = true;
					}
					ENGINE_TRY(DrawInspectorReferenceSearch(field, value, context, result));
					return result;
				}
				case FieldType::Enum:
				{
					std::string preview = value.ToString();
					for (const auto& entry : field.GetType().GetEnum()->GetEntries())
					{
						if (entry.Value == value.AsEnum())
							preview = entry.Name;
					}
					if (ImGui::BeginCombo(label, context.Mixed ? "Multiple values" : preview.c_str()))
					{
						for (const auto& entry : field.GetType().GetEnum()->GetEntries())
						{
							if (ImGui::Selectable(entry.Name.c_str(), entry.Value == value.AsEnum()))
							{
								value = Value::FromEnum(entry.Value);
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
					return ReflectedDrawerResult{ .Activated = changed, .Changed = changed, .Committed = changed };
				}
				case FieldType::Array:
				case FieldType::Map:
				case FieldType::Struct:
					return DrawInspectorContainer(field, value, context);
				case FieldType::Variant:
				{
					auto resolved = field.ResolveVariant(context.Resolve);
					if (!resolved)
					{
						ImGui::TextWrapped("%s: %s", label, value.AsVariant().Get().dump().c_str());
						ImGui::TextDisabled("Unresolved: %s", resolved.error().GetMessageText().c_str());
						return ReflectedDrawerResult{};
					}
					ENGINE_TRY_ASSIGN(Value typed, ValueFromJson(JsonReader(value.AsVariant().Get()), (*resolved)->GetType()));
					ENGINE_TRY_ASSIGN(auto result, DrawReflectedValue(**resolved, typed, context));
					if (result.Changed)
					{
						ENGINE_TRY_ASSIGN(Json json, ValueToJson(typed, (*resolved)->GetType()));
						value = Value::FromVariant(VariantValue(std::move(json)));
					}
					return result;
				}
			}
			return DrawerItemResult(changed, immediate);
		}

	}

	Result<ReflectedDrawerResult> DrawReflectedValue(const FieldInfo& field, Value& value, const ReflectedDrawerContext& context)
	{
		if (value.IsNull() || value.GetKind() != field.GetKind())
			return MakeError(ErrorCode::Validation, "drawer value does not match field '{}'", field.GetName());
		ImGui::PushID(context.Path.c_str());
		ImGui::BeginDisabled(context.ReadOnly || field.IsReadOnly());
		if (context.Mixed)
			ImGui::TextDisabled("Multiple values");
		Value candidate = value;
		auto result = Utils::DrawInspectorScalar(field, candidate, context);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s%s%s", field.GetDescription().c_str(), field.GetMeta().Unit.empty() ? "" : "\nUnit: ", field.GetMeta().Unit.c_str());
		ImGui::EndDisabled();
		ImGui::PopID();
		if (!result)
			return result;
		if (result->Changed && !context.ReadOnly && !field.IsReadOnly())
		{
			ValidationContext validation;
			field.ValidateValue(candidate, context.Resolve, validation);
			ENGINE_TRY(validation.ToStatus(field.GetName()));
			value = std::move(candidate);
		}
		return result;
	}

}
