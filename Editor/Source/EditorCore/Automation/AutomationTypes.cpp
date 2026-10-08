#include "EditorPCH.h"
#include "EditorCore/Automation/AutomationTypes.h"

#include "EditorCore/Commands/Command.h"
#include "Engine/Reflection/TypeRegistry.h"

namespace Engine {

	void RegisterAutomationCommonTypes(TypeRegistry& registry)
	{
		RegisterAutomationSharedTypes(registry);

		registry.Enum<CommandOrigin>("CommandOrigin", "Who issued an undoable command.")
			.Entry(CommandOrigin::User, "User", "The editor's user, through its panels.")
			.Entry(CommandOrigin::Agent, "Agent", "An automation client; the undo label carries the prefix \"[agent] \".");
	}

}
