#include "EditorPCH.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/StructInfo.h"
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

		ProjectSettings patched = current;
		ENGINE_TRY(info->ApplyMergePatch(&patched, reader, ReadContext{}));
		ENGINE_TRY_ASSIGN(Json after, ProjectSerializer::ToJson(patched, registry));
		// The document the command will apply must load as a project file (header and every rule of ProjectSerializer).
		ProjectLoadReport report;
		ENGINE_TRY(ProjectSerializer::FromJson(after, registry, {}, report));
		return CreateScope<ProjectSettingsCommand>(std::move(label), CreateRef<const Json>(std::move(before)), CreateRef<const Json>(std::move(after)));
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
		return sizeof(ProjectSettingsCommand) + m_Label.capacity() + Utils::EstimateSettingsBytes(*m_Before) + Utils::EstimateSettingsBytes(*m_After);
	}

}
