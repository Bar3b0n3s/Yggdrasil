#include "EditorPCH.h"
#include "EditorCore/Commands/CompositeCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"

namespace Engine {

	CompositeCommand::CompositeCommand(std::string label)
		: m_Label(std::move(label))
	{
	}

	void CompositeCommand::Add(Scope<Command> child)
	{
		ENGINE_ASSERT(child != nullptr, "CompositeCommand '{}': Add needs a command", m_Label);
		ENGINE_ASSERT(m_AppliedCount == 0, "CompositeCommand '{}' holds executed children; Add appends only unexecuted ones", m_Label);
		m_Children.push_back(std::move(child));
	}

	void CompositeCommand::AddExecuted(Scope<Command> child)
	{
		ENGINE_ASSERT(child != nullptr, "CompositeCommand '{}': AddExecuted needs a command", m_Label);
		ENGINE_ASSERT(m_AppliedCount == m_Children.size(), "CompositeCommand '{}' holds unexecuted children; AddExecuted appends only executed ones",
			m_Label);
		m_Children.push_back(std::move(child));
		++m_AppliedCount;
	}

	Status CompositeCommand::Execute(EditorContext& context)
	{
		const size_t start = m_AppliedCount;
		for (size_t step = start; step < m_Children.size(); ++step)
		{
			Status executed = m_Children[step]->Execute(context);
			if (executed)
			{
				m_AppliedCount = step + 1;
				continue;
			}

			// Undo the children this call executed, newest first, so the editor is as it was before the call.
			Error error = std::move(executed).error().WithContext(std::format("in step {} '{}'", step, m_Children[step]->GetLabel()));
			for (size_t undo = step; undo > start; --undo)
			{
				const size_t index = undo - 1;
				Status undone = m_Children[index]->Undo(context);
				if (undone)
				{
					m_AppliedCount = index;
					continue;
				}
				Error both = std::move(error).WithContext(std::format("and rolling back step {} '{}' failed too, leaving steps 0 to {} applied: {}", index,
					m_Children[index]->GetLabel(), index, undone.error().ToString()));
				ENGINE_ERROR("'{}' failed and could not be rolled back completely: {}", m_Label, both.ToString());
				return std::unexpected(std::move(both));
			}
			return std::unexpected(std::move(error));
		}
		return {};
	}

	Status CompositeCommand::Undo(EditorContext& context)
	{
		const size_t applied = m_AppliedCount;
		for (size_t undo = applied; undo > 0; --undo)
		{
			const size_t step = undo - 1;
			Status undone = m_Children[step]->Undo(context);
			if (undone)
			{
				m_AppliedCount = step;
				continue;
			}

			// Execute the children this call undid again, oldest first, so the composite stays applied.
			Error error = std::move(undone).error().WithContext(std::format("undoing step {} '{}'", step, m_Children[step]->GetLabel()));
			for (size_t redo = step + 1; redo < applied; ++redo)
			{
				Status executed = m_Children[redo]->Execute(context);
				if (executed)
				{
					m_AppliedCount = redo + 1;
					continue;
				}
				Error both = std::move(error).WithContext(std::format("and executing step {} '{}' again failed too, leaving steps 0 to {} applied: {}",
					redo, m_Children[redo]->GetLabel(), redo - 1, executed.error().ToString()));
				ENGINE_ERROR("Undoing '{}' failed and the undone steps could not all be executed again: {}", m_Label, both.ToString());
				return std::unexpected(std::move(both));
			}
			return std::unexpected(std::move(error));
		}
		return {};
	}

	bool CompositeCommand::ChangesScene() const
	{
		return std::any_of(m_Children.begin(), m_Children.end(), [](const Scope<Command>& child)
		{
			return child->ChangesScene();
		});
	}

	size_t CompositeCommand::GetMemorySize() const
	{
		size_t total = 0;
		for (const Scope<Command>& child : m_Children)
			total += child->GetMemorySize();
		return total;
	}

	Status CompositeCommand::ReplayOnSceneCopy(Scene& scene, bool after) const
	{
		if (m_AppliedCount != 0 && m_AppliedCount != m_Children.size())
		{
			return MakeError(ErrorCode::InvalidState, "'{}' has only {} of its {} steps applied, so it cannot be replayed", m_Label, m_AppliedCount,
				m_Children.size());
		}
		for (size_t index = 0; index < m_Children.size(); ++index)
		{
			const size_t step = after ? index : m_Children.size() - 1 - index;
			if (Status replayed = m_Children[step]->ReplayOnSceneCopy(scene, after); !replayed)
				return std::unexpected(std::move(replayed).error().WithContext(std::format("replaying step {} '{}'", step, m_Children[step]->GetLabel())));
		}
		return {};
	}

	const Command& CompositeCommand::GetChild(size_t index) const
	{
		ENGINE_ASSERT(index < m_Children.size(), "CompositeCommand::GetChild index {} out of range", index);
		return *m_Children[index];
	}

}
