#include "EditorPCH.h"
#include "EditorCore/Automation/Private/MethodSupport.h"

#include "EditorCore/Automation/ProjectMethods.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "EditorCore/Private/OpenScenes.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/NativeComponentName.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/Value.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		std::string ToProjectRelative(const VfsPath& path)
		{
			return std::string(path.GetPath());
		}

		ProjectSummary MakeProjectSummary(const EditorContext& editor)
		{
			const LoadedProject& project = editor.GetProject();
			ProjectSummary summary;
			summary.Name = project.GetSettings().Name;
			summary.Root = FileSystem::PathToUtf8(project.GetRoot());
			summary.ProjectFile = FileSystem::PathToUtf8(project.GetProjectFile());
			summary.ReadOnly = editor.IsReadOnly();
			return summary;
		}

		SceneSummary MakeSceneSummary(const EditorContext& editor)
		{
			ENGINE_ASSERT(editor.HasScene(), "MakeSceneSummary needs an open scene");
			const Scene& scene = editor.GetScene();
			SceneSummary summary;
			summary.Path = editor.GetScenePath().has_value() ? ToProjectRelative(*editor.GetScenePath()) : std::string();
			summary.Name = scene.GetName();
			summary.Revision = ToAutomationCounter(editor.GetRevision());
			summary.Dirty = editor.IsSceneDirty();
			summary.EntityCount = ToAutomationCounter(scene.GetEntityCount());
			return summary;
		}

		Status SaveOpenScene(EditorContext& editor, const VfsPath& path)
		{
			if (!editor.HasScene())
				return MakeError(ErrorCode::InvalidState, "no scene open; call scene.open or scene.new first");
			if (editor.IsReadOnly())
				return MakeError(ErrorCode::PermissionDenied, "the editor is read-only (--read-only): it cannot write '{}'", path.ToString());

			ENGINE_TRY_ASSIGN(const std::string text, WithContext(SceneSerializer::SaveToString(editor.GetScene()), std::format("while saving the scene to '{}'", ToProjectRelative(path))));
			ENGINE_TRY(editor.WriteProjectFile(path, std::as_bytes(std::span(text.data(), text.size()))));
			return editor.MarkSceneSaved(path);
		}

		Result<VfsPath> GetOwnScenePath(const EditorContext& editor)
		{
			if (!editor.HasScene())
				return MakeError(ErrorCode::InvalidState, "no scene open; call scene.open or scene.new first");
			if (!editor.GetScenePath().has_value())
			{
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("the open scene '{}' was never saved", editor.GetScene().GetName()))
						.WithHint("save it first with scene.save {path: \"Assets/Scenes/<Name>.scene\"}"));
			}
			return *editor.GetScenePath();
		}

		Status CheckDirtyScene(const EditorContext& editor, bool save, bool discard, std::string_view discardFlag)
		{
			if (!editor.HasScene() || !editor.IsSceneDirty() || discard)
				return {};
			if (save)
			{
				ENGINE_TRY(GetOwnScenePath(editor));
				return {};
			}
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("the open scene '{}' has unsaved changes", editor.GetScene().GetName()))
					.WithHint(std::format("pass save: true to write it first, or {}: true to drop the changes", discardFlag)));
		}

		Status ResolveDirtyScene(EditorContext& editor, bool save, bool discard, std::string_view discardFlag)
		{
			ENGINE_TRY(CheckDirtyScene(editor, save, discard, discardFlag));
			if (!editor.HasScene() || !editor.IsSceneDirty() || discard)
				return {};
			ENGINE_TRY_ASSIGN(const VfsPath path, GetOwnScenePath(editor));
			return SaveOpenScene(editor, path);
		}

		Result<std::vector<std::string>> SaveDirtyOpenScenes(EditorContext& editor)
		{
			const std::vector<OpenSceneState> scenes = ListDirtyOpenScenes(editor);
			for (const OpenSceneState& scene : scenes)
			{
				if (scene.Path.has_value())
					continue;
				if (scene.Addressed)
					return std::unexpected(GetOwnScenePath(editor).error());
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("the open scene '{}' (open scene {}) was never saved", scene.Name, scene.Id))
						.WithHint(std::format("save it first with scene.save {{scene: {}, path: \"Assets/Scenes/<Name>.scene\"}}", scene.Id)));
			}
			std::vector<std::string> saved;
			for (const OpenSceneState& scene : scenes)
			{
				ENGINE_TRY_ASSIGN(const Scope<EditorSceneTargetScope> addressed, AddressOpenScene(editor, scene));
				ENGINE_TRY(SaveOpenScene(editor, *scene.Path));
				saved.push_back(ToProjectRelative(*scene.Path));
			}
			return saved;
		}

		Status CheckNoDirtyOpenScenes(const EditorContext& editor, std::string_view hint)
		{
			const std::vector<OpenSceneState> scenes = ListDirtyOpenScenes(editor);
			if (scenes.empty())
				return {};
			const std::string message = scenes.size() == 1 ? std::format("the open scene {} has unsaved changes", FormatOpenSceneNames(scenes))
														   : std::format("the open scenes {} have unsaved changes", FormatOpenSceneNames(scenes));
			return std::unexpected(Error(ErrorCode::InvalidState, message).WithHint(std::string(hint)));
		}

		Status LoadSceneFile(const EditorContext& editor, Scene& scene, const VfsPath& path, LoadMode mode, UUIDGenerator* repairIds,
			LoadReport& report)
		{
			LoadOptions options;
			options.Mode = mode;
			options.RepairIdGenerator = mode == LoadMode::Repair ? repairIds : nullptr;
			options.SourcePath = ToProjectRelative(path);
			// Script field values resolve against the declared fields, so a valid value reports nothing and only an unknown or
			// mismatching one is a diagnostic (ADR 0026). --strict (EditorContextSpecification::StrictUnknowns): unknown keys,
			// fields and components, and those script field values, are errors.
			ENGINE_TRY_ASSIGN(const Ref<const ScriptFieldSchemaSource> schemas, editor.GetScriptSchemaSnapshot());
			options.Schemas = schemas.get();
			options.StrictUnknowns = editor.GetSpecification().StrictUnknowns;
			const Status loaded = SceneSerializer::LoadFromFile(scene, editor.GetVfs(), path, options, report);
			if (!loaded)
				return std::unexpected(ToEditorFileError(loaded.error()));
			return {};
		}

		std::string FormatLoadDiagnostic(std::string_view file, std::string_view pointer, std::string_view message, std::string_view code)
		{
			std::string text = std::format("'{}' {}: {}", file, pointer.empty() ? std::string_view("(root)") : pointer, message);
			if (!code.empty())
				text += std::format(" ({})", code);
			return text;
		}

		void LogLoadDiagnostic(std::string_view file, DiagnosticSeverity severity, std::string_view pointer, std::string_view message, std::string_view code)
		{
			const std::string text = FormatLoadDiagnostic(file, pointer, message, code);
			if (severity == DiagnosticSeverity::Error)
				ENGINE_ERROR("{}", text);
			else
				ENGINE_WARN("{}", text);
		}

		void LogLoadDiagnostics(std::string_view file, const LoadReport& report)
		{
			for (const LoadDiagnostic& diagnostic : report.Diagnostics)
				LogLoadDiagnostic(file, diagnostic.Severity, diagnostic.JsonPointer, diagnostic.Message, diagnostic.Code);
		}

		void CanonicalizeEnumSpellings(Json& value, const TypeInfo& type)
		{
			switch (type.GetKind())
			{
				case FieldType::Enum:
				{
					if (!value.is_string() || type.GetEnum() == nullptr)
						return;
					const Result<std::string> text = JsonReader(value).ReadString();
					if (!text)
						return;
					const EnumEntry* entry = type.GetEnum()->FindByNameIgnoreCase(*text);
					if (entry != nullptr && entry->Name != *text)
						value = entry->Name;
					return;
				}
				case FieldType::Array:
				{
					if (!value.is_array() || type.GetElement() == nullptr)
						return;
					for (Json& element : value)
						CanonicalizeEnumSpellings(element, *type.GetElement());
					return;
				}
				case FieldType::Map:
				{
					if (!value.is_object() || type.GetElement() == nullptr)
						return;
					for (auto member = value.begin(); member != value.end(); ++member)
						CanonicalizeEnumSpellings(member.value(), *type.GetElement());
					return;
				}
				case FieldType::Struct:
				{
					if (!value.is_object() || type.GetStruct() == nullptr)
						return;
					for (auto member = value.begin(); member != value.end(); ++member)
					{
						const FieldInfo* field = type.GetStruct()->FindField(member.key());
						if (field != nullptr)
							CanonicalizeEnumSpellings(member.value(), field->GetType());
					}
					return;
				}
				case FieldType::Bool:
				case FieldType::Int32:
				case FieldType::UInt32:
				case FieldType::Float:
				case FieldType::Vec2:
				case FieldType::Vec3:
				case FieldType::Vec4:
				case FieldType::Quat:
				case FieldType::Color3:
				case FieldType::Color4:
				case FieldType::Bool3:
				case FieldType::String:
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				case FieldType::Variant:
					return;
			}
		}

		Result<const ComponentInfo*> FindWritableComponent(const TypeRegistry& registry, std::string_view name, std::string_view pointer)
		{
			if (IsNativeComponentName(name))
			{
				const Error refusal = MakeNativeComponentWriteError(name);
				return std::unexpected(MakeParamError(refusal.GetCode(), pointer, refusal.GetMessageText(), refusal.GetHint()));
			}
			const ComponentInfo* info = registry.FindComponent(name);
			if (info == nullptr || info->GetHostOps() == nullptr || !info->HasFlag(ComponentFlags::Serializable))
			{
				std::vector<std::string> suggestions = registry.SuggestComponentNames(name);
				return std::unexpected(MakeParamError(ErrorCode::NotFound, pointer, std::format("no component '{}'", name),
					suggestions.empty() ? std::string("component.list lists every component") : MakeDidYouMeanHint(suggestions)));
			}
			if (info->HasFlag(ComponentFlags::EntityLevel))
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is an entity member, not a component", name),
					"set names, tags and active state with the name, tags and active params, and parents with entity.reparent"));
			}
			if (info->HasFlag(ComponentFlags::Hidden))
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is maintained by the engine and cannot be written", name)));
			}
			return info;
		}

		// Writes one component's (partial) JSON `value`: added when `add`, else merged into the existing component (RFC 7386).
		// Stored fields go through ComponentAccess in one write; virtual fields (Transform.EulerAngles, WorldPosition) are set
		// after it through their setters. Errors are located below `pointer`.
		static Status WriteComponent(EditorContext& editor, Entity entity, const ComponentInfo& info, const Json& value, bool add,
			const std::string& pointer, const IFieldSchemaSource* schemas)
		{
			if (!value.is_object())
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("the value of '{}' must be an object of fields", info.GetName())));
			if (info.GetName() == "Script")
			{
				// Every slot's script must be a Behaviour (Docs/Decisions/0031-m20-contract.md decision 17).
				const auto slots = value.find("Scripts");
				for (size_t slot = 0; slots != value.end() && slots->is_array() && slot < slots->size(); ++slot)
				{
					const Json& entry = (*slots)[slot];
					const auto script = entry.is_object() ? entry.find("Script") : entry.end();
					if (!entry.is_object() || script == entry.end() || script->is_null())
						continue;
					const std::string scriptPointer =
						JsonReader::AppendPointer(JsonReader::AppendPointer(JsonReader::AppendPointer(pointer, "Scripts"), slot), "Script");
					const auto handle = UUID::FromString(JsonReader(*script).ReadString().value_or(std::string()));
					if (!handle.has_value())
						return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, scriptPointer, "expected a script asset handle"));
					if (handle->IsValid())
					{
						EditorScriptService* service = editor.GetScriptService();
						if (service == nullptr)
							return std::unexpected(MakeParamError(ErrorCode::InvalidState, scriptPointer, "no project script service is available"));
						const auto data = service->GetFields(*handle);
						if (!data)
							return std::unexpected(LocateAtParam(data.error(), scriptPointer));
						if ((*data)->Kind != ScriptKind::Behaviour)
							return std::unexpected(MakeParamError(ErrorCode::Validation, scriptPointer,
								"SCRIPT_NOT_A_BEHAVIOUR: only a Behaviour script may be attached to an entity"));
					}
				}
			}

			Json stored = Json::object();
			std::vector<std::pair<const FieldInfo*, const Json*>> virtuals;
			for (auto member = value.begin(); member != value.end(); ++member)
			{
				const FieldInfo* field = info.FindField(member.key());
				if (field != nullptr && field->IsVirtual())
					virtuals.emplace_back(field, &member.value());
				else
					stored[member.key()] = member.value();
			}

			const Status written = add ? ComponentAccess::AddComponent(entity, info.GetName(), &stored, schemas)
									   : (stored.empty() ? Status() : ComponentAccess::PatchComponentJson(entity, info.GetName(), stored, schemas));
			if (!written)
				return std::unexpected(PrefixPointers(written.error(), pointer));

			for (const auto& [field, json] : virtuals)
			{
				const std::string fieldPointer = JsonReader::AppendPointer(pointer, field->GetName());
				Result<Value> fieldValue = ValueFromJson(JsonReader(*json), field->GetType());
				if (!fieldValue)
					return std::unexpected(PrefixPointers(fieldValue.error(), fieldPointer));
				const Status set = ComponentAccess::SetFieldValue(entity, info.GetName(), field->GetName(), *fieldValue, schemas);
				if (!set)
					return std::unexpected(PrefixPointers(set.error(), pointer));
			}
			return {};
		}

		Status WriteComponents(EditorContext& editor, Entity entity, const std::map<std::string, VariantValue>& components)
		{
			Ref<const ScriptFieldSchemaSource> schemas;
			if (components.contains("Script"))
			{
				ENGINE_TRY_ASSIGN(schemas, editor.GetScriptSchemaSnapshot());
			}
			const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
			std::vector<std::pair<const ComponentInfo*, const VariantValue*>> additions;
			for (const auto& [name, value] : components)
			{
				const std::string pointer = JsonReader::AppendPointer("/components", name);
				ENGINE_TRY_ASSIGN(const ComponentInfo* info, FindWritableComponent(registry, name, pointer));
				if (info->GetHostOps()->Has(entity))
					ENGINE_TRY(WriteComponent(editor, entity, *info, value.Get(), false, pointer, schemas.get()));
				else
					additions.emplace_back(info, &value);
			}

			std::sort(additions.begin(), additions.end(), [](const auto& left, const auto& right)
			{
				return left.first->GetIndex() < right.first->GetIndex();
			});
			while (!additions.empty())
			{
				const auto isPending = [&additions](const ComponentInfo* component)
				{
					return std::ranges::any_of(additions, [component](const auto& addition)
					{
						return addition.first == component;
					});
				};
				auto ready = std::ranges::find_if(additions, [&isPending](const auto& addition)
				{
					return std::ranges::none_of(addition.first->GetRequires(), isPending);
				});
				if (ready == additions.end())
					ready = additions.begin();
				const std::string pointer = JsonReader::AppendPointer("/components", ready->first->GetName());
				ENGINE_TRY(WriteComponent(editor, entity, *ready->first, ready->second->Get(), true, pointer, schemas.get()));
				additions.erase(ready);
			}
			return {};
		}

		Status CheckPlayEntityCapacity(const AutomationMethodContext& context, const Scene& scene, size_t additional)
		{
			const PlaySession* session = context.GetPlaySession();
			if (session == nullptr || &session->GetScene() != &scene)
				return {};
			return session->CheckEntityCapacity(additional);
		}

		size_t CountSubtreeEntities(std::span<const Entity> roots)
		{
			size_t count = 0;
			std::vector<ConstEntity> pending(roots.begin(), roots.end());
			while (!pending.empty())
			{
				const ConstEntity entity = pending.back();
				pending.pop_back();
				ENGINE_ASSERT(entity.IsValid(), "CountSubtreeEntities needs valid entities");
				++count;
				for (const UUID child : entity.GetChildren())
					pending.push_back(entity.GetScene()->FindEntityByID(child));
			}
			return count;
		}

	}

}
