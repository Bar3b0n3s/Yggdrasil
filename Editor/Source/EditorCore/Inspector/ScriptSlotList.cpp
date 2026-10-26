#include "EditorPCH.h"
#include "EditorCore/Inspector/ScriptSlotList.h"

#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace Utils {

		static const char* ScriptSlotChangeLabel(ScriptSlotChange change)
		{
			switch (change)
			{
				case ScriptSlotChange::Append:   return "Add script";
				case ScriptSlotChange::Remove:   return "Remove script";
				case ScriptSlotChange::MoveUp:   return "Move script up";
				case ScriptSlotChange::MoveDown: return "Move script down";
			}
			return "Change scripts";
		}

		// `slots` with `change` applied at `slot`. Errors: InvalidArgument naming the entity for a slot it does not have.
		static Status ChangeScriptSlots(Json& slots, ScriptSlotChange change, size_t slot, std::string_view entity)
		{
			if (change == ScriptSlotChange::Append)
			{
				slots.push_back(Json::object());
				return {};
			}
			const bool valid = slot < slots.size() && (change != ScriptSlotChange::MoveUp || slot > 0)
				&& (change != ScriptSlotChange::MoveDown || slot + 1 < slots.size());
			if (!valid)
				return MakeError(ErrorCode::InvalidArgument, "'{}' has {} script slots, so its slot {} cannot be {}", entity, slots.size(), slot,
					change == ScriptSlotChange::Remove ? "removed" : "moved that way");
			if (change == ScriptSlotChange::Remove)
				slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(slot));
			else
				std::swap(slots[slot], slots[change == ScriptSlotChange::MoveUp ? slot - 1 : slot + 1]);
			return {};
		}

	}

	Result<Json> MakeScriptSlotChange(const Scene& scene, std::span<const UUID> entities, ScriptSlotChange change, size_t slot, SceneTarget target)
	{
		Json ops = Json::array();
		for (const UUID id : entities)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid())
				return MakeError(ErrorCode::NotFound, "entity {} is not in the scene", id.ToString());
			const std::string path = scene.GetEntityPath(entity);
			Result<Json> component = ComponentAccess::GetComponentJson(entity, "Script");
			if (!component)
				return std::unexpected(std::move(component).error().WithContext(std::format("changing the script slots of '{}'", path)));
			const auto found = component->find("Scripts");
			if (found == component->end() || !found->is_array())
				return MakeError(ErrorCode::InvalidArgument, "'{}' has no script slot list", path);
			Json slots = *found;
			ENGINE_TRY(Utils::ChangeScriptSlots(slots, change, slot, path));
			ops.push_back(Json{ { "method", "entity.update" },
				{ "params", Json{ { "entity", id.ToString() }, { "components", Json{ { "Script", Json{ { "Scripts", std::move(slots) } } } } }, { "target", target == SceneTarget::Play ? "Play" : "Edit" } } } });
		}
		return Json{ { "label", Utils::ScriptSlotChangeLabel(change) }, { "ops", std::move(ops) } };
	}

}
