#include "EditorPCH.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// An estimate of the heap bytes of a settings document: twice its minified size (its strings plus about as much for
		// its nodes). Settings commands are rare and small, so writing the document once per estimate is cheap.
		static size_t EstimateSettingsBytes(const Json& document)
		{
			const Result<std::string> text = JsonWriter::Write(document, JsonStyle::Minified);
			return sizeof(Json) + (text ? text->size() * 2 : 0);
		}

		// StructInfo::ApplyMergePatch erases a null member from the merged document before it reads it, so a null member of
		// `patch` (a merge patch of a value of `type`) that names no serialized field of its struct would never reach the read
		// and would be dropped without a word ({"Window": {"Titel": null}}, a mistyped reset). This turns each such member into
		// an empty object, which the read then reports as unknown like any other member the settings do not have
		// (REFLECTION_UNKNOWN_FIELD with its did-you-mean hint: a warning, or an error in a strict read) and drops. It descends
		// as the merge does: into the members that name a Struct field and into the values of a Map; a Variant value and an
		// array are replaced whole, so a null inside one is a value, not a reset.
		static void ExposeUnknownNullMembers(const TypeInfo& type, Json& patch)
		{
			if (!patch.is_object())
				return;
			if (type.GetKind() == FieldType::Map)
			{
				for (Json& value : patch)
					ExposeUnknownNullMembers(*type.GetElement(), value);
				return;
			}
			if (type.GetKind() != FieldType::Struct)
				return;

			for (auto member = patch.begin(); member != patch.end(); ++member)
			{
				const FieldInfo* field = type.GetStruct()->FindField(member.key());
				const bool serialized = field != nullptr && !field->IsVirtual() && field->GetMeta().Serialized;
				if (!serialized && member->is_null())
					*member = Json::object();
				else if (serialized)
					ExposeUnknownNullMembers(field->GetType(), *member);
			}
		}

	}

	ProjectSettingsCommand::ProjectSettingsCommand(std::string label, Ref<const Json> before, Ref<const Json> after)
		: m_Label(std::move(label)), m_Before(std::move(before)), m_After(std::move(after))
	{
		ENGINE_ASSERT(m_Before != nullptr && m_After != nullptr, "ProjectSettingsCommand '{}' needs both documents", m_Label);
	}

	Result<Scope<ProjectSettingsCommand>> ProjectSettingsCommand::CreateFromPatch(const EditorContext& context, const Json& patch, std::string label)
	{
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "the project settings cannot change: no project is open");
		const JsonReader reader(patch);
		if (!reader.IsObject())
		{
			return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
				std::format("a settings patch must be an object, got {}", JsonTypeToString(reader.GetType()))));
		}

		const TypeRegistry& registry = context.GetTypeRegistry();
		const StructInfo* info = registry.FindStruct<ProjectSettings>();
		ENGINE_VERIFY(info != nullptr, "the editor's type registry lacks the project settings types");
		const ProjectSettings& current = context.GetProject().GetSettings();
		ENGINE_TRY_ASSIGN(Json before, ProjectSerializer::ToJson(current, registry));

		Json exposed = patch;
		Utils::ExposeUnknownNullMembers(info->GetType(), exposed);
		ProjectSettings patched = current;
		std::vector<ValidationIssue> warnings;
		const ReadContext read{ .Schemas = nullptr, .Strict = context.GetSpecification().StrictUnknowns, .Diagnostics = &warnings };
		ENGINE_TRY(info->ApplyMergePatch(&patched, JsonReader(exposed, reader.GetPointer()), read));
		ENGINE_TRY_ASSIGN(Json after, ProjectSerializer::ToJson(patched, registry));
		// The document the command will apply must load as a project file (header and every rule of ProjectSerializer).
		ProjectLoadReport report;
		ENGINE_TRY(ProjectSerializer::FromJson(after, registry, {}, report));
		Scope<ProjectSettingsCommand> command =
			CreateScope<ProjectSettingsCommand>(std::move(label), CreateRef<const Json>(std::move(before)), CreateRef<const Json>(std::move(after)));
		command->m_Warnings = std::move(warnings);
		return command;
	}

	Status ProjectSettingsCommand::Execute(EditorContext& context)
	{
		return context.ApplyProjectSettings(*m_After);
	}

	Status ProjectSettingsCommand::Undo(EditorContext& context)
	{
		return context.ApplyProjectSettings(*m_Before);
	}

	size_t ProjectSettingsCommand::GetMemorySize() const
	{
		size_t warningBytes = m_Warnings.capacity() * sizeof(ValidationIssue);
		for (const ValidationIssue& warning : m_Warnings)
		{
			warningBytes += warning.Code.capacity() + warning.JsonPointer.capacity() + warning.Message.capacity() + warning.Hint.capacity();
			for (const std::string& suggestion : warning.Suggestions)
				warningBytes += sizeof(std::string) + suggestion.capacity();
		}
		return sizeof(ProjectSettingsCommand) + m_Label.capacity() + Utils::EstimateSettingsBytes(*m_Before) + Utils::EstimateSettingsBytes(*m_After) + warningBytes;
	}

}
