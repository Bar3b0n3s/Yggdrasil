#include "EnginePCH.h"
#include "Engine/Scripting/Private/ScriptSlots.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/ScriptComponent.h"

namespace Engine {

	namespace Detail {

		uint32_t GetScriptSlotCount(const ScriptComponent& component)
		{
			return static_cast<uint32_t>(component.Scripts.size());
		}

		ScriptSlotView GetScriptSlot(const ScriptComponent& component, uint32_t slot)
		{
			ENGINE_CORE_ASSERT(slot < GetScriptSlotCount(component), "Script slot {} is out of range", slot);
			const ScriptSlot& entry = component.Scripts[slot];
			return { .Script = entry.Script.GetHandle(), .Fields = &entry.Fields, .ExecutionOrder = entry.ExecutionOrder };
		}

	}

}
