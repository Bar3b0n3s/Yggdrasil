#include "EditorPCH.h"
#include "EditorCore/Commands/SceneEditCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Scene.h"

// M4 contract stub (Roadmap rule 3): stream A (commands) implements snapshot undo. The constructor stores the edit, so
// commands can be built and inspected while Execute and Undo are stubs.

namespace Engine {

	SceneEditCommand::SceneEditCommand(std::string label, std::vector<SceneEntityChange> changes, std::string mergeKey)
		: m_Label(std::move(label)), m_Changes(std::move(changes)), m_MergeKey(std::move(mergeKey))
	{
	}

	Status SceneEditCommand::Execute(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneEditCommand::Execute is an M4 contract stub");
	}

	Status SceneEditCommand::Undo(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneEditCommand::Undo is an M4 contract stub");
	}

	bool SceneEditCommand::MergeWith(const Command& /*next*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	size_t SceneEditCommand::GetMemorySize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Status SceneEditCommand::ApplyChanges(Scene& /*scene*/, std::span<const SceneEntityChange> /*changes*/, bool /*after*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SceneEditCommand::ApplyChanges is an M4 contract stub");
	}

}
