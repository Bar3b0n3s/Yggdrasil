#include "EditorPCH.h"
#include "EditorCore/Commands/CompositeCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"

// M4 contract stub (Roadmap rule 3): stream A (commands) implements atomic command groups.

namespace Engine {

	CompositeCommand::CompositeCommand(std::string label)
		: m_Label(std::move(label))
	{
	}

	void CompositeCommand::Add(Scope<Command> /*child*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void CompositeCommand::AddExecuted(Scope<Command> /*child*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status CompositeCommand::Execute(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CompositeCommand::Execute is an M4 contract stub");
	}

	Status CompositeCommand::Undo(EditorContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CompositeCommand::Undo is an M4 contract stub");
	}

	bool CompositeCommand::ChangesScene() const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

	size_t CompositeCommand::GetMemorySize() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const Command& CompositeCommand::GetChild(size_t index) const
	{
		ENGINE_CORE_ASSERT(index < m_Children.size(), "CompositeCommand::GetChild index {} out of range", index);
		return *m_Children[index];
	}

}
