#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Engine {

	class Scene;

	// A change the inspector makes to the slot list of Script components (Docs/Decisions/0031-m20-contract.md decision 17).
	enum class ScriptSlotChange : uint8_t
	{
		Append,  // an empty slot after the last one, whose script is picked next (appending keeps every slot override's index)
		Remove,  // the slot at the index
		MoveUp,  // the slot at the index swapped with the one before it
		MoveDown // the slot at the index swapped with the one after it
	};

	// The edit.batch params ({label, ops}) that apply `change` at `slot` to the Script component of every entity of `entities`
	// in `scene`, addressed with `target`: one entity.update per entity, writing its whole list (an array is replaced whole,
	// RFC 7386), so the change is one undo step that automation can repeat. Each entity changes its own list by index, its
	// slots keeping their own scripts, fields and orders. `slot` is ignored for Append. The batch writes every value of each
	// list, so entity.update refuses it, changing nothing, while a slot holds a value its script rejects (an unknown name,
	// the wrong type), naming that value. Errors: NotFound when an entity is missing or has no Script component;
	// InvalidArgument when an entity has no slot at `slot` (Remove, MoveUp, MoveDown), or no slot before it (MoveUp) or
	// after it (MoveDown).
	[[nodiscard]] Result<Json> MakeScriptSlotChange(const Scene& scene, std::span<const UUID> entities, ScriptSlotChange change, size_t slot,
		SceneTarget target);

}
