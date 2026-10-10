#include "EnginePCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"
#include "Engine/Scripting/RequireResolver.h"

#include <lualib.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace Engine {

#if !defined(ENGINE_DIST)
	namespace {

		struct ReloadRoots
		{
			lua_State* Vm = nullptr; // borrowed VM; all roots are released before its owner can stop
			std::vector<int> References{};
			~ReloadRoots() { Release(); }
			void Release()
			{
				for (const int reference : References)
					lua_unref(Vm, reference);
				References.clear();
			}
			int Retain(lua_State* state, int index)
			{
				const int reference = lua_ref(state, index);
				References.push_back(reference);
				return reference;
			}
		};

		struct ReloadTable
		{
			int Target = LUA_NOREF;
			int Backup = LUA_NOREF;
			int Candidate = LUA_NOREF;
			bool OldReadOnly = false;
			bool NewReadOnly = false;
			bool Reserved = false;
		};

		struct ReloadModule
		{
			AssetHandle Handle{};
			Ref<const ScriptData> Data{};
			int Old = LUA_NOREF;
			int Candidate = LUA_NOREF;
			int Canonical = LUA_NOREF;
		};

		struct ReloadMetadata
		{
			int Target = LUA_NOREF;
			int Backup = LUA_NOREF;
			int Candidate = LUA_NOREF;
			bool Published = false;
		};

		struct ReloadInstance
		{
			UUID Entity{};
			Ref<const ScriptData> Data{};
			int Table = LUA_NOREF;
			bool Recreated = false;
		};

		void CopyContents(lua_State* vm, int target, int source)
		{
			target = lua_absindex(vm, target);
			source = lua_absindex(vm, source);
			lua_cleartable(vm, target);
			for (int iterator = 0; (iterator = lua_rawiter(vm, source, iterator)) != -1;)
				lua_rawset(vm, target);
			if (!lua_getmetatable(vm, source))
				lua_pushnil(vm);
			lua_setmetatable(vm, target);
		}

		void RestoreTables(lua_State* vm, std::span<const ReloadTable> tables) noexcept
		{
			const int base = lua_gettop(vm);
			// Union capacity was reserved while every original key was still present. Backups pin every key/value.
			// clear retains that capacity; raw writes, metatable changes and readonly restoration cannot allocate.
			for (auto iterator = tables.rbegin(); iterator != tables.rend(); ++iterator)
			{
				if (!iterator->Reserved)
					continue;
				lua_getref(vm, iterator->Target);
				lua_getref(vm, iterator->Backup);
				lua_setreadonly(vm, -2, false);
				CopyContents(vm, -2, -1);
				lua_setreadonly(vm, -2, iterator->OldReadOnly);
				lua_pop(vm, 2);
			}
			lua_settop(vm, base);
		}

		void RestoreMetadata(ScriptCall& call, std::span<const ReloadMetadata> metadata) noexcept
		{
			for (auto iterator = metadata.rbegin(); iterator != metadata.rend(); ++iterator)
			{
				if (!iterator->Published)
					continue;
				lua_getref(call.State, iterator->Target);
				lua_getref(call.State, iterator->Backup);
				const auto restored = Detail::SetScriptRegistrationRecord(call, -2, -1);
				ENGINE_CORE_VERIFY(restored.has_value(), "invalid pinned reload metadata");
				lua_pop(call.State, 2);
			}
		}

		void ReserveAndPatch(ScriptCall& call, std::vector<ReloadTable>& tables)
		{
			lua_State* vm = call.State;
			for (auto& table : tables)
			{
				lua_getref(vm, table.Target);
				const int target = lua_gettop(vm);
				lua_setreadonly(vm, target, false);
				table.Reserved = true; // rollback also covers an allocation failure halfway through reservation
				lua_getref(vm, table.Candidate);
				const int source = lua_gettop(vm);
				for (int iterator = 0; (iterator = lua_rawiter(vm, source, iterator)) != -1;)
				{
					lua_pop(vm, 1);
					lua_pushvalue(vm, -1);
					lua_rawget(vm, target);
					const bool absent = lua_isnil(vm, -1);
					lua_pop(vm, 1);
					if (absent)
					{
						lua_pushboolean(vm, true);
						lua_rawset(vm, target);
					}
					else
						lua_pop(vm, 1);
				}
				lua_pop(vm, 2);
			}
			for (auto& table : tables)
			{
				lua_getref(vm, table.Target);
				lua_getref(vm, table.Candidate);
				CopyContents(vm, -2, -1);
				lua_setreadonly(vm, -2, table.NewReadOnly);
				lua_pop(vm, 2);
			}
		}

		Status AddPatch(lua_State* vm, ReloadRoots& roots, std::vector<ReloadTable>& tables, int target, int candidate)
		{
			for (const auto& previous : tables)
			{
				lua_getref(vm, previous.Target);
				lua_getref(vm, target);
				const bool sameTarget = lua_rawequal(vm, -2, -1) != 0;
				lua_pop(vm, 2);
				if (!sameTarget)
					continue;
				lua_getref(vm, previous.Candidate);
				lua_getref(vm, candidate);
				const bool sameCandidate = lua_rawequal(vm, -2, -1) != 0;
				lua_pop(vm, 2);
				if (!sameCandidate)
					return MakeError(ErrorCode::Validation, "aliased live module tables cannot receive different reload replacements");
				return {};
			}
			lua_getref(vm, target);
			const bool oldReadOnly = lua_getreadonly(vm, -1) != 0;
			lua_clonetable(vm, -1);
			const int backup = roots.Retain(vm, -1);
			lua_pop(vm, 2);
			lua_getref(vm, candidate);
			const bool newReadOnly = lua_getreadonly(vm, -1) != 0;
			lua_pop(vm, 1);
			tables.push_back({ target, backup, candidate, oldReadOnly, newReadOnly, false });
			return {};
		}

		bool HasIdentity(lua_State* vm, int index)
		{
			const int type = lua_type(vm, index);
			return type == LUA_TTABLE || type == LUA_TFUNCTION || type == LUA_TUSERDATA || type == LUA_TTHREAD || type == LUA_TBUFFER;
		}

		void PollGraph(ScriptCall& call, uint32_t depth)
		{
			if (depth >= 256)
				Lua::RaiseError(call, "reload value graph exceeds the supported traversal depth");
			if (const auto fault = Detail::SandboxAccess::CheckInterrupt(*call.Sandbox, -1))
				Lua::RaiseError(call, *fault == ScriptErrorKind::Memory ? "reload memory limit exceeded" : "reload deadline exceeded");
			if (!lua_checkstack(call.State, 12))
				Lua::RaiseError(call, "reload value graph exceeds the VM stack limit");
		}

		void PushCanonical(lua_State* vm, int index, const std::map<const void*, int>& replacements)
		{
			const auto found = HasIdentity(vm, index) ? replacements.find(lua_topointer(vm, index)) : replacements.end();
			if (found != replacements.end())
				lua_getref(vm, found->second);
			else
				lua_pushvalue(vm, index);
		}

		void MatchExports(ScriptCall& call, ReloadRoots& roots, int candidate, int live,
			std::map<const void*, int>& replacements, uint32_t depth = 0)
		{
			lua_State* vm = call.State;
			candidate = lua_absindex(vm, candidate);
			live = lua_absindex(vm, live);
			if (!HasIdentity(vm, candidate) || lua_rawequal(vm, candidate, live))
				return;
			PollGraph(call, depth);
			const auto pointer = lua_topointer(vm, candidate);
			const auto found = replacements.find(pointer);
			if (found != replacements.end())
			{
				lua_getref(vm, found->second);
				const bool same = lua_rawequal(vm, -1, live) != 0;
				lua_pop(vm, 1);
				if (!same)
					Lua::RaiseError(call, "unchanged module exports have conflicting live identities");
				return;
			}
			replacements.emplace(pointer, roots.Retain(vm, live));
			if (!lua_istable(vm, candidate) || !lua_istable(vm, live))
				return;
			// Primitive export keys establish object identities first. Object keys may then refer to those exports.
			for (int pass = 0; pass != 2; ++pass)
				for (int iterator = 0; (iterator = lua_rawiter(vm, candidate, iterator)) != -1;)
				{
					const bool identityKey = HasIdentity(vm, -2);
					if (identityKey != (pass != 0))
					{
						lua_pop(vm, 2);
						continue;
					}
					if (identityKey && !replacements.contains(lua_topointer(vm, -2)))
						Lua::RaiseError(call, "unchanged module has an object key without a stable exported identity");
					PushCanonical(vm, -2, replacements);
					lua_rawget(vm, live);
					MatchExports(call, roots, -2, -1, replacements, depth + 1);
					lua_pop(vm, 3);
				}
			if (lua_getmetatable(vm, candidate))
			{
				if (!lua_getmetatable(vm, live))
					lua_pushnil(vm);
				MatchExports(call, roots, -2, -1, replacements, depth + 1);
				lua_pop(vm, 2);
			}
		}

		void MarkSharedValues(ScriptCall& call, int index, std::set<const void*>& visited, uint32_t depth = 0)
		{
			lua_State* vm = call.State;
			index = lua_absindex(vm, index);
			if (!HasIdentity(vm, index) || !visited.insert(lua_topointer(vm, index)).second)
				return;
			PollGraph(call, depth);
			if (!lua_istable(vm, index))
				return;
			for (int iterator = 0; (iterator = lua_rawiter(vm, index, iterator)) != -1;)
			{
				MarkSharedValues(call, -2, visited, depth + 1);
				MarkSharedValues(call, -1, visited, depth + 1);
				lua_pop(vm, 2);
			}
			if (lua_getmetatable(vm, index))
			{
				MarkSharedValues(call, -1, visited, depth + 1);
				lua_pop(vm, 1);
			}
		}

		void RebindGraph(ScriptCall& call, int index, const std::map<const void*, int>& replacements,
			std::set<const void*>& visited, uint32_t depth = 0)
		{
			lua_State* vm = call.State;
			index = lua_absindex(vm, index);
			const bool table = lua_istable(vm, index);
			const bool function = lua_isfunction(vm, index);
			const bool thread = lua_isthread(vm, index);
			const auto pointer = lua_topointer(vm, index);
			// Only root candidates are traversed when they themselves have a canonical replacement. In particular,
			// never follow a freshly rebound upvalue into a live object; old code/state stays completely untouched.
			if ((!table && !function && !thread) || (depth != 0 && replacements.contains(pointer)) || !visited.insert(pointer).second)
				return;
			PollGraph(call, depth);
			if (function)
			{
				for (int upvalue = 1; lua_getupvalue(vm, index, upvalue); ++upvalue)
				{
					RebindGraph(call, -1, replacements, visited, depth + 1);
					PushCanonical(vm, -1, replacements);
					lua_remove(vm, -2);
					static_cast<void>(lua_setupvalue(vm, index, upvalue));
				}
				lua_getfenv(vm, index);
				RebindGraph(call, -1, replacements, visited, depth + 1);
				PushCanonical(vm, -1, replacements);
				static_cast<void>(lua_setfenv(vm, index));
				lua_pop(vm, 1);
				return;
			}
			if (thread)
			{
				lua_State* child = lua_tothread(vm, index);
				// Public Luau APIs expose an unstarted thread's function and arguments, but not every temporary
				// register in suspended frames. Refuse an unprovable publication rather than retain staged aliases.
				if (lua_stackdepth(child) != 0)
					Lua::RaiseError(call, "reload cannot reconnect a coroutine suspended during module initialization");
				if (!lua_checkstack(child, 2))
					Lua::RaiseError(call, "reload coroutine exceeds the VM stack limit");
				const int count = lua_gettop(child);
				for (int slot = 1; slot <= count; ++slot)
				{
					lua_pushvalue(child, slot);
					lua_xmove(child, vm, 1);
					RebindGraph(call, -1, replacements, visited, depth + 1);
					PushCanonical(vm, -1, replacements);
					lua_xmove(vm, child, 1);
					lua_replace(child, slot);
					lua_pop(vm, 1);
				}
				return;
			}
			const bool readOnly = lua_getreadonly(vm, index) != 0;
			struct RestoreReadOnly
			{
				lua_State* Vm = nullptr;
				int Index = 0;
				bool ReadOnly = false;
				~RestoreReadOnly() { lua_setreadonly(Vm, Index, ReadOnly); }
			} restore{ vm, index, readOnly };
			lua_setreadonly(vm, index, false);
			lua_clonetable(vm, index);
			const int snapshot = lua_gettop(vm);
			for (int iterator = 0; (iterator = lua_rawiter(vm, snapshot, iterator)) != -1;)
			{
				RebindGraph(call, -2, replacements, visited, depth + 1);
				RebindGraph(call, -1, replacements, visited, depth + 1);
				PushCanonical(vm, -2, replacements);
				const bool replacedKey = !lua_rawequal(vm, -1, -3);
				if (lua_isnil(vm, -1))
					lua_pop(vm, 1);
				else
				{
					PushCanonical(vm, -2, replacements);
					lua_rawset(vm, index);
				}
				if (replacedKey)
				{
					lua_pushvalue(vm, -2);
					lua_pushnil(vm);
					lua_rawset(vm, index);
				}
				lua_pop(vm, 2);
			}
			lua_pop(vm, 1);
			if (lua_getmetatable(vm, index))
			{
				RebindGraph(call, -1, replacements, visited, depth + 1);
				PushCanonical(vm, -1, replacements);
				lua_setmetatable(vm, index);
				lua_pop(vm, 1);
			}
		}

		Error ReloadError(const ScriptError& failure)
		{
			ErrorLocation location{};
			location.File = failure.Script;
			location.Line = failure.Line;
			location.Column = failure.Column;
			if (!failure.JsonPointer.empty())
				location.JsonPointer = failure.JsonPointer;
			return Error(failure.Kind == ScriptErrorKind::Timeout ? ErrorCode::Timeout : failure.Kind == ScriptErrorKind::Compile ? ErrorCode::CompileFailed
																																  : ErrorCode::Script,
				failure.Message)
				.WithLocation(std::move(location));
		}

	}

#endif

	Result<ScriptReloadResult> ScriptEngine::Reload(AssetHandle changed, bool fromTest)
	{
		m_State->AssertOwner();
#if defined(ENGINE_DIST)
		static_cast<void>(changed);
		static_cast<void>(fromTest);
		return MakeError(ErrorCode::Unsupported, "script reload is unavailable in Dist");
#else
		if (!changed.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "reload requires a script asset");
		if (IsStopped() || !m_State->Vm || IsReadOnly())
			return MakeError(ErrorCode::InvalidState, "reload requires a writable running script engine");
		if (GetRunMode() != RunModes::Editor)
			return MakeError(ErrorCode::Unsupported, "script reload is EditorOnly");
		if (fromTest && (!IsTestMode() || !m_State->SuiteActive))
			return MakeError(ErrorCode::InvalidState, "explicit test reload requires an active suite");
		if (GetHost().IsReloadDeferred() && !fromTest)
			return ScriptReloadResult{ .Deferred = true };
		AssetManager* assets = GetHost().GetAssets();
		if (!assets)
			return MakeError(ErrorCode::InvalidState, "reload requires an asset manager");
		auto& sandbox = *m_State->Vm;
		Detail::SandboxAccess::ClearFailure(sandbox);
		ScriptCall main = Detail::SandboxAccess::MainCall(sandbox);
		ReloadRoots roots{ main.State, {} };
		std::vector<std::string> loaded;
		Status preparation{};
		auto inventoried = m_State->RunNative([&loaded, &preparation, &roots](ScriptCall& call)
		{
			roots.Vm = call.State;
			auto paths = Detail::SandboxAccess::GetLoadedModulePaths(call);
			if (!paths)
				preparation = std::unexpected(paths.error());
			else
				loaded = std::move(*paths);
			// Also reserve the host stack for non-allocating rollback after this protected frame is gone.
			if (!lua_checkstack(call.State, 24))
				return Lua::RaiseError(call, "reload cannot reserve rollback stack capacity");
			return 0;
		}, fromTest ? ScriptExecutionOrigin::TestDriver : ScriptExecutionOrigin::Eval, {}, "Reload");
		if (!inventoried)
		{
			m_State->Publish(inventoried.error(), {}, "Reload");
			return std::unexpected(inventoried.error());
		}
		if (inventoried->Failure)
		{
			m_State->Publish(*inventoried->Failure);
			return std::unexpected(ReloadError(*inventoried->Failure));
		}
		const auto publishFailure = [this](Status status) -> Status
		{
			if (!status)
				m_State->Publish(status.error(), {}, "Reload");
			return status;
		};
		ENGINE_TRY(publishFailure(std::move(preparation)));
		std::map<AssetHandle, Ref<const ScriptData>> scripts;
		std::map<std::string, AssetHandle> handles;
		const auto add = [&scripts, &handles, assets](AssetHandle handle) -> Status
		{
			if (scripts.contains(handle))
				return {};
			ENGINE_TRY_ASSIGN(auto asset, assets->Load(handle));
			auto data = AssetCast<ScriptData>(asset);
			if (!data)
				return MakeError(ErrorCode::Validation, "reload target is not a script");
			handles.emplace(data->SourceMap.Path, handle);
			scripts.emplace(handle, std::move(data));
			return {};
		};
		ENGINE_TRY(publishFailure(add(changed)));
		for (const auto& path : loaded)
			if (const auto handle = assets->Resolve(path))
				ENGINE_TRY(publishFailure(add(*handle)));
		for (const auto& [entity, instance] : m_State->Instances)
		{
			static_cast<void>(entity);
			ENGINE_TRY(publishFailure(add(instance.Handle)));
		}
		std::map<AssetHandle, std::set<AssetHandle>> dependencies;
		for (const auto& [handle, data] : scripts)
			for (const auto& edge : data->Requires)
				if (edge.From == data->SourceMap.Path)
				{
					const auto dependency = edge.Handle.IsValid() ? std::optional<AssetHandle>(edge.Handle) : assets->Resolve(edge.Path);
					if (dependency)
						dependencies[handle].insert(*dependency);
				}
		for (const auto& edge : Detail::SandboxAccess::GetResolver(sandbox)->GetRequires())
		{
			const auto from = handles.find(edge.From);
			const auto to = handles.find(edge.Path);
			if (from != handles.end() && to != handles.end())
				dependencies[from->second].insert(to->second);
		}
		std::set<AssetHandle> affected{ changed };
		for (bool expanded = true; expanded;)
		{
			expanded = false;
			for (const auto& [handle, required] : dependencies)
				if (!affected.contains(handle) && std::ranges::any_of(required, [&affected](AssetHandle dependency)
				{
					return affected.contains(dependency);
				}))
					expanded = affected.insert(handle).second || expanded;
		}
		std::vector<AssetHandle> ordered;
		std::map<AssetHandle, uint8_t> visits;
		const auto visit = [&dependencies, &affected, &visits, &ordered](auto&& self, AssetHandle handle) -> Status
		{
			if (visits[handle] == 2)
				return {};
			if (visits[handle] == 1)
				return MakeError(ErrorCode::Script, "reload dependency graph contains a cycle");
			visits[handle] = 1;
			for (AssetHandle dependency : dependencies[handle])
				if (affected.contains(dependency))
					ENGINE_TRY(self(self, dependency));
			visits[handle] = 2;
			ordered.push_back(handle);
			return {};
		};
		for (AssetHandle handle : affected)
			ENGINE_TRY(publishFailure(visit(visit, handle)));
		std::vector<Ref<const ScriptData>> candidates;
		std::vector<ReloadModule> reloadModules;
		std::map<AssetHandle, AssetRef<ScriptData>> schemaScripts;
		for (AssetHandle handle : ordered)
		{
			const auto data = scripts.find(handle);
			if (data == scripts.end())
			{
				const Error error(ErrorCode::NotFound, "reload dependency has no published script asset");
				m_State->Publish(error, {}, "Reload");
				return std::unexpected(error);
			}
			candidates.push_back(data->second);
			reloadModules.push_back({ handle, data->second });
			schemaScripts.emplace(handle, data->second);
		}
		const auto schemaResult = ScriptFieldSchemaSource::Create(std::move(schemaScripts));
		if (!schemaResult)
		{
			m_State->Publish(schemaResult.error(), {}, "Reload");
			return std::unexpected(schemaResult.error());
		}
		const auto schemas = *schemaResult;
		std::vector<ReloadTable> patches;
		std::vector<ReloadMetadata> metadata;
		std::vector<ReloadInstance> instances;
		Status operation{};
		const auto outcome = m_State->RunNative([this, &roots, &reloadModules, &candidates, &patches, &metadata, &instances, &operation, schemas](ScriptCall& call) -> int
		{
			const auto perform = [&]() -> Status
			{
				for (auto& module : reloadModules)
				{
					ENGINE_TRY_ASSIGN(auto path, VfsPath::Create("project", module.Data->SourceMap.Path));
					const auto cached = Detail::SandboxAccess::PushModuleReturn(call, path);
					if (cached)
					{
						module.Old = roots.Retain(call.State, -1);
						lua_pop(call.State, 1);
					}
					else if (cached.error().GetCode() != ErrorCode::NotFound)
						return std::unexpected(cached.error());
				}
				ENGINE_TRY(Detail::SandboxAccess::BeginReload(call, candidates));
				for (auto& module : reloadModules)
				{
					ENGINE_TRY(Detail::SandboxAccess::EvaluateReloadModule(call, *module.Data));
					module.Candidate = roots.Retain(call.State, -1);
					if (module.Data->Kind == ScriptKind::Behaviour)
					{
						ENGINE_TRY_ASSIGN(auto registration, Detail::InspectScriptRegistration(call, -1));
						if (registration.Kind != ScriptKind::Behaviour || !lua_istable(call.State, -1))
							return MakeError(ErrorCode::Validation, "reload Behaviour did not return an authenticated class");
					}
					const bool newTable = lua_istable(call.State, -1);
					lua_pop(call.State, 1);
					if (module.Old != LUA_NOREF)
					{
						lua_getref(call.State, module.Old);
						if (newTable && lua_istable(call.State, -1))
							module.Canonical = module.Old;
						lua_pop(call.State, 1);
					}
					if (module.Canonical == LUA_NOREF)
						module.Canonical = module.Candidate;
				}
				std::map<const void*, int> replacements;
				for (const auto& module : reloadModules)
					if (module.Canonical != module.Candidate)
					{
						lua_getref(call.State, module.Candidate);
						replacements.emplace(lua_topointer(call.State, -1), module.Canonical);
						lua_pop(call.State, 1);
					}
				std::vector<int> newDependencies;
				ENGINE_TRY_ASSIGN(auto stagedPaths, Detail::SandboxAccess::GetReloadModulePaths(call));
				for (const auto& name : stagedPaths)
				{
					if (std::ranges::any_of(reloadModules, [&name](const auto& module)
					{
						return module.Data->SourceMap.Path == name;
					}))
						continue;
					ENGINE_TRY_ASSIGN(auto path, VfsPath::Create("project", name));
					ENGINE_TRY(Detail::SandboxAccess::PushModuleReturn(call, path));
					const auto live = Detail::SandboxAccess::PushLiveModuleReturn(call, path);
					if (live)
					{
						MatchExports(call, roots, -2, -1, replacements);
						ENGINE_TRY(Detail::SandboxAccess::SetReloadModuleReturn(call, path, -1));
						lua_pop(call.State, 2);
					}
					else
					{
						if (live.error().GetCode() != ErrorCode::NotFound)
							return std::unexpected(live.error());
						newDependencies.push_back(roots.Retain(call.State, -1));
						lua_pop(call.State, 1);
					}
				}
				std::set<const void*> visited;
				lua_xpush(lua_mainthread(call.State), call.State, LUA_GLOBALSINDEX);
				MarkSharedValues(call, -1, visited);
				lua_pop(call.State, 1);
				for (const auto& [candidate, reference] : replacements)
				{
					static_cast<void>(candidate);
					lua_getref(call.State, reference);
					if (HasIdentity(call.State, -1))
						visited.insert(lua_topointer(call.State, -1));
					lua_pop(call.State, 1);
				}
				for (const auto& module : reloadModules)
				{
					lua_getref(call.State, module.Candidate);
					RebindGraph(call, -1, replacements, visited);
					if (module.Data->Kind == ScriptKind::Behaviour)
					{
						const bool readOnly = lua_getreadonly(call.State, -1) != 0;
						lua_setreadonly(call.State, -1, false);
						lua_getref(call.State, module.Canonical);
						lua_rawsetfield(call.State, -2, "__index");
						lua_setreadonly(call.State, -1, readOnly);
					}
					lua_pop(call.State, 1);
					if (module.Canonical != module.Candidate)
					{
						ENGINE_TRY(AddPatch(call.State, roots, patches, module.Canonical, module.Candidate));
						lua_getref(call.State, module.Canonical);
						ENGINE_TRY(Detail::PushScriptRegistrationRecord(call, -1));
						const int previous = roots.Retain(call.State, -1);
						lua_pop(call.State, 2);
						lua_getref(call.State, module.Candidate);
						ENGINE_TRY(Detail::PushScriptRegistrationRecord(call, -1));
						const int next = roots.Retain(call.State, -1);
						lua_pop(call.State, 2);
						metadata.push_back({ module.Canonical, previous, next });
					}
					lua_getref(call.State, module.Canonical);
					ENGINE_TRY(Detail::SandboxAccess::SetReloadModule(call, *module.Data, -1));
					lua_pop(call.State, 1);
				}
				for (int reference : newDependencies)
				{
					lua_getref(call.State, reference);
					RebindGraph(call, -1, replacements, visited);
					lua_pop(call.State, 1);
				}

				for (const auto& [id, instance] : m_State->Instances)
				{
					const auto module = std::ranges::find_if(reloadModules, [&instance](const auto& item)
					{
						return item.Handle == instance.Handle;
					});
					if (module == reloadModules.end() || instance.DestroyPrepared)
						continue;
					if (module->Data->Kind != ScriptKind::Behaviour)
						return MakeError(ErrorCode::Validation, "an attached script cannot reload as a non-Behaviour");
					const bool recreated = instance.Table == LUA_NOREF;
					if (recreated)
					{
						lua_newtable(call.State);
						Lua::Push(call, ScriptEntityIdentity{ id, GetHost().GetSceneGeneration() });
						lua_rawsetfield(call.State, -2, "Entity");
					}
					else
					{
						lua_getref(call.State, instance.Table);
						lua_clonetable(call.State, -1);
						lua_remove(call.State, -2);
					}
					const int candidate = lua_gettop(call.State);
					lua_getref(call.State, module->Canonical);
					lua_setmetatable(call.State, candidate);
					ResolveContext resolve{};
					resolve.Registry = &GetHost().GetTypes();
					resolve.Schemas = schemas.get();
					for (const auto& schema : module->Data->Fields)
					{
						// Runtime state survives, including removed fields and dynamically added values.
						lua_rawgetfield(call.State, candidate, schema.Name.c_str());
						const bool absent = lua_isnil(call.State, -1);
						lua_pop(call.State, 1);
						if (!absent || (!recreated && instance.Script && std::ranges::any_of(instance.Script->Fields, [&schema](const auto& previous)
						{
							return previous.Name == schema.Name;
						})))
							continue;
						ENGINE_TRY_ASSIGN(auto field, schemas->FindField(instance.Handle, schema.Name));
						ENGINE_TRY_ASSIGN(auto value, ValueFromJson(JsonReader(schema.DefaultValue.Get()), field->GetType()));
						const auto overridden = instance.Overrides.find(schema.Name);
						if (overridden != instance.Overrides.end())
						{
							auto overrideValue = ValueFromJson(JsonReader(overridden->second.Get()), field->GetType());
							ValidationContext validation;
							if (overrideValue)
								field->ValidateValue(*overrideValue, resolve, validation);
							if (overrideValue && !validation.HasErrors())
								value = std::move(*overrideValue);
						}
						Lua::PushValue(call, value, *field);
						lua_rawsetfield(call.State, candidate, schema.Name.c_str());
					}
					const int candidateReference = roots.Retain(call.State, candidate);
					lua_pop(call.State, 1);
					if (!recreated)
						ENGINE_TRY(AddPatch(call.State, roots, patches, instance.Table, candidateReference));
					instances.push_back({ id, module->Data, recreated ? candidateReference : instance.Table, recreated });
				}
				ENGINE_TRY(call.PrepareHostMutation());
				ReserveAndPatch(call, patches);
				for (auto& entry : metadata)
				{
					// Keep old nonnil associations in their existing slots through every allocating operation.
					// Removing one is deferred until success, so rollback never needs to reinsert a collected slot.
					if (entry.Candidate == LUA_REFNIL)
						continue;
					lua_getref(call.State, entry.Target);
					lua_getref(call.State, entry.Candidate);
					entry.Published = true;
					ENGINE_TRY(Detail::SetScriptRegistrationRecord(call, -2, -1));
					lua_pop(call.State, 2);
				}
				return Detail::SandboxAccess::CommitReload(call);
			};
			operation = perform();
			return 0;
		}, fromTest ? ScriptExecutionOrigin::TestDriver : ScriptExecutionOrigin::Eval, {}, "Reload");
		if (!outcome || outcome->Failure || !operation)
		{
			ScriptCall restoration{ .State = roots.Vm, .Sandbox = &sandbox, .Engine = this };
			RestoreTables(roots.Vm, patches);
			RestoreMetadata(restoration, metadata);
			Detail::SandboxAccess::RollbackReload(sandbox);
			roots.Release(); // recovery must not retain failed candidate allocations through these temporary roots
			if (outcome && outcome->Failure)
			{
				m_State->Publish(*outcome->Failure);
				return std::unexpected(ReloadError(*outcome->Failure));
			}
			const Error error = !outcome ? outcome.error() : operation.error();
			m_State->Publish(error, {}, "Reload");
			return std::unexpected(error);
		}
		Detail::SandboxAccess::FinalizeReload(sandbox);
		for (const auto& entry : metadata)
			if (entry.Candidate == LUA_REFNIL && entry.Backup != LUA_REFNIL)
			{
				ScriptCall restoration{ .State = roots.Vm, .Sandbox = &sandbox, .Engine = this };
				lua_getref(roots.Vm, entry.Target);
				lua_pushnil(roots.Vm);
				const auto removed = Detail::SetScriptRegistrationRecord(restoration, -2, -1);
				ENGINE_CORE_VERIFY(removed.has_value(), "invalid pinned reload metadata removal");
				lua_pop(roots.Vm, 2);
			}
		for (const auto& update : instances)
		{
			const auto found = m_State->Instances.find(update.Entity);
			if (found == m_State->Instances.end())
				continue;
			found->second.Script = update.Data;
			found->second.FaultDisabled = false;
			m_State->DisabledHistory.erase(update.Entity);
			if (update.Recreated)
			{
				found->second.Table = update.Table;
				// Transfer the new instance's root to State; every other transaction root remains scoped here.
				const auto reference = std::ranges::find(roots.References, update.Table);
				ENGINE_CORE_VERIFY(reference != roots.References.end(), "missing reload instance root");
				roots.References.erase(reference);
			}
		}
		roots.Release();
		for (UUID id : m_State->OrderedInstances(false))
			if (std::ranges::any_of(instances, [id](const auto& item)
			{
				return item.Entity == id;
			}))
				m_State->Invoke(id, "OnHotReload");
		return ScriptReloadResult{ .Scripts = std::move(ordered) };
#endif
	}

}
