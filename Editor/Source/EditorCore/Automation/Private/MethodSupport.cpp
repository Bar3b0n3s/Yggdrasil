#include "EditorPCH.h"
#include "EditorCore/Automation/Private/MethodSupport.h"

#include "EditorCore/Automation/ProjectMethods.h"
#include "EditorCore/Automation/SceneMethods.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <charconv>

namespace Engine {

	namespace Utils {

		// `source` with a new code, message and issues; its contexts, hint and location are kept.
		static Error RebuildError(const Error& source, ErrorCode code, std::string message, std::vector<ErrorIssue> issues)
		{
			Error rebuilt(code, std::move(message));
			for (const std::string& context : source.GetContexts())
			{
				Error next = std::move(rebuilt).WithContext(context);
				rebuilt = std::move(next);
			}
			if (!source.GetHint().empty())
			{
				Error next = std::move(rebuilt).WithHint(source.GetHint());
				rebuilt = std::move(next);
			}
			Error located = std::move(rebuilt).WithLocation(source.GetLocation());
			return std::move(located).WithIssues(std::move(issues));
		}

		Error ReplaceIssues(const Error& error, std::vector<ErrorIssue> issues)
		{
			return RebuildError(error, error.GetCode(), error.GetMessageText(), std::move(issues));
		}

		Error PrefixPointers(const Error& error, std::string_view prefix)
		{
			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = std::string(prefix) + issue.JsonPointer;

			ErrorLocation location;
			location.JsonPointer = std::string(prefix) + error.GetLocation().JsonPointer.value_or(std::string());
			return ReplaceIssues(Error(error).WithLocation(std::move(location)), std::move(issues));
		}

		Error LocateAtParam(const Error& error, std::string_view pointer)
		{
			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
			{
				if (issue.JsonPointer.empty())
					issue.JsonPointer = std::string(pointer);
			}
			ErrorLocation location;
			location.JsonPointer = std::string(pointer);
			return ReplaceIssues(Error(error).WithLocation(std::move(location)), std::move(issues));
		}

		Error MakeParamError(ErrorCode code, std::string_view pointer, std::string message, std::string hint)
		{
			ErrorLocation location;
			location.JsonPointer = std::string(pointer);
			ErrorIssue issue;
			issue.JsonPointer = std::string(pointer);
			issue.Message = message;
			issue.Hint = hint;
			return Error(code, std::move(message)).WithHint(std::move(hint)).WithLocation(std::move(location)).WithIssue(std::move(issue));
		}

		std::string ToProjectRelative(const VfsPath& path)
		{
			return std::string(path.GetPath());
		}

		std::string FormatOptionalUUID(UUID id)
		{
			return id.IsValid() ? id.ToString() : std::string();
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
			editor.MarkSceneSaved(path);
			return {};
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

		Status LoadSceneFile(const EditorContext& editor, Scene& scene, const VfsPath& path, LoadMode mode, UUIDGenerator* repairIds,
			LoadReport& report)
		{
			LoadOptions options;
			options.Mode = mode;
			options.RepairIdGenerator = mode == LoadMode::Repair ? repairIds : nullptr;
			options.SourcePath = ToProjectRelative(path);
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

		std::vector<const ComponentInfo*> GetEntityComponents(ConstEntity entity)
		{
			std::vector<const ComponentInfo*> components;
			for (const ComponentInfo* info : entity.GetScene()->GetTypeRegistry().GetComponents())
			{
				if (info->HasFlag(ComponentFlags::Serializable) && !info->HasFlag(ComponentFlags::EntityLevel) && info->GetHostOps() != nullptr
					&& info->GetHostOps()->Has(entity))
				{
					components.push_back(info);
				}
			}
			return components;
		}

		Result<std::optional<uint64_t>> ParseSequenceCursor(std::string_view cursor)
		{
			if (cursor.empty())
				return std::optional<uint64_t>(0);
			if (cursor == "end")
				return std::optional<uint64_t>();

			uint64_t value = 0;
			const bool digitsOnly = std::all_of(cursor.begin(), cursor.end(), [](char character)
			{
				return character >= '0' && character <= '9';
			});
			const std::from_chars_result parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), value);
			if (!digitsOnly || parsed.ec != std::errc() || parsed.ptr != cursor.data() + cursor.size())
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/cursor",
					std::format("'{}' is not a cursor", cursor), "pass \"\" (the oldest entry), \"end\" or the nextCursor of the previous read"));
			}
			return std::optional<uint64_t>(value);
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
