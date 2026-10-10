#include "EnginePCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <Luau/Allocator.h>
#include <Luau/Ast.h>
#include <Luau/Parser.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <set>
#include <tuple>

namespace Engine {

	struct ScriptApiRegistry::Storage
	{
		std::vector<ScriptApiGroup> Modules{};
		std::vector<ScriptApiGroup> Types{};
		std::vector<ScriptApiEnum> Enums{};
		std::vector<ScriptApiAlias> Aliases{};
		std::array<ScriptApiCoverage, 3> Coverage{};
		const TypeRegistry* Reflection = nullptr;
		bool Frozen = false;
		std::string Invalid{};
	};

	namespace Utils {

		constexpr std::array<RunModes, 3> ScriptModes{ RunModes::Editor, RunModes::Release, RunModes::Dist };
		static int ModeIndex(RunModes mode)
		{
			for (int i = 0; i < 3; ++i)
				if (ScriptModes[static_cast<size_t>(i)] == mode)
					return i;
			return -1;
		}
		static bool Identifier(std::string_view name)
		{
			if (name.empty())
				return false;
			for (size_t i = 0; i < name.size(); ++i)
			{
				const char c = name[i];
				if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || (i && c >= '0' && c <= '9')))
					return false;
			}
			return true;
		}
		static ScriptApiGroup& Group(std::vector<ScriptApiGroup>& groups, std::string_view name, std::string_view description, std::string& invalid)
		{
			for (auto& group : groups)
				if (group.Name == name)
				{
					if (!description.empty() && !group.Description.empty() && group.Description != description)
						invalid = std::format("conflicting description for {}", name);
					if (group.Description.empty())
						group.Description = description;
					return group;
				}
			groups.push_back({ std::string(name), std::string(description), {} });
			return groups.back();
		}
		static void Append(auto& storage, bool type, size_t index, ScriptApiMember member)
		{
			ENGINE_CORE_ASSERT(!storage.Frozen, "Script registration after Freeze");
			if (storage.Frozen)
				return;
			auto& group = (type ? storage.Types : storage.Modules)[index];
			for (const auto& existing : group.Members)
				if (existing.Name == member.Name)
				{
					ENGINE_CORE_ASSERT(false, "Duplicate script member {}.{}", group.Name, member.Name);
					storage.Invalid = std::format("duplicate script member {}.{}", group.Name, member.Name);
					return;
				}
			group.Members.push_back(std::move(member));
		}
		static std::string ReflectedType(const TypeInfo& type)
		{
			switch (type.GetKind())
			{
				case FieldType::Bool:      return "boolean";
				case FieldType::Int32:
				case FieldType::UInt32:
				case FieldType::Float:     return "number";
				case FieldType::Vec2:
				case FieldType::Vec3:      return "vector";
				case FieldType::Vec4:
				case FieldType::Color3:
				case FieldType::Color4:    return "Color";
				case FieldType::Quat:      return "Quat";
				case FieldType::Bool3:     return "{boolean}";
				case FieldType::String:    return "string";
				case FieldType::EntityRef: return "Entity?";
				case FieldType::AssetRef:  return "AssetRef?";
				case FieldType::Enum:      return type.GetEnum()->GetName();
				case FieldType::Array:     return "{" + ReflectedType(*type.GetElement()) + "}";
				case FieldType::Map:       return "{[string]: " + ReflectedType(*type.GetElement()) + "}";
				case FieldType::Variant:   return "any";
				case FieldType::Struct:
				{
					std::string result = "{";
					for (const auto& field : type.GetStruct()->GetFields())
					{
						if (result.size() > 1)
							result += ", ";
						result += field->GetName() + ": " + ReflectedType(field->GetType());
					}
					return result + "}";
				}
			}
			return "unknown";
		}
		class TypeNames final : public Luau::AstVisitor
		{
		public:
			bool visit(Luau::AstType*) override { return true; }
			bool visit(Luau::AstTypePack*) override { return true; }
			bool visit(Luau::AstTypeReference* node) override
			{
				if (!m_Generics.contains(node->name.value))
					References.emplace(node->name.value);
				return true;
			}
			bool visit(Luau::AstTypeFunction* node) override
			{
				HasGenerics |= node->generics.size != 0 || node->genericPacks.size != 0;
				auto previous = m_Generics;
				for (const auto* generic : node->generics)
					m_Generics.emplace(generic->name.value);
				for (const auto* generic : node->genericPacks)
					m_Generics.emplace(generic->name.value);
				for (auto* argument : node->argTypes.types)
					argument->visit(this);
				if (node->argTypes.tailType)
					node->argTypes.tailType->visit(this);
				node->returnTypes->visit(this);
				m_Generics = std::move(previous);
				return false;
			}
			bool visit(Luau::AstTypePackGeneric* node) override
			{
				if (!m_Generics.contains(node->genericName.value))
					References.emplace(node->genericName.value);
				return false;
			}
			std::set<std::string> References{};
			bool HasGenerics = false;
		private:
			std::set<std::string> m_Generics{};
		};
		static bool CallableType(const Luau::AstType* type)
		{
			if (type->is<Luau::AstTypeFunction>())
				return true;
			if (const auto* group = type->as<Luau::AstTypeGroup>())
				return CallableType(group->type);
			if (const auto* intersection = type->as<Luau::AstTypeIntersection>())
			{
				for (const auto* item : intersection->types)
					if (!CallableType(item))
						return false;
				return intersection->types.size > 0;
			}
			return false;
		}
		static Status ValidateType(std::string_view text, const std::set<std::string>& known, bool callable = false)
		{
			Luau::Allocator allocator;
			Luau::AstNameTable names(allocator);
			auto parsed = Luau::Parser::parseType(text.data(), text.size(), names, allocator);
			if (!parsed.root || !parsed.errors.empty())
				return MakeError(ErrorCode::Validation, "invalid Luau signature '{}'", text);
			if (callable && !CallableType(parsed.root))
				return MakeError(ErrorCode::Validation, "callable needs a function signature: '{}'", text);
			TypeNames used;
			parsed.root->visit(&used);
			for (const auto& name : used.References)
				if (!known.contains(name))
					return MakeError(ErrorCode::Validation, "unknown script type '{}' in '{}'", name, text);
			return {};
		}
		static ScriptApiCounter* Counter(auto& storage, RunModes mode, std::string_view owner, std::string_view member)
		{
			const int index = ModeIndex(mode);
			if (index < 0)
				return nullptr;
			for (auto& counter : storage.Coverage[static_cast<size_t>(index)].Members)
				if (counter.Owner == owner && counter.Member == member)
					return &counter;
			return nullptr;
		}
		static bool Count(const ScriptCall& call)
		{
			return call.Engine && call.Engine->IsTestMode();
		}

	}

	namespace Detail {

		struct ScriptRegistryAccess
		{
			static ScriptCall Call(lua_State* state)
			{
				Sandbox* sandbox = SandboxAccess::FromState(state);
				if (!sandbox)
					luaL_error(state, "unowned script state");
				auto call = SandboxAccess::ThreadCall(*sandbox, state);
				if (!call)
					luaL_error(state, "%s", call.error().GetMessageText().c_str());
				return *call;
			}
			static int Invoke(ScriptCall& call, const ScriptApiGroup& group, const ScriptApiMember& member, bool write)
			{
				auto& api = *SandboxAccess::GetApi(*call.Sandbox);
				auto& storage = *api.m_Storage;
				const std::string qualified = group.Name + "." + member.Name;
				call.MemberName = qualified;
				call.Member = &member;
				if (call.Engine)
				{
					const auto executable = ScriptEngineAccess::CheckExecution(call);
					if (!executable)
						return lua_isyieldable(call.State) ? lua_yield(call.State, 0) : Lua::RaiseError(call, executable.error());
				}
				lua_rawgetfield(call.State, LUA_REGISTRYINDEX, "Engine.Api.Environment");
				const auto environment = SandboxAccess::IsReloadEvaluating(*call.Sandbox) ? ScriptApiEnvironment::LoadTime : static_cast<ScriptApiEnvironment>(lua_tointeger(call.State, -1));
				lua_pop(call.State, 1);
				lua_rawgetfield(call.State, LUA_REGISTRYINDEX, "Engine.Api.Mode");
				const auto mode = static_cast<RunModes>(lua_tointeger(call.State, -1));
				lua_pop(call.State, 1);
				if (environment == ScriptApiEnvironment::LoadTime && !HasFlag(member.Options.Environments, environment))
					return Lua::RaiseError(call, "is not available at load time");
				if (environment == ScriptApiEnvironment::Runtime && !call.Engine && !HasFlag(member.Options.Environments, ScriptApiEnvironment::LoadTime))
					return Lua::RaiseError(call, "requires an active script engine");
				if (!HasFlag(member.Options.Environments, environment))
					return Lua::RaiseError(call, "is only available in test runs");
				if (!HasFlag(member.Options.Modes, mode))
					return Lua::RaiseError(call, "is not available in this run mode");
				if (write && !member.Writable)
					return Lua::RaiseError(call, "property is read-only");
				if (write ? member.Options.SetterMutates : member.Options.Mutates)
				{
					auto writable = call.CheckWritable();
					if (!writable)
						return Lua::RaiseError(call, writable.error());
				}
				std::vector<std::pair<int, int64_t>> enumValues;
				for (const auto& parameter : member.Options.EnumParameters)
				{
					if (parameter.Optional && parameter.DefaultValue.empty() && Lua::IsNoneOrNil(call, parameter.ArgumentIndex))
						continue;
					enumValues.emplace_back(parameter.ArgumentIndex, Lua::CheckEnum(call, parameter.ArgumentIndex, parameter.EnumName));
				}
				ScriptApiCounter* counter = Utils::Count(call) ? Utils::Counter(storage, mode, group.Name, member.Name) : nullptr;
				if (counter)
				{
					if (member.Kind != ScriptApiMemberKind::Property && member.Kind != ScriptApiMemberKind::ProxyField)
						++counter->Calls;
					for (auto& hit : counter->EnumValues)
						for (const auto& [slot, value] : enumValues)
							if (slot == hit.ArgumentIndex)
								for (const auto& entry : api.FindEnum(hit.EnumName)->Values)
									if (entry.Value == value && entry.Name == hit.ValueName)
										++hit.Count;
				}
				int results = 0;
				if (member.Kind == ScriptApiMemberKind::ProxyField)
				{
					const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
					const auto* field = storage.Reflection->GetComponents()[proxy.ComponentTypeIndex]->FindField(member.Name);
					if (write)
					{
						auto status = ScriptProxy::WriteField(call, proxy, member.Name, Lua::CheckValue(call, 2, *field));
						if (!status)
							return Lua::RaiseError(call, status.error());
					}
					else
					{
						auto value = ScriptProxy::ReadField(call, proxy, member.Name);
						if (!value)
							return Lua::RaiseError(call, value.error());
						Lua::PushValue(call, *value, *field);
						results = 1;
					}
				}
				else if (member.ComponentTypeIndex && group.Name == "Entity")
				{
					if (!call.Engine)
						return Lua::RaiseError(call, "requires an active script engine");
					auto proxy = ScriptProxy::GetShortcut(call.Engine->GetHost(), Lua::Check<ScriptEntityIdentity>(call, 1), member.Name);
					if (!proxy)
						return Lua::RaiseError(call, proxy.error());
					if (*proxy)
						Lua::Push(call, **proxy);
					else
						Lua::PushNil(call);
					results = 1;
				}
				else
				{
					ScriptNativeFunction function = write ? member.Setter : member.Function;
					if (!function)
						return Lua::RaiseError(call, "member is not a native callable");
					results = function(call);
				}
				if (call.Engine)
				{
					// A nested lifecycle callback may have ended the case while the outer native operation was
					// running. A yieldable caller must suspend here; a catchable error would let raw Luau continue.
					const auto executable = ScriptEngineAccess::CheckExecution(call);
					if (!executable)
						return lua_isyieldable(call.State) ? lua_yield(call.State, 0) : Lua::RaiseError(call, executable.error());
				}
				if (counter && member.Kind == ScriptApiMemberKind::Property)
				{
					if (write)
						++counter->Writes;
					else
						++counter->Reads;
				}
				return results;
			}
			static int Dispatch(lua_State* state)
			{
				auto call = Call(state);
				const auto& storage = *SandboxAccess::GetApi(*call.Sandbox)->m_Storage;
				const bool type = lua_toboolean(state, lua_upvalueindex(1)) != 0;
				const auto& group = (type ? storage.Types : storage.Modules)[static_cast<size_t>(lua_tointeger(state, lua_upvalueindex(2)))];
				return Invoke(call, group, group.Members[static_cast<size_t>(lua_tointeger(state, lua_upvalueindex(3)))], false);
			}
			static void PushDispatch(lua_State* state, bool type, size_t group, size_t member)
			{
				lua_pushboolean(state, type);
				lua_pushinteger(state, static_cast<int>(group));
				lua_pushinteger(state, static_cast<int>(member));
				lua_pushcclosure(state, Dispatch, "Engine.Api", 3);
			}
			static int Index(lua_State* state) { return Access(state, false, false); }
			static int NewIndex(lua_State* state) { return Access(state, true, false); }
			static int Namecall(lua_State* state) { return Access(state, false, true); }
			static int Access(lua_State* state, bool write, bool namecall)
			{
				auto call = Call(state);
				const size_t groupIndex = static_cast<size_t>(lua_tointeger(state, lua_upvalueindex(1)));
				const auto& group = SandboxAccess::GetApi(*call.Sandbox)->m_Storage->Types[groupIndex];
				int atom = -1;
				const char* method = namecall ? lua_namecallatom(state, &atom) : nullptr;
				const std::string name = namecall && method ? std::string(method) : Lua::Check<std::string>(call, 2);
				for (size_t i = 0; i < group.Members.size(); ++i)
				{
					const auto& member = group.Members[i];
					if (member.Name != name)
						continue;
					if (member.Kind == ScriptApiMemberKind::Property || member.Kind == ScriptApiMemberKind::ProxyField)
					{
						if (namecall)
							return Lua::RaiseError(call, "property cannot be called as a method");
						lua_remove(state, 2);
						return Invoke(call, group, member, write);
					}
					if (member.Kind == ScriptApiMemberKind::Method || member.Kind == ScriptApiMemberKind::Operator)
					{
						if (write)
							return Lua::RaiseError(call, "methods are read-only");
						if (namecall)
							return Invoke(call, group, member, false);
						PushDispatch(state, true, groupIndex, i);
						return 1;
					}
				}
				return Lua::RaiseError(call, std::format("unknown {} member '{}'", group.Name, name));
			}
			static int16_t Atom(lua_State* state, const char* text, size_t length)
			{
				Sandbox* sandbox = SandboxAccess::FromState(state);
				if (!sandbox)
					return -1;
				auto* api = SandboxAccess::GetApi(*sandbox);
				if (!api)
					return -1;
				int id = 0;
				for (const auto& group : api->GetTypes())
					for (const auto& member : group.Members)
					{
						if (id > std::numeric_limits<int16_t>::max())
							return -1;
						if (member.Name == std::string_view(text, length))
							return static_cast<int16_t>(id);
						++id;
					}
				return -1;
			}
		};

		int GetScriptValueTag(const ScriptApiRegistry& api, std::string_view type)
		{
			if (type == "Entity")
				return std::to_underlying(ScriptValueTag::Entity);
			if (type == "Quat")
				return std::to_underlying(ScriptValueTag::Quat);
			if (type == "Color")
				return std::to_underlying(ScriptValueTag::Color);
			if (type == "AssetRef")
				return std::to_underlying(ScriptValueTag::AssetRef);
			if (type == "RandomGenerator")
				return std::to_underlying(ScriptValueTag::RandomGenerator);
			if (type == "TaskHandle")
				return std::to_underlying(ScriptValueTag::TaskHandle);
			if (type == "FieldDescriptor")
				return std::to_underlying(ScriptValueTag::Field);
			const auto types = api.GetTypes();
			for (size_t i = 0; i < types.size(); ++i)
				if (types[i].Name == type)
					return std::to_underlying(ScriptValueTag::ComponentBegin) + static_cast<int>(i);
			return -1;
		}

	}

	ScriptApiRegistry::ScriptApiRegistry()
		: m_Storage(CreateScope<Storage>())
	{
	}
	ScriptApiRegistry::~ScriptApiRegistry() = default;
	ScriptModuleBuilder ScriptApiRegistry::Module(std::string_view name, std::string_view description)
	{
		ENGINE_CORE_ASSERT(!m_Storage->Frozen, "Script registration after Freeze");
		ScriptModuleBuilder builder;
		if (m_Storage->Frozen)
			return builder;
		auto& group = Utils::Group(m_Storage->Modules, name, description, m_Storage->Invalid);
		builder.m_Registry = this;
		builder.m_GroupIndex = static_cast<size_t>(&group - m_Storage->Modules.data());
		return builder;
	}
	ScriptTypeBuilder ScriptApiRegistry::Type(std::string_view name, std::string_view description)
	{
		ENGINE_CORE_ASSERT(!m_Storage->Frozen, "Script registration after Freeze");
		ScriptTypeBuilder builder;
		if (m_Storage->Frozen)
			return builder;
		auto& group = Utils::Group(m_Storage->Types, name, description, m_Storage->Invalid);
		builder.m_Registry = this;
		builder.m_GroupIndex = static_cast<size_t>(&group - m_Storage->Types.data());
		return builder;
	}
	Status ScriptApiRegistry::RegisterEnum(const EnumInfo& enumeration)
	{
		if (IsFrozen())
			return MakeError(ErrorCode::InvalidState, "registry is frozen");
		if (FindEnum(enumeration.GetName()))
			return MakeError(ErrorCode::AlreadyExists, "duplicate enum '{}'", enumeration.GetName());
		if (!Utils::Identifier(enumeration.GetName()) || enumeration.GetDescription().empty() || enumeration.GetEntries().empty())
			return MakeError(ErrorCode::Validation, "invalid enum metadata");
		m_Storage->Enums.push_back({ enumeration.GetName(), enumeration.GetDescription(), { enumeration.GetEntries().begin(), enumeration.GetEntries().end() } });
		return {};
	}
	Status ScriptApiRegistry::RegisterAlias(std::string_view name, std::string_view definition, std::string_view description)
	{
		if (IsFrozen())
			return MakeError(ErrorCode::InvalidState, "registry is frozen");
		for (const auto& item : m_Storage->Aliases)
			if (item.Name == name)
				return MakeError(ErrorCode::AlreadyExists, "duplicate alias '{}'", name);
		if (!Utils::Identifier(name) || definition.empty() || description.empty())
			return MakeError(ErrorCode::Validation, "invalid alias metadata");
		m_Storage->Aliases.push_back({ std::string(name), std::string(definition), std::string(description) });
		return {};
	}
	bool ScriptApiRegistry::IsFrozen() const
	{
		return m_Storage->Frozen;
	}
	std::span<const ScriptApiGroup> ScriptApiRegistry::GetModules() const
	{
		return m_Storage->Modules;
	}
	std::span<const ScriptApiGroup> ScriptApiRegistry::GetTypes() const
	{
		return m_Storage->Types;
	}
	std::span<const ScriptApiEnum> ScriptApiRegistry::GetEnums() const
	{
		return m_Storage->Enums;
	}
	std::span<const ScriptApiAlias> ScriptApiRegistry::GetAliases() const
	{
		return m_Storage->Aliases;
	}
	const ScriptApiEnum* ScriptApiRegistry::FindEnum(std::string_view name) const
	{
		for (const auto& item : m_Storage->Enums)
			if (item.Name == name)
				return &item;
		return nullptr;
	}

	Status ScriptApiRegistry::Freeze(const TypeRegistry& types)
	{
		if (IsFrozen())
			return m_Storage->Reflection == &types ? Status{} : Status(MakeError(ErrorCode::InvalidState, "registry was frozen against other reflected types"));
		if (!types.IsFrozen())
			return MakeError(ErrorCode::InvalidState, "reflection must be frozen first");
		if (!m_Storage->Invalid.empty())
			return MakeError(ErrorCode::Validation, "{}", m_Storage->Invalid);
		Storage candidate = *m_Storage;
		candidate.Reflection = &types;
		for (const auto* enumeration : types.GetEnums())
		{
			for (const auto& item : candidate.Enums)
				if (item.Name == enumeration->GetName())
					return MakeError(ErrorCode::Validation, "duplicate reflected enum '{}'", enumeration->GetName());
			candidate.Enums.push_back({ enumeration->GetName(), enumeration->GetDescription(), { enumeration->GetEntries().begin(), enumeration->GetEntries().end() } });
		}
		for (const auto* component : types.GetComponents())
		{
			if (!component->HasFlag(ComponentFlags::ScriptVisible) || component->HasFlag(ComponentFlags::Hidden) || component->HasFlag(ComponentFlags::EntityLevel))
				continue;
			auto& group = Utils::Group(candidate.Types, component->GetName(), {}, candidate.Invalid);
			if (group.Description.empty())
				group.Description = component->GetDescription();
			const size_t groupIndex = static_cast<size_t>(&group - candidate.Types.data());
			for (const auto& field : component->GetFields())
			{
				if (!field->GetMeta().Scriptable || field->GetMeta().Hidden)
					continue;
				ScriptApiMember member;
				member.Name = field->GetName();
				member.Kind = ScriptApiMemberKind::ProxyField;
				member.Signature = Utils::ReflectedType(field->GetType());
				member.Description = field->GetDescription();
				member.Options.Modes = field->GetMeta().Modes;
				member.Options.Mutates = false;
				member.Readable = true;
				member.Writable = !field->IsReadOnly();
				member.ComponentTypeIndex = component->GetIndex();
				Utils::Append(candidate, true, groupIndex, std::move(member));
			}
			if (!component->HasFlag(ComponentFlags::NoShortcut))
			{
				auto& entity = Utils::Group(candidate.Types, "Entity", {}, candidate.Invalid);
				if (entity.Description.empty())
					entity.Description = "A generation-checked entity identity.";
				ScriptApiMember shortcut;
				shortcut.Name = component->GetName();
				shortcut.Kind = ScriptApiMemberKind::Property;
				shortcut.Signature = component->GetName() + (component->HasFlag(ComponentFlags::Required) ? "" : "?");
				shortcut.Description = "The entity's " + component->GetName() + " component, or nil when absent.";
				shortcut.Options.Mutates = false;
				shortcut.Readable = true;
				shortcut.ComponentTypeIndex = component->GetIndex();
				Utils::Append(candidate, true, static_cast<size_t>(&entity - candidate.Types.data()), std::move(shortcut));
			}
		}
		if (!candidate.Invalid.empty())
			return MakeError(ErrorCode::Validation, "{}", candidate.Invalid);
		// Literal overloads replace the general signature using the authoritative reflected component set.
		for (auto& group : candidate.Types)
			if (group.Name == "Entity")
				for (auto& member : group.Members)
					if (member.Name == "GetComponent")
					{
						std::string overloads;
						for (const auto* component : types.GetComponents())
						{
							if (!component->HasFlag(ComponentFlags::ScriptVisible) || component->HasFlag(ComponentFlags::Hidden) || component->HasFlag(ComponentFlags::EntityLevel))
								continue;
							if (!overloads.empty())
								overloads += " & ";
							overloads += "((self: Entity, name: \"" + component->GetName() + "\") -> " + component->GetName() + "?)";
						}
						if (!overloads.empty())
							member.Signature = std::move(overloads);
					}
		std::set<std::string> known{ "any", "unknown", "never", "nil", "boolean", "number", "string", "thread", "buffer", "vector" };
		for (const auto& group : candidate.Types)
			if (!known.insert(group.Name).second)
				return MakeError(ErrorCode::Validation, "duplicate type '{}'", group.Name);
		for (const auto& item : candidate.Enums)
			if (!known.insert(item.Name).second)
				return MakeError(ErrorCode::Validation, "duplicate type '{}'", item.Name);
		for (const auto& item : candidate.Aliases)
			if (!known.insert(item.Name).second)
				return MakeError(ErrorCode::Validation, "duplicate type '{}'", item.Name);
		for (const auto& alias : candidate.Aliases)
			ENGINE_TRY(Utils::ValidateType(alias.Definition, known));
		for (auto* groups : { &candidate.Modules, &candidate.Types })
		{
			std::ranges::sort(*groups, {}, &ScriptApiGroup::Name);
			for (auto& group : *groups)
			{
				if (!Utils::Identifier(group.Name) || group.Description.empty())
					return MakeError(ErrorCode::Validation, "missing name/description for '{}'", group.Name);
				std::ranges::sort(group.Members, [](const auto& a, const auto& b)
				{
					return std::tie(a.Name, a.Kind) < std::tie(b.Name, b.Kind);
				});
				for (auto& member : group.Members)
				{
					if (!Utils::Identifier(member.Name) || member.Description.empty())
						return MakeError(ErrorCode::Validation, "invalid metadata for {}.{}", group.Name, member.Name);
					if (member.Options.Modes != RunModes::All && member.Options.Modes != RunModes::EditorOnly)
						return MakeError(ErrorCode::Validation, "invalid RunModes for {}.{}", group.Name, member.Name);
					if (member.Options.Environments == ScriptApiEnvironment::None || (std::to_underlying(member.Options.Environments) & ~std::to_underlying(ScriptApiEnvironment::All)) != 0)
						return MakeError(ErrorCode::Validation, "invalid availability for {}.{}", group.Name, member.Name);
					ENGINE_TRY(Utils::ValidateType(member.Signature, known, member.Kind != ScriptApiMemberKind::Constant && member.Kind != ScriptApiMemberKind::Property && member.Kind != ScriptApiMemberKind::ProxyField));
					if (member.Kind == ScriptApiMemberKind::Constant)
					{
						if (!member.ConstantValue || member.ConstantValue->IsNull())
							return MakeError(ErrorCode::Validation, "constant requires a scalar value");
						const auto kind = member.ConstantValue->GetKind();
						if (kind != FieldType::Bool && kind != FieldType::Int32 && kind != FieldType::UInt32 && kind != FieldType::Float && kind != FieldType::String)
							return MakeError(ErrorCode::Validation, "constant requires boolean, number or string");
						if (kind == FieldType::Float && !std::isfinite(member.ConstantValue->AsFloat()))
							return MakeError(ErrorCode::Validation, "constant must be finite");
						if ((kind == FieldType::Bool && member.Signature != "boolean") || (kind == FieldType::String && member.Signature != "string") || (kind != FieldType::Bool && kind != FieldType::String && member.Signature != "number"))
							return MakeError(ErrorCode::Validation, "constant value and declared type disagree");
					}
					if (member.Kind != ScriptApiMemberKind::Constant && member.Kind != ScriptApiMemberKind::Callback && !member.ComponentTypeIndex && !member.Function)
						return MakeError(ErrorCode::Validation, "missing callback for {}.{}", group.Name, member.Name);
					std::set<int> slots;
					for (const auto& parameter : member.Options.EnumParameters)
					{
						if (parameter.ArgumentIndex <= 0 || !slots.insert(parameter.ArgumentIndex).second)
							return MakeError(ErrorCode::Validation, "invalid enum slot for {}.{}", group.Name, member.Name);
						const ScriptApiEnum* enumeration = nullptr;
						for (const auto& item : candidate.Enums)
							if (item.Name == parameter.EnumName)
								enumeration = &item;
						if (!enumeration)
							return MakeError(ErrorCode::Validation, "unknown enum '{}'", parameter.EnumName);
						if (!parameter.DefaultValue.empty() && (!parameter.Optional || !std::ranges::any_of(enumeration->Values, [&](const auto& v)
						{
							return v.Name == parameter.DefaultValue;
						})))
							return MakeError(ErrorCode::Validation, "invalid enum default '{}'", parameter.DefaultValue);
					}
				}
			}
		}
		std::ranges::sort(candidate.Enums, {}, &ScriptApiEnum::Name);
		std::ranges::sort(candidate.Aliases, {}, &ScriptApiAlias::Name);
		if (candidate.Types.size() + std::to_underlying(Detail::ScriptValueTag::ComponentBegin) > LUA_UTAG_LIMIT)
			return MakeError(ErrorCode::Validation, "too many script userdata types");
		for (size_t i = 0; i < candidate.Coverage.size(); ++i)
		{
			auto& coverage = candidate.Coverage[i];
			coverage.Mode = Utils::ScriptModes[i];
			for (const auto* groups : { &candidate.Modules, &candidate.Types })
				for (const auto& group : *groups)
					for (const auto& member : group.Members)
					{
						if (member.Kind == ScriptApiMemberKind::Constant || !HasFlag(member.Options.Modes, coverage.Mode))
							continue;
						ScriptApiCounter counter;
						counter.Owner = group.Name;
						counter.Member = member.Name;
						counter.Kind = member.Kind;
						counter.Modes = member.Options.Modes;
						counter.RequiresRead = member.Readable;
						counter.RequiresWrite = member.Writable;
						for (const auto& parameter : member.Options.EnumParameters)
							for (const auto& enumeration : candidate.Enums)
								if (enumeration.Name == parameter.EnumName && enumeration.Values.size() <= 16)
									for (const auto& entry : enumeration.Values)
										counter.EnumValues.push_back({ parameter.ArgumentIndex, enumeration.Name, entry.Name, 0 });
						coverage.Members.push_back(std::move(counter));
					}
			std::ranges::sort(coverage.Members, [](const auto& a, const auto& b)
			{
				return std::tie(a.Owner, a.Member, a.Kind) < std::tie(b.Owner, b.Member, b.Kind);
			});
		}
		candidate.Frozen = true;
		*m_Storage = std::move(candidate);
		return {};
	}

	Status ScriptApiRegistry::Bind(ScriptCall& call, ScriptApiEnvironment environment, RunModes mode)
	{
		if (!IsFrozen())
			return MakeError(ErrorCode::InvalidState, "script registry is not frozen");
		if (!call.State || !call.Sandbox || Utils::ModeIndex(mode) < 0 || (environment != ScriptApiEnvironment::LoadTime && environment != ScriptApiEnvironment::Runtime && environment != ScriptApiEnvironment::Test) || (environment == ScriptApiEnvironment::Test && !call.Engine))
			return MakeError(ErrorCode::InvalidArgument, "invalid script binding context");
		lua_State* state = call.State;
		lua_pushinteger(state, std::to_underlying(environment));
		lua_rawsetfield(state, LUA_REGISTRYINDEX, "Engine.Api.Environment");
		lua_pushinteger(state, std::to_underlying(mode));
		lua_rawsetfield(state, LUA_REGISTRYINDEX, "Engine.Api.Mode");
		Detail::InitializeBindingMetadata(call);
		lua_callbacks(state)->useratom = Detail::ScriptRegistryAccess::Atom;
		for (size_t i = 0; i < m_Storage->Types.size(); ++i)
		{
			const auto& group = m_Storage->Types[i];
			lua_newtable(state);
			lua_pushlstring(state, group.Name.data(), group.Name.size());
			lua_setfield(state, -2, "__type");
			lua_pushliteral(state, "locked");
			lua_setfield(state, -2, "__metatable");
			for (const auto& [name, function] : std::array<std::pair<const char*, lua_CFunction>, 3>{ { { "__index", Detail::ScriptRegistryAccess::Index }, { "__newindex", Detail::ScriptRegistryAccess::NewIndex }, { "__namecall", Detail::ScriptRegistryAccess::Namecall } } })
			{
				lua_pushinteger(state, static_cast<int>(i));
				lua_pushcclosure(state, function, name, 1);
				lua_setfield(state, -2, name);
			}
			for (size_t j = 0; j < group.Members.size(); ++j)
				if (group.Members[j].Kind == ScriptApiMemberKind::Operator)
				{
					Detail::ScriptRegistryAccess::PushDispatch(state, true, i, j);
					lua_setfield(state, -2, group.Members[j].Name.c_str());
				}
			lua_setreadonly(state, -1, 1);
			lua_setuserdatametatable(state, Detail::GetScriptValueTag(*this, group.Name));
			if (std::ranges::any_of(group.Members, [](const auto& member)
			{
				return member.Kind == ScriptApiMemberKind::Constructor;
			}))
			{
				lua_newtable(state);
				for (size_t j = 0; j < group.Members.size(); ++j)
					if (group.Members[j].Kind == ScriptApiMemberKind::Constructor)
					{
						Detail::ScriptRegistryAccess::PushDispatch(state, true, i, j);
						lua_setfield(state, -2, group.Members[j].Name.c_str());
					}
				lua_setreadonly(state, -1, 1);
				lua_setglobal(state, group.Name.c_str());
			}
		}
		for (size_t i = 0; i < m_Storage->Modules.size(); ++i)
		{
			const auto& group = m_Storage->Modules[i];
			lua_newtable(state);
			for (size_t j = 0; j < group.Members.size(); ++j)
			{
				const auto& member = group.Members[j];
				if (member.Kind == ScriptApiMemberKind::Constant)
				{
					const auto& value = *member.ConstantValue;
					switch (value.GetKind())
					{
						case FieldType::Bool:   Lua::Push(call, value.AsBool()); break;
						case FieldType::Int32:  Lua::Push(call, value.AsInt32()); break;
						case FieldType::UInt32: Lua::Push(call, value.AsUInt32()); break;
						case FieldType::Float:  Lua::Push(call, value.AsFloat()); break;
						case FieldType::String: Lua::PushString(call, value.AsString()); break;
						default:                return MakeError(ErrorCode::Validation, "invalid constant {}.{}", group.Name, member.Name);
					}
				}
				else
					Detail::ScriptRegistryAccess::PushDispatch(state, false, i, j);
				lua_setfield(state, -2, member.Name.c_str());
			}
			lua_setreadonly(state, -1, 1);
			lua_setglobal(state, group.Name.c_str());
		}
		return {};
	}

	namespace Utils {

		static std::string Policy(const ScriptApiMember& member)
		{
			std::string text = member.Options.Modes == RunModes::EditorOnly ? "EditorOnly" : "All modes";
			text += "; ";
			bool separator = false;
			for (const auto& [environment, name] : std::array<std::pair<ScriptApiEnvironment, const char*>, 3>{ { { ScriptApiEnvironment::LoadTime, "LoadTime" }, { ScriptApiEnvironment::Runtime, "Runtime" }, { ScriptApiEnvironment::Test, "Test" } } })
				if (HasFlag(member.Options.Environments, environment))
				{
					if (separator)
						text += "|";
					text += name;
					separator = true;
				}
			text += member.Options.Mutates ? "; mutates" : "; pure/read";
			if (member.Writable)
				text += member.Options.SetterMutates ? "; setter mutates" : "; local setter";
			return text;
		}
		static std::string Comment(std::string_view text)
		{
			std::string result;
			for (char c : text)
			{
				result += c;
				if (c == '\n')
					result += "-- ";
			}
			return result;
		}
		static std::string Literal(std::string_view text)
		{
			std::string result = "\"";
			for (char c : text)
			{
				if (c == '\\' || c == '"')
					result += '\\';
				if (c == '\n')
					result += "\\n";
				else if (c == '\r')
					result += "\\r";
				else
					result += c;
			}
			return result + "\"";
		}
		static std::string_view TypeSource(std::string_view source, const Luau::Location& location)
		{
			const auto offset = [source](Luau::Position position)
			{
				size_t start = 0;
				for (unsigned int line = 0; line < position.line; ++line)
				{
					const size_t next = source.find('\n', start);
					if (next == std::string_view::npos)
						return source.size();
					start = next + 1;
				}
				return std::min(start + position.column, source.size());
			};
			const size_t begin = offset(location.begin);
			return source.substr(begin, offset(location.end) - begin);
		}
		static void CallableParts(const Luau::AstType* type, std::vector<const Luau::AstTypeFunction*>& functions)
		{
			if (const auto* function = type->as<Luau::AstTypeFunction>())
				functions.push_back(function);
			else if (const auto* group = type->as<Luau::AstTypeGroup>())
				CallableParts(group->type, functions);
			else if (const auto* intersection = type->as<Luau::AstTypeIntersection>())
				for (const auto* part : intersection->types)
					CallableParts(part, functions);
		}
		static Result<std::string> ExternMember(const ScriptApiGroup& group, const ScriptApiMember& member, std::set<std::string>& names, std::string& aliases)
		{
			Luau::Allocator allocator;
			Luau::AstNameTable astNames(allocator);
			const auto parsed = Luau::Parser::parseType(member.Signature.data(), member.Signature.size(), astNames, allocator);
			if (!parsed.root || !parsed.errors.empty())
				return MakeError(ErrorCode::Validation, "invalid frozen signature {}.{}", group.Name, member.Name);
			TypeNames types;
			parsed.root->visit(&types);
			if (types.HasGenerics)
			{
				// Luau 0.741 cannot declare generic extern methods, and its inline extern property visitor loses
				// the generic function scope. A generated top-level alias keeps that scope and the exact signature.
				std::string name = std::format("EngineSignature_{}_{}_{}", group.Name.size(), group.Name, member.Name);
				while (!names.insert(name).second)
					name += '_';
				aliases += "type " + name + " = " + member.Signature + "\n";
				return "\t" + member.Name + ": " + name + "\n";
			}
			std::vector<const Luau::AstTypeFunction*> functions;
			CallableParts(parsed.root, functions);
			if (functions.size() > 1 && CallableType(parsed.root) && std::ranges::all_of(functions, [&group](const auto* function)
			{
				if (function->argTypes.types.size == 0)
					return false;
				const auto* self = function->argTypes.types.data[0]->template as<Luau::AstTypeReference>();
				return self && !self->prefix && self->parameters.size == 0 && group.Name == self->name.value;
			}))
			{
				// Repeated declarations merge into the same callable overload set without asking the New solver
				// to normalize the complete intersection AST (18 component returns otherwise expand exponentially).
				std::string declaration;
				for (const auto* function : functions)
				{
					declaration += "\tfunction " + member.Name + "(self";
					for (size_t i = 1; i < function->argTypes.types.size; ++i)
					{
						const std::string name = i < function->argNames.size && function->argNames.data[i] ? std::string(function->argNames.data[i]->first.value) : std::format("argument{}", i);
						declaration += ", " + name + ": " + std::string(TypeSource(member.Signature, function->argTypes.types.data[i]->location));
					}
					if (const auto* tail = function->argTypes.tailType)
					{
						const auto* variadic = tail->as<Luau::AstTypePackVariadic>();
						declaration += ", ...: " + std::string(TypeSource(member.Signature, variadic ? variadic->variadicType->location : tail->location));
					}
					declaration += "): " + std::string(TypeSource(member.Signature, function->returnTypes->location)) + "\n";
				}
				return declaration;
			}
			return "\t" + member.Name + ": " + member.Signature + "\n";
		}

	}

	Result<std::string> ScriptApiRegistry::GenerateDefinitions() const
	{
		if (!IsFrozen())
			return MakeError(ErrorCode::InvalidState, "script registry is not frozen");
		std::string result = "-- Generated from ScriptApiRegistry and TypeRegistry.\n";
		std::set<std::string> names;
		for (const auto& group : m_Storage->Types)
			names.insert(group.Name);
		for (const auto& item : m_Storage->Enums)
			names.insert(item.Name);
		for (const auto& item : m_Storage->Aliases)
			names.insert(item.Name);
		for (const auto& item : m_Storage->Enums)
		{
			result += "-- " + Utils::Comment(item.Description) + "\nexport type " + item.Name + " = ";
			for (size_t i = 0; i < item.Values.size(); ++i)
			{
				if (i)
					result += " | ";
				result += Utils::Literal(item.Values[i].Name);
			}
			result += "\n\n";
		}
		for (const auto& item : m_Storage->Aliases)
			result += "-- " + Utils::Comment(item.Description) + "\nexport type " + item.Name + " = " + item.Definition + "\n\n";
		for (const auto& group : m_Storage->Types)
		{
			std::string aliases;
			std::string declaration = "-- " + Utils::Comment(group.Description) + "\ndeclare extern type " + group.Name + " with\n";
			for (const auto& member : group.Members)
			{
				if (member.Kind == ScriptApiMemberKind::Constructor)
					continue;
				ENGINE_TRY_ASSIGN(auto property, Utils::ExternMember(group, member, names, aliases));
				declaration += "\t-- " + Utils::Comment(member.Description) + " [" + Utils::Policy(member) + "]\n" + property;
			}
			result += aliases + declaration + "end\n\n";
			if (std::ranges::any_of(group.Members, [](const auto& member)
			{
				return member.Kind == ScriptApiMemberKind::Constructor;
			}))
			{
				result += "declare " + group.Name + ": {\n";
				for (const auto& member : group.Members)
					if (member.Kind == ScriptApiMemberKind::Constructor)
						result += "\t-- " + Utils::Comment(member.Description) + " [" + Utils::Policy(member) + "]\n\t" + member.Name + ": " + member.Signature + ",\n";
				result += "}\n\n";
			}
		}
		for (const auto& group : m_Storage->Modules)
		{
			result += "-- " + Utils::Comment(group.Description) + "\ndeclare " + group.Name + ": {\n";
			for (const auto& member : group.Members)
				result += "\t-- " + Utils::Comment(member.Description) + " [" + Utils::Policy(member) + "]\n\t" + member.Name + ": " + member.Signature + ",\n";
			result += "}\n\n";
		}
		if (result.ends_with("\n\n"))
			result.pop_back();
		return result;
	}

	Result<std::string> ScriptApiRegistry::GenerateDocumentation() const
	{
		if (!IsFrozen())
			return MakeError(ErrorCode::InvalidState, "script registry is not frozen");
		std::string result = "# Script API\n\nGenerated from ScriptApiRegistry and TypeRegistry.\n";
		for (const auto* groups : { &m_Storage->Modules, &m_Storage->Types })
			for (const auto& group : *groups)
			{
				result += "\n## " + group.Name + "\n\n" + group.Description + "\n";
				for (const auto& member : group.Members)
				{
					result += "\n### " + group.Name + "." + member.Name + "\n\n`" + member.Signature + "`\n\n" + member.Description + "\n\n" + Utils::Policy(member) + ".\n";
					for (const auto& parameter : member.Options.EnumParameters)
						result += std::format("\nArgument {}: {}{}{}.\n", parameter.ArgumentIndex, parameter.EnumName, parameter.Optional ? " (optional)" : "", parameter.DefaultValue.empty() ? "" : "; default " + parameter.DefaultValue);
				}
			}
		for (const auto& item : m_Storage->Enums)
		{
			result += "\n## " + item.Name + "\n\n" + item.Description + "\n";
			for (const auto& value : item.Values)
				result += "\n- `" + value.Name + "`: " + value.Description + "\n";
		}
		for (const auto& item : m_Storage->Aliases)
			result += "\n## " + item.Name + "\n\n`" + item.Definition + "`\n\n" + item.Description + "\n";
		return result;
	}

	Result<ScriptApiCoverage> ScriptApiRegistry::GetCoverage(RunModes mode) const
	{
		if (!IsFrozen())
			return MakeError(ErrorCode::InvalidState, "script registry is not frozen");
		const int index = Utils::ModeIndex(mode);
		if (index < 0)
			return MakeError(ErrorCode::InvalidArgument, "coverage needs one concrete run mode");
		return m_Storage->Coverage[static_cast<size_t>(index)];
	}

	void ScriptApiRegistry::ResetCoverage()
	{
		for (auto& coverage : m_Storage->Coverage)
			for (auto& member : coverage.Members)
			{
				member.Calls = 0;
				member.Reads = 0;
				member.Writes = 0;
				for (auto& value : member.EnumValues)
					value.Count = 0;
			}
	}

	Status ScriptApiRegistry::RecordCallback(ScriptCall& call, std::string_view type, std::string_view name, RunModes mode)
	{
		if (!IsFrozen())
			return MakeError(ErrorCode::InvalidState, "script registry is not frozen");
		if (Utils::ModeIndex(mode) < 0)
			return MakeError(ErrorCode::InvalidArgument, "callback coverage needs one concrete run mode");
		auto* counter = Utils::Counter(*m_Storage, mode, type, name);
		if (!counter || counter->Kind != ScriptApiMemberKind::Callback)
			return MakeError(ErrorCode::NotFound, "unknown callback {}.{}", type, name);
		if (Utils::Count(call))
			++counter->Calls;
		return {};
	}

	Status ScriptApiRegistry::RecordProxyAccess(ScriptCall& call, size_t componentTypeIndex, std::string_view field, bool write)
	{
		if (!IsFrozen() || !call.Engine)
			return MakeError(ErrorCode::InvalidState, "proxy coverage requires an active script engine");
		if (componentTypeIndex >= m_Storage->Reflection->GetComponents().size())
			return MakeError(ErrorCode::InvalidArgument, "unknown component type");
		const auto* info = m_Storage->Reflection->GetComponents()[componentTypeIndex];
		auto* counter = Utils::Counter(*m_Storage, call.Engine->GetRunMode(), info->GetName(), field);
		if (!counter || counter->Kind != ScriptApiMemberKind::ProxyField)
			return MakeError(ErrorCode::NotFound, "unknown proxy field {}.{}", info->GetName(), field);
		if (Utils::Count(call))
		{
			if (write)
				++counter->Writes;
			else
				++counter->Reads;
		}
		return {};
	}

	ScriptModuleBuilder& ScriptModuleBuilder::Function(std::string_view name, ScriptNativeFunction function, std::string_view signature, std::string_view description, ScriptMemberOptions options)
	{
		if (m_Registry)
			Utils::Append(*m_Registry->m_Storage, false, m_GroupIndex, { std::string(name), ScriptApiMemberKind::Function, std::string(signature), std::string(description), std::move(options), function });
		return *this;
	}
	ScriptModuleBuilder& ScriptModuleBuilder::Constant(std::string_view name, const Value& value, std::string_view type, std::string_view description, RunModes modes, ScriptApiEnvironment environments)
	{
		if (m_Registry)
		{
			ScriptApiMember member{ std::string(name), ScriptApiMemberKind::Constant, std::string(type), std::string(description), { .Modes = modes, .Environments = environments, .Mutates = false, .SetterMutates = false } };
			member.ConstantValue = value;
			Utils::Append(*m_Registry->m_Storage, false, m_GroupIndex, std::move(member));
		}
		return *this;
	}
	ScriptTypeBuilder& ScriptTypeBuilder::Method(std::string_view name, ScriptNativeFunction function, std::string_view signature, std::string_view description, ScriptMemberOptions options)
	{
		if (m_Registry)
			Utils::Append(*m_Registry->m_Storage, true, m_GroupIndex, { std::string(name), ScriptApiMemberKind::Method, std::string(signature), std::string(description), std::move(options), function });
		return *this;
	}
	ScriptTypeBuilder& ScriptTypeBuilder::Property(std::string_view name, ScriptNativeFunction getter, ScriptNativeFunction setter, std::string_view type, std::string_view description, ScriptMemberOptions options)
	{
		if (m_Registry)
			Utils::Append(*m_Registry->m_Storage, true, m_GroupIndex, { std::string(name), ScriptApiMemberKind::Property, std::string(type), std::string(description), std::move(options), getter, setter, true, setter != nullptr });
		return *this;
	}
	ScriptTypeBuilder& ScriptTypeBuilder::Operator(std::string_view name, ScriptNativeFunction function, std::string_view signature, std::string_view description, ScriptMemberOptions options)
	{
		if (m_Registry)
			Utils::Append(*m_Registry->m_Storage, true, m_GroupIndex, { std::string(name), ScriptApiMemberKind::Operator, std::string(signature), std::string(description), std::move(options), function });
		return *this;
	}
	ScriptTypeBuilder& ScriptTypeBuilder::Constructor(std::string_view name, ScriptNativeFunction function, std::string_view signature, std::string_view description, ScriptMemberOptions options)
	{
		if (m_Registry)
			Utils::Append(*m_Registry->m_Storage, true, m_GroupIndex, { std::string(name), ScriptApiMemberKind::Constructor, std::string(signature), std::string(description), std::move(options), function });
		return *this;
	}
	ScriptTypeBuilder& ScriptTypeBuilder::Callback(std::string_view name, std::string_view signature, std::string_view description, ScriptMemberOptions options)
	{
		if (m_Registry)
			Utils::Append(*m_Registry->m_Storage, true, m_GroupIndex, { std::string(name), ScriptApiMemberKind::Callback, std::string(signature), std::string(description), std::move(options) });
		return *this;
	}

}
