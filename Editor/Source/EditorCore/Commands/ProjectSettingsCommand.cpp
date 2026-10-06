#include "EditorPCH.h"
#include "EditorCore/Commands/ProjectSettingsCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"

// M4 contract stub (Roadmap rule 3): stream A (commands) implements the project settings command.

namespace Engine {

	ProjectSettingsCommand::ProjectSettingsCommand(std::string label, Ref<const Json> before, Ref<const Json> after)
		: m_Label(std::move(label)), m_Before(std::move(before)), m_After(std::move(after))
	{
		ENGINE_CORE_ASSERT(m_Before != nullptr && m_After != nullptr, "ProjectSettingsCommand '{}' needs both documents", m_Label);
	}

	Result<Scope<ProjectSettingsCommand>> ProjectSettingsCommand::CreateFromPatch(const EditorContext& /*context*/, const Json& /*patch*/,
		std::string /*label*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSettingsCommand::CreateFromPatch is an M4 contract stub");
	}

	Status ProjectSettingsCommand::Execute(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSettingsCommand::Execute is an M4 contract stub");
	}

	Status ProjectSettingsCommand::Undo(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectSettingsCommand::Undo is an M4 contract stub");
	}

	size_t ProjectSettingsCommand::GetMemorySize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
