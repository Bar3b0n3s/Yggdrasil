#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>

namespace Engine {

	struct ScriptComponent;

	namespace Detail {

		// One slot of a Script component (Docs/Decisions/0031-m20-contract.md decision 17): its behaviour, its field overrides
		// and its ExecutionOrder. A slot without a script creates no instance.
		struct ScriptSlotView
		{
			AssetHandle Script{};
			// Borrowed from the component; valid until the component changes or is removed.
			const std::map<std::string, VariantValue>* Fields = nullptr;
			int32_t ExecutionOrder = 0;
		};

		// The scripting runtime reads a Script component's slots only through these two functions, so every instance, task
		// and error is keyed (entity, slot): the slots are the component's Scripts list, numbered from 0 in list order.
		[[nodiscard]] uint32_t GetScriptSlotCount(const ScriptComponent& component);
		// slot < GetScriptSlotCount(component).
		[[nodiscard]] ScriptSlotView GetScriptSlot(const ScriptComponent& component, uint32_t slot);

	}

}
