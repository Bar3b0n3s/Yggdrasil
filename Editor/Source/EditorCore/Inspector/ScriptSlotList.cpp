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

		// The Script component's slot list of entity `id` (its serialized "Scripts") and the entity's path. Errors: NotFound
		// when the entity is missing or has no Script component; InvalidArgument without a slot list.
		static Result<std::pair<Json, std::string>> ReadScriptSlots(const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid())
				return MakeError(ErrorCode::NotFound, "entity {} is not in the scene", id.ToString());
			std::string path = scene.GetEntityPath(entity);
			Result<Json> component = ComponentAccess::GetComponentJson(entity, "Script");
			if (!component)
				return std::unexpected(std::move(component).error().WithContext(std::format("changing the script slots of '{}'", path)));
			const auto found = component->find("Scripts");
			if (found == component->end() || !found->is_array())
				return MakeError(ErrorCode::InvalidArgument, "'{}' has no script slot list", path);
			return std::pair<Json, std::string>(*found, std::move(path));
		}

		// The entity.update op that writes `slots` as entity `id`'s whole list.
		static Json MakeScriptSlotUpdate(UUID id, Json slots, SceneTarget target)
		{
			return Json{ { "method", "entity.update" },
				{ "params", Json{ { "entity", id.ToString() }, { "components", Json{ { "Script", Json{ { "Scripts", std::move(slots) } } } } }, { "target", target == SceneTarget::Play ? "Play" : "Edit" } } } };
		}

	}

	Result<Json> MakeScriptSlotChange(const Scene& scene, std::span<const UUID> entities, ScriptSlotChange change, size_t slot, SceneTarget target)
	{
		Json ops = Json::array();
		for (const UUID id : entities)
		{
			ENGINE_TRY_ASSIGN(auto list, Utils::ReadScriptSlots(scene, id));
			auto& [slots, path] = list;
			ENGINE_TRY(Utils::ChangeScriptSlots(slots, change, slot, path));
			ops.push_back(Utils::MakeScriptSlotUpdate(id, std::move(slots), target));
		}
		return Json{ { "label", Utils::ScriptSlotChangeLabel(change) }, { "ops", std::move(ops) } };
	}

	Result<Json> MakeScriptSlotGraph(const Scene& scene, std::span<const UUID> entities, size_t slot, std::string_view path, SceneTarget target)
	{
		Json ops = Json::array();
		ops.push_back(Json{ { "method", "graph.create" }, { "name", "graph" }, { "params", Json{ { "path", std::string(path) } } } });
		for (const UUID id : entities)
		{
			ENGINE_TRY_ASSIGN(auto list, Utils::ReadScriptSlots(scene, id));
			auto& [slots, entityPath] = list;
			if (slot >= slots.size())
				return MakeError(ErrorCode::InvalidArgument, "'{}' has {} script slots, so its slot {} cannot hold a new graph", entityPath, slots.size(), slot);
			// A new behaviour: the slot's saved field values belonged to its old script.
			slots[slot]["Script"] = Json{ { "$ref", "graph.graph.id" } };
			slots[slot]["Fields"] = Json::object();
			ops.push_back(Utils::MakeScriptSlotUpdate(id, std::move(slots), target));
		}
		return Json{ { "label", "New Graph" }, { "ops", std::move(ops) } };
	}

}
