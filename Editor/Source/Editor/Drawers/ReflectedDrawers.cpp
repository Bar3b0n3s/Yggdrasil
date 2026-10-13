#include "EditorPCH.h"
#include "Editor/Drawers/ReflectedDrawers.h"

#include "Editor/Ui/EditorStyle.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/TransformSystem.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace Engine {

	namespace Utils {

		static void MergeDrawerResult(ReflectedDrawerResult& into, const ReflectedDrawerResult& child)
		{
			into.Activated |= child.Activated;
			into.Changed |= child.Changed;
			into.Committed |= child.Committed;
			into.Cancelled |= child.Cancelled;
			into.Active |= child.Active;
		}

		static ReflectedDrawerResult DrawerItemResult(bool changed, bool immediate = false)
		{
			return { .Activated = ImGui::IsItemActivated(), .Changed = changed, .Committed = ImGui::IsItemDeactivatedAfterEdit() || (changed && immediate), .Cancelled = (ImGui::IsItemActive() || ImGui::IsItemDeactivated()) && ImGui::IsKeyPressed(ImGuiKey_Escape), .Active = ImGui::IsItemActive() };
		}

		static std::string DrawerLabel(const FieldInfo& field, const ReflectedDrawerContext& context)
		{
			return context.DisplayLabel.empty() ? EditorLabel(field.GetName()) : context.DisplayLabel;
		}

		static void DrawerTooltip(const FieldInfo& field)
		{
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s%s%s%s", field.GetDescription().c_str(), field.GetMeta().Unit.empty() ? "" : "\nUnit: ",
					field.GetMeta().Unit.c_str(), field.IsReadOnly() ? "\nRead only" : "");
		}

		// Seven significant digits show any authored float without float noise, and typed text round-trips what is shown.
		// Drags never round the stored value to this format (ImGuiSliderFlags_NoRoundToFormat) and clamp typed entries.
		constexpr const char* InspectorFloatFormat = "%.7g";
		constexpr ImGuiSliderFlags InspectorFloatFlags = ImGuiSliderFlags_NoRoundToFormat | ImGuiSliderFlags_AlwaysClamp;

		static bool IsComponentMixed(bool mixed, uint32_t mixedComponents, int component)
		{
			return mixed && (mixedComponents == 0 || (mixedComponents & (1u << static_cast<uint32_t>(component))) != 0);
		}

		// -0 equals 0: show one zero. Only the drawn copy changes, so nothing is written without an edit.
		static float DisplayedZero(float value)
		{
			return value == 0.0f ? 0.0f : value;
		}

		// FieldMeta::MinMagnitude excludes (-m, m): an edit that lands inside it steps to the bound on its side, or on the
		// previous value's side when it lands on zero exactly, so dragging through zero never produces an invalid value.
		static float StepOverMinMagnitude(float value, float previous, const FieldMeta& meta)
		{
			if (!meta.MinMagnitude.has_value())
				return value;
			const float magnitude = static_cast<float>(*meta.MinMagnitude);
			if (!(std::fabs(value) < magnitude))
				return value;
			const bool negative = value != 0.0f ? value < 0.0f : previous < 0.0f;
			return negative ? -magnitude : magnitude;
		}

		template<typename TInteger>
		static TInteger StepOverMinMagnitude(TInteger value, TInteger previous, const FieldMeta& meta)
		{
			if (!meta.MinMagnitude.has_value())
				return value;
			const double magnitude = std::ceil(*meta.MinMagnitude);
			if (!(std::fabs(static_cast<double>(value)) < magnitude))
				return value;
			const bool negative = value != 0 ? value < 0 : previous < 0;
			if constexpr (std::is_signed_v<TInteger>)
			{
				if (negative)
					return static_cast<TInteger>(std::max(-magnitude, static_cast<double>(std::numeric_limits<TInteger>::min())));
			}
			return static_cast<TInteger>(std::min(magnitude, static_cast<double>(std::numeric_limits<TInteger>::max())));
		}

		// The integer bounds that FieldMeta's inclusive Min and Max admit, within the type's range.
		template<typename TInteger>
		static std::pair<TInteger, TInteger> IntegerRange(const FieldMeta& meta)
		{
			constexpr double Lowest = static_cast<double>(std::numeric_limits<TInteger>::min());
			constexpr double Highest = static_cast<double>(std::numeric_limits<TInteger>::max());
			const double low = meta.Min.has_value() ? std::clamp(std::ceil(*meta.Min), Lowest, Highest) : Lowest;
			const double high = meta.Max.has_value() ? std::clamp(std::floor(*meta.Max), Lowest, Highest) : Highest;
			return { static_cast<TInteger>(low), static_cast<TInteger>(high) };
		}

		static ReflectedDrawerResult DrawInspectorVector(float* values, int count, float speed, float minimum, float maximum,
			const FieldInfo& field, bool mixed, uint32_t mixedComponents)
		{
			ReflectedDrawerResult result;
			const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
			const float available = ImGui::GetContentRegionAvail().x;
			const float minimumAxisWidth = ImGui::CalcTextSize("X -1.23").x + ImGui::GetStyle().FramePadding.x * 2.0f;
			const bool inlineAxes = available >= minimumAxisWidth * static_cast<float>(count) + spacing * static_cast<float>(count - 1);
			const float width = inlineAxes ? (available - spacing * static_cast<float>(count - 1)) / static_cast<float>(count) : available;
			constexpr std::array<const char*, 4> Formats{ "X %.7g", "Y %.7g", "Z %.7g", "W %.7g" };
			ImGui::BeginGroup();
			for (int axis = 0; axis < count; ++axis)
			{
				if (axis > 0 && inlineAxes)
					ImGui::SameLine(0.0f, spacing);
				ImGui::PushID(axis);
				ImGui::SetNextItemWidth(std::max(1.0f, width));
				const float previous = values[axis];
				values[axis] = DisplayedZero(values[axis]);
				const bool changed = ImGui::DragFloat("##Axis", &values[axis], speed, minimum, maximum,
					IsComponentMixed(mixed, mixedComponents, axis) ? "--" : Formats[static_cast<size_t>(axis)], InspectorFloatFlags);
				if (changed)
				{
					values[axis] = StepOverMinMagnitude(values[axis], previous, field.GetMeta());
					result.ChangedComponents |= 1u << static_cast<uint32_t>(axis);
				}
				MergeDrawerResult(result, DrawerItemResult(changed));
				DrawerTooltip(field);
				ImGui::PopID();
			}
			ImGui::EndGroup();
			return result;
		}

		static bool IsUsableRotation(const glm::quat& rotation)
		{
			const float lengthSquared = glm::dot(rotation, rotation);
			return std::isfinite(lengthSquared) && lengthSquared > 0.0f;
		}

		// A rotation edits as Euler degrees in Transform.EulerAngles' convention (TransformSystem, X in [-90, 90]), the way
		// people author it; the raw components stay available under a disclosure and are renormalized after an edit.
		// Either edit produces a unit quaternion, so it passes the field's unit-length validation. The whole rotation is the
		// edited value, also in a multi-edit.
		static Result<ReflectedDrawerResult> DrawInspectorRotation(const FieldInfo& field, Value& value, const ReflectedDrawerContext& context)
		{
			const glm::quat current = value.AsQuat();
			const float eulerSpeed = static_cast<float>(field.GetMeta().Step.value_or(0.5));
			// While a gesture owns the angles, keep the angles being edited: re-deriving them from the quaternion every frame
			// would flip X past +-90 degrees mid-drag. A kept value is used only while it still describes the rotation.
			ImGuiStorage* storage = ImGui::GetStateStorage();
			const std::array<ImGuiID, 4> keys{ ImGui::GetID("##EulerGesture"), ImGui::GetID("##EulerX"), ImGui::GetID("##EulerY"), ImGui::GetID("##EulerZ") };
			glm::vec3 euler = IsUsableRotation(current) ? TransformSystem::EulerDegreesFromQuaternion(glm::normalize(current)) : glm::vec3(0.0f);
			if (storage->GetBool(keys[0]))
			{
				const glm::vec3 kept(storage->GetFloat(keys[1]), storage->GetFloat(keys[2]), storage->GetFloat(keys[3]));
				if (IsUsableRotation(current) && std::fabs(glm::dot(TransformSystem::QuaternionFromEulerDegrees(kept), glm::normalize(current))) > 1.0f - 1e-5f)
					euler = kept;
			}
			ReflectedDrawerResult result = DrawInspectorVector(&euler.x, 3, eulerSpeed, -std::numeric_limits<float>::max(),
				std::numeric_limits<float>::max(), field, context.Mixed, 0);
			result.ChangedComponents = 0;
			if (result.Changed)
				value = Value::FromQuat(TransformSystem::QuaternionFromEulerDegrees(euler));
			storage->SetBool(keys[0], result.Active);
			for (size_t axis = 0; axis < 3; ++axis)
				storage->SetFloat(keys[axis + 1], euler[static_cast<glm::length_t>(axis)]);
			if (ImGui::TreeNodeEx("##Components", ImGuiTreeNodeFlags_SpanAvailWidth, "Quaternion components"))
			{
				std::array components{ current.x, current.y, current.z, current.w };
				ReflectedDrawerResult raw = DrawInspectorVector(components.data(), 4, 0.01f, -1.0f, 1.0f, field, context.Mixed, 0);
				ImGui::TreePop();
				if (raw.Changed)
				{
					const glm::quat edited(components[3], components[0], components[1], components[2]);
					if (!IsUsableRotation(edited))
						return MakeError(ErrorCode::Validation, "a rotation must be a non-zero, finite quaternion");
					value = Value::FromQuat(glm::normalize(edited));
				}
				MergeDrawerResult(result, raw);
			}
			return result;
		}

		static Result<Value> DefaultDrawerValue(const TypeInfo& type, const ScriptFieldSchema* schema)
		{
			if (schema != nullptr)
				return ValueFromJson(JsonReader(schema->DefaultValue.Get()), type);
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
			const float popupWidth = std::min(ImGui::GetFontSize() * 30.0f, ImGui::GetIO().DisplaySize.x - ImGui::GetStyle().WindowPadding.x * 2.0f);
			ImGui::SetNextWindowSize(ImVec2(popupWidth, 0.0f), ImGuiCond_Appearing);
			if (!ImGui::BeginPopup("ReferenceSearch", ImGuiWindowFlags_AlwaysAutoResize))
				return {};

			// The popup owns its query bytes in ImGui storage. No borrowed owner or heap pointer survives Draw.
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
			Status status;
			if (context.FindReferences)
			{
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::InputTextWithHint("##Query", "Search resources by name or path", query.data(), query.size()))
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
					status = std::unexpected(std::move(candidates).error());
				}
				else
				{
					if (candidates->empty())
						ImGui::TextDisabled("No matching resources.");
					if (ImGui::BeginChild("Results", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f)))
					{
						for (const auto& candidate : *candidates)
						{
							const std::string id = candidate.Id.ToString();
							ImGui::PushID(id.c_str());
							ImGui::BeginDisabled(!candidate.Id.IsValid());
							const ImVec2 position = ImGui::GetCursorScreenPos();
							const float lineHeight = ImGui::GetTextLineHeight();
							const bool selected = ImGui::Selectable("##Candidate", !context.Mixed && candidate.Id == value.AsUUID(), 0,
								ImVec2(0.0f, lineHeight * 2.0f + ImGui::GetStyle().ItemSpacing.y));
							// Authored names (including ##) are text, never widget identities. Clip long paths to the row.
							ImGui::GetWindowDrawList()->PushClipRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true);
							ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), candidate.Label.c_str());
							ImGui::GetWindowDrawList()->AddText(ImVec2(position.x, position.y + lineHeight), ImGui::GetColorU32(ImGuiCol_TextDisabled), candidate.Path.c_str());
							ImGui::GetWindowDrawList()->PopClipRect();
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("%s\n%s\n%s", candidate.Label.c_str(), candidate.Path.c_str(), id.c_str());
							ImGui::EndDisabled();
							ImGui::PopID();
							if (selected)
							{
								value = field.GetKind() == FieldType::EntityRef ? Value::FromEntityRef(candidate.Id) : Value::FromAssetRef(candidate.Id);
								result.Activated = result.Changed = result.Committed = true;
								ImGui::CloseCurrentPopup();
								break;
							}
						}
					}
					ImGui::EndChild();
				}
			}
			if (ImGui::CollapsingHeader("Paste reference ID", context.FindReferences ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::TextWrapped("Enter a 16-digit reference ID. Use Clear to remove the reference.");
				std::string candidate = value.AsUUID().ToString();
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::InputText("##ReferenceId", &candidate, ImGuiInputTextFlags_EnterReturnsTrue))
				{
					const auto id = UUID::FromString(candidate);
					if (!id)
						status = MakeError(ErrorCode::Validation, "enter a 16-digit reference ID");
					else
					{
						value = field.GetKind() == FieldType::EntityRef ? Value::FromEntityRef(*id) : Value::FromAssetRef(*id);
						result.Activated = result.Changed = result.Committed = true;
						ImGui::CloseCurrentPopup();
					}
				}
			}
			ImGui::EndPopup();
			return status;
		}

		static Result<ReflectedDrawerResult> DrawInspectorContainer(const FieldInfo& field, Value& value, const ReflectedDrawerContext& context)
		{
			ReflectedDrawerResult result;
			const std::string label = DrawerLabel(field, context);
			if (!ImGui::TreeNodeEx("##Container", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label.c_str()))
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
				ImGui::PushID(static_cast<int>(FNV1a32(key)));
				ReflectedDrawerContext child = context;
				// A container replaces its whole value; a parent's component mask never describes a child.
				child.MixedComponents = 0;
				child.DisplayLabel = isStruct ? "" : isMap ? key
														   : "Element " + std::to_string(i + 1);
				child.ScriptSchema = context.ScriptSchema == nullptr ? nullptr : context.ScriptSchema->Element.get();
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
					const FieldInfo* elementSchema = field.GetType().GetElementSchema();
					const FieldInfo& descriptor = elementSchema != nullptr ? *elementSchema : field;
					FieldMeta childMeta = descriptor.GetMeta();
					const TypeInfo* elementType = field.GetType().GetElement();
					if (elementType->GetKind() == FieldType::AssetRef)
						childMeta.AssetFilter = elementType->GetAssetTypeName();
					synthetic = CreateScope<FieldInfo>(FieldInfo::Specification{ .Name = key, .Description = descriptor.GetDescription(), .Type = elementType, .Meta = std::move(childMeta), .Accessor = {}, .Resolver = descriptor.GetResolver() });
					childField = synthetic.get();
					if (isMap)
						child.Resolve.Key = key;
				}
				if (isMap)
				{
					std::string renamed = key;
					ImGui::SetNextItemWidth(-1.0f);
					if (ImGui::InputText("##Key", &renamed, ImGuiInputTextFlags_EnterReturnsTrue) && renamed != key)
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
					remove = ImGui::SmallButton("Remove entry");
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
				auto added = DefaultDrawerValue(*field.GetType().GetElement(), context.ScriptSchema == nullptr ? nullptr : context.ScriptSchema->Element.get());
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
			constexpr const char* Label = "##Value";
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
					ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, context.Mixed);
					changed = ImGui::Checkbox(Label, &candidate);
					ImGui::PopItemFlag();
					value = Value::FromBool(candidate);
					immediate = true;
					break;
				}
				case FieldType::Int32:
				{
					const int32_t previous = value.AsInt32();
					int32_t candidate = previous;
					const auto [low, high] = IntegerRange<int32_t>(field.GetMeta());
					changed = ImGui::DragScalar(Label, ImGuiDataType_S32, &candidate, speed, &low, &high, nullptr, ImGuiSliderFlags_AlwaysClamp);
					if (changed)
						candidate = StepOverMinMagnitude(candidate, previous, field.GetMeta());
					value = Value::FromInt32(candidate);
					break;
				}
				case FieldType::UInt32:
				{
					const uint32_t previous = value.AsUInt32();
					uint32_t candidate = previous;
					const auto [low, high] = IntegerRange<uint32_t>(field.GetMeta());
					changed = ImGui::DragScalar(Label, ImGuiDataType_U32, &candidate, speed, &low, &high, nullptr, ImGuiSliderFlags_AlwaysClamp);
					if (changed)
						candidate = StepOverMinMagnitude(candidate, previous, field.GetMeta());
					value = Value::FromUInt32(candidate);
					break;
				}
				case FieldType::Float:
				{
					const float previous = value.AsFloat();
					float candidate = DisplayedZero(previous);
					changed = ImGui::DragFloat(Label, &candidate, speed, minimum, maximum, InspectorFloatFormat, InspectorFloatFlags);
					if (changed)
						candidate = StepOverMinMagnitude(candidate, previous, field.GetMeta());
					value = Value::FromFloat(candidate);
					break;
				}
				case FieldType::Vec2:
				{
					glm::vec2 candidate = value.AsVec2();
					const auto result = DrawInspectorVector(&candidate.x, 2, speed, minimum, maximum, field, context.Mixed, context.MixedComponents);
					value = Value::FromVec2(candidate);
					return result;
				}
				case FieldType::Vec3:
				case FieldType::Color3:
				{
					glm::vec3 candidate = value.AsVec3();
					if (kind == FieldType::Vec3)
					{
						const auto result = DrawInspectorVector(&candidate.x, 3, speed, minimum, maximum, field, context.Mixed, context.MixedComponents);
						value = Value::FromVec3(candidate);
						return result;
					}
					changed = ImGui::ColorEdit3(Label, &candidate.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
					value = kind == FieldType::Color3 ? Value::FromColor3(candidate) : Value::FromVec3(candidate);
					break;
				}
				case FieldType::Vec4:
				case FieldType::Color4:
				{
					glm::vec4 candidate = value.AsVec4();
					if (kind == FieldType::Vec4)
					{
						const auto result = DrawInspectorVector(&candidate.x, 4, speed, minimum, maximum, field, context.Mixed, context.MixedComponents);
						value = Value::FromVec4(candidate);
						return result;
					}
					changed = ImGui::ColorEdit4(Label, &candidate.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
					value = kind == FieldType::Color4 ? Value::FromColor4(candidate) : Value::FromVec4(candidate);
					break;
				}
				case FieldType::Quat:
					return DrawInspectorRotation(field, value, context);
				case FieldType::Bool3:
				{
					glm::bvec3 candidate = value.AsBool3();
					ReflectedDrawerResult result;
					for (int i = 0; i < 3; ++i)
					{
						if (i > 0 && ImGui::GetContentRegionAvail().x > ImGui::GetFrameHeight() + ImGui::GetFontSize() * 2.0f)
							ImGui::SameLine();
						ImGui::PushID(i);
						ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, IsComponentMixed(context.Mixed, context.MixedComponents, i));
						const bool toggled = ImGui::Checkbox(i == 0 ? "X" : i == 1 ? "Y"
																				   : "Z",
							&candidate[i]);
						ImGui::PopItemFlag();
						if (toggled)
							result.ChangedComponents |= 1u << static_cast<uint32_t>(i);
						MergeDrawerResult(result, DrawerItemResult(toggled, true));
						ImGui::PopID();
					}
					value = Value::FromBool3(candidate);
					return result;
				}
				case FieldType::String:
				{
					std::string candidate = value.AsString();
					changed = ImGui::InputText(Label, &candidate);
					value = Value::FromString(std::move(candidate));
					break;
				}
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				{
					const UUID currentId = value.AsUUID();
					const auto reference = context.DescribeReference && currentId.IsValid() ? context.DescribeReference(field, currentId) : std::nullopt;
					const std::string name = context.Mixed ? "Multiple values" : reference ? reference->Label
						: currentId.IsValid()                                              ? "Unresolved reference"
																						   : "None";
					const float buttonSize = ImGui::GetFrameHeight();
					const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
					const float available = ImGui::GetContentRegionAvail().x;
					const bool inlineActions = available > buttonSize * 4.0f + spacing * 2.0f;
					const float nameWidth = std::max(1.0f, available - (inlineActions ? buttonSize * 2.0f + spacing * 2.0f : 0.0f));
					const ImVec2 position = ImGui::GetCursorScreenPos();
					if (ImGui::Button("##Reference", ImVec2(nameWidth, buttonSize)))
						ImGui::OpenPopup("ReferenceSearch");
					const ImVec2 padding = ImGui::GetStyle().FramePadding;
					ImGui::GetWindowDrawList()->PushClipRect(ImVec2(position.x + padding.x, position.y),
						ImVec2(position.x + nameWidth - padding.x, position.y + buttonSize), true);
					ImGui::GetWindowDrawList()->AddText(ImVec2(position.x + padding.x, position.y + padding.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
					ImGui::GetWindowDrawList()->PopClipRect();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("%s\n%s\n%s\nDrop a reference here, or click to choose.", name.c_str(),
							reference ? reference->Path.c_str() : field.GetDescription().c_str(), currentId.ToString().c_str());
					ReflectedDrawerResult result;

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
					if (inlineActions)
						ImGui::SameLine(0.0f, spacing);
					ImGui::BeginDisabled(!currentId.IsValid() && !context.Mixed);
					if (ImGui::Button("x##Clear", ImVec2(buttonSize, buttonSize)))
					{
						value = kind == FieldType::EntityRef ? Value::FromEntityRef({}) : Value::FromAssetRef({});
						result.Activated = result.Changed = result.Committed = true;
					}
					ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("Clear reference");
					ImGui::SameLine(0.0f, spacing);
					if (ImGui::Button("...##Browse", ImVec2(buttonSize, buttonSize)))
						ImGui::OpenPopup("ReferenceSearch");
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip("Choose a reference or paste an ID");
					ENGINE_TRY(DrawInspectorReferenceSearch(field, value, context, result));
					return result;
				}
				case FieldType::Enum:
				{
					std::string preview = value.ToString();
					for (const auto& entry : field.GetType().GetEnum()->GetEntries())
					{
						if (entry.Value == value.AsEnum())
							preview = EditorLabel(entry.Name);
					}
					if (ImGui::BeginCombo(Label, context.Mixed ? "Multiple values" : preview.c_str()))
					{
						for (const auto& entry : field.GetType().GetEnum()->GetEntries())
						{
							if (ImGui::Selectable(EditorLabel(entry.Name).c_str(), entry.Value == value.AsEnum()))
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
						ImGui::TextWrapped("%s: %s", DrawerLabel(field, context).c_str(), value.AsVariant().Get().dump().c_str());
						ImGui::TextDisabled("Unresolved: %s", resolved.error().GetMessageText().c_str());
						return ReflectedDrawerResult{};
					}
					ENGINE_TRY_ASSIGN(Value typed, ValueFromJson(JsonReader(value.AsVariant().Get()), (*resolved)->GetType()));
					ReflectedDrawerContext child = context;
					child.DisplayLabel = DrawerLabel(field, context);
					ENGINE_TRY_ASSIGN(auto result, DrawReflectedValue(**resolved, typed, child));
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
		// Hash the bytes, so authored map keys containing ### cannot reset ImGui's string hash.
		ImGui::PushID(static_cast<int>(FNV1a32(context.Path)));
		ImGui::BeginDisabled(context.ReadOnly || field.IsReadOnly());
		if (context.Mixed)
			ImGui::TextDisabled("Multiple values");
		const FieldType kind = field.GetKind();
		const bool scalar = kind != FieldType::Struct && kind != FieldType::Map && kind != FieldType::Array && kind != FieldType::Variant;
		bool table = false;
		if (scalar)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, Utils::EditorUiScale()));
			table = ImGui::BeginTable("Property", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX);
			if (!table)
			{
				ImGui::PopStyleVar();
				ImGui::EndDisabled();
				ImGui::PopID();
				return ReflectedDrawerResult{};
			}
			if (table)
			{
				const bool wideControl = kind == FieldType::Vec2 || kind == FieldType::Vec3 || kind == FieldType::Vec4 || kind == FieldType::Quat
					|| kind == FieldType::Bool3 || kind == FieldType::AssetRef || kind == FieldType::EntityRef;
				const float labelWidth = std::min(ImGui::GetFontSize() * (wideControl ? 7.0f : 9.0f), ImGui::GetContentRegionAvail().x * (wideControl ? 0.28f : 0.40f));
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				std::string label = Utils::DrawerLabel(field, context);
				if (!field.GetMeta().Unit.empty())
					label += " (" + field.GetMeta().Unit + ")";
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(label.c_str());
				ImGui::PopTextWrapPos();
				Utils::DrawerTooltip(field);
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(-1.0f);
			}
		}
		Value candidate = value;
		auto result = Utils::DrawInspectorScalar(field, candidate, context);
		Utils::DrawerTooltip(field);
		if (table)
		{
			ImGui::EndTable();
			ImGui::PopStyleVar();
		}
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
