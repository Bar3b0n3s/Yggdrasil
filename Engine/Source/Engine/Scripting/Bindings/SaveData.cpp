#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <lua.h>
#include <nlohmann/json.hpp>

#include <format>
#include <optional>
#include <string>
#include <vector>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): the SaveData module, a game's options and progress kept across
// runs, over the play session's store (IScriptHost's save-data members).

namespace Engine {

	namespace Utils {

		static std::string CheckSaveKey(ScriptCall& call)
		{
			return Lua::CheckBoundedString(call, 1, MaxSaveDataKeyBytes);
		}

		static int SaveDataGet(ScriptCall& call)
		{
			const std::string key = CheckSaveKey(call);
			const Result<Json> value = call.Engine->GetHost().GetSaveValue(key);
			if (!value)
				return Lua::RaiseError(call, value.error());
			if (value->is_null())
				lua_pushnil(call.State);
			else
				Lua::PushJson(call, *value);
			return 1;
		}

		static int SaveDataSet(ScriptCall& call)
		{
			const std::string key = CheckSaveKey(call);
			const Json value = Lua::IsNoneOrNil(call, 2) ? Json(nullptr) : Lua::CheckJson(call, 2);
			// The store is simulation state (the state hash's save-data block): an evaluation's write is an external mutation.
			if (const Status permission = call.PrepareHostMutation(); !permission)
				return Lua::RaiseError(call, permission.error());
			if (const Status set = call.Engine->GetHost().SetSaveValue(key, value); !set)
				return Lua::RaiseError(call, set.error());
			return 0;
		}

		static int SaveDataDelete(ScriptCall& call)
		{
			const std::string key = CheckSaveKey(call);
			IScriptHost& host = call.Engine->GetHost();
			const Result<Json> held = host.GetSaveValue(key);
			if (!held)
				return Lua::RaiseError(call, held.error());
			const bool existed = !held->is_null();
			if (existed)
			{
				if (const Status permission = call.PrepareHostMutation(); !permission)
					return Lua::RaiseError(call, permission.error());
				if (const Status deleted = host.SetSaveValue(key, Json(nullptr)); !deleted)
					return Lua::RaiseError(call, deleted.error());
			}
			Lua::Push(call, existed);
			return 1;
		}

		static int SaveDataKeys(ScriptCall& call)
		{
			const Result<std::vector<std::string>> keys = call.Engine->GetHost().GetSaveKeys();
			if (!keys)
				return Lua::RaiseError(call, keys.error());
			lua_createtable(call.State, static_cast<int>(keys->size()), 0);
			for (size_t index = 0; index < keys->size(); ++index)
			{
				Lua::PushString(call, (*keys)[index]);
				lua_rawseti(call.State, -2, static_cast<int>(index + 1));
			}
			return 1;
		}

		static int SaveDataCommit(ScriptCall& call)
		{
			const Status committed = call.Engine->GetHost().CommitSaveData();
			Lua::Push(call, committed.has_value());
			if (committed.has_value())
				return 1;
			Lua::PushString(call, committed.error().GetMessageText());
			return 2;
		}

		static int SaveDataLoadError(ScriptCall& call)
		{
			const Result<std::optional<std::string>> error = call.Engine->GetHost().GetSaveDataLoadError();
			if (!error)
				return Lua::RaiseError(call, error.error());
			if (error->has_value())
				Lua::PushString(call, **error);
			else
				lua_pushnil(call.State);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterSaveData(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("SaveData", "A game's save data: options and progress kept across runs, in the play session's store.")
				.Function("Get", &Utils::SaveDataGet, "(key: string) -> any",
					std::format("Returns the value saved under key (1 to {} bytes of UTF-8), or nil when there is none: a boolean, number, string, "
								"array or table of string keys, as it was set.",
						MaxSaveDataKeyBytes),
					read)
				.Function("Set", &Utils::SaveDataSet, "(key: string, value: any) -> ()",
					std::format("Saves value under key; nil deletes it. A value is what converts to JSON: nil, booleans, finite numbers, strings, "
								"arrays and tables of string keys (no functions, cycles or mixed keys), nesting at most {} arrays and tables. The "
								"store is part of the session's state hash and lasts across Scene.Load and Scene.Reload; it outlives the run only "
								"once SaveData.Commit writes it. The whole store is at most {} bytes of compact JSON; a Set past it raises, naming "
								"the size.",
						MaxSaveDataDepth, MaxSaveDataBytes))
				.Function("Delete", &Utils::SaveDataDelete, "(key: string) -> boolean", "Deletes the value saved under key; returns whether there was one.")
				.Function("GetKeys", &Utils::SaveDataKeys, "() -> {string}", "Returns the keys of the saved values, sorted by their bytes.", read)
				.Function("Commit", &Utils::SaveDataCommit, "() -> (boolean, string?)",
					"Writes the store where the game keeps it: the Runtime's user://SaveData/Store.json (replaced atomically and flushed to "
					"the disk), editor play's Library/SaveData/Store.json (a read-only editor's private store). Returns true, or false and "
					"the reason (a headless Runtime reads its store and never writes it; a failed write). Test runs, replays, recordings "
					"and lockstep sessions keep their store in memory: Commit returns true and writes nothing.")
				.Function("GetLoadError", &Utils::SaveDataLoadError, "() -> string?",
					"Returns why the store the session started with could not be read (it was moved aside as Store.corrupt-<n>.json and "
					"the game started with no save data), or nil.",
					read);
			return {};
		}

	}

}
