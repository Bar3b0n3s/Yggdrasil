#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Scripting/Private/NativeConversionBudget.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptHost.h"

#include <lualib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace Engine {

	namespace Utils {

		static constexpr const char* BindingMetadataKey = "Engine.BindingMetadata";

		static void CheckText(ScriptCall& call, std::string_view text, bool empty = false)
		{
			if ((!empty && text.empty()) || text.find('\0') != std::string_view::npos || !IsValidUtf8(text))
				Lua::RaiseError(call, "metadata requires nonempty UTF-8 without NUL");
		}
		static void RequireTable(ScriptCall& call, int index)
		{
			if (!lua_istable(call.State, index))
				Lua::RaiseError(call, "expected a table");
		}
		static void StoreRecord(ScriptCall& call, int identity, int record)
		{
			identity = lua_absindex(call.State, identity);
			record = lua_absindex(call.State, record);
			lua_rawgetfield(call.State, LUA_REGISTRYINDEX, BindingMetadataKey);
			lua_pushvalue(call.State, identity);
			lua_pushvalue(call.State, record);
			lua_rawset(call.State, -3);
			lua_pop(call.State, 1);
		}
		static bool PushRecord(ScriptCall& call, int identity)
		{
			identity = lua_absindex(call.State, identity);
			lua_rawgetfield(call.State, LUA_REGISTRYINDEX, BindingMetadataKey);
			lua_pushvalue(call.State, identity);
			lua_rawget(call.State, -2);
			lua_remove(call.State, -2);
			if (lua_istable(call.State, -1))
				return true;
			lua_pop(call.State, 1);
			return false;
		}
		static void SetString(ScriptCall& call, int record, const char* name, std::string_view value)
		{
			record = lua_absindex(call.State, record);
			Lua::PushString(call, value);
			lua_rawsetfield(call.State, record, name);
		}
		static std::string GetString(ScriptCall& call, int record, const char* name)
		{
			lua_rawgetfield(call.State, record, name);
			const auto value = Lua::Check<std::string>(call, -1);
			lua_pop(call.State, 1);
			return value;
		}
		static std::string ReadEncodedFields(ScriptCall& call, int record, const char* name, Detail::NativeConversionBudget& budget)
		{
			lua_rawgetfield(call.State, record, name);
			auto encoded = budget.ReadString(-1);
			lua_pop(call.State, 1);
			// These authenticated records contain our own compact JSON. Reserve its native parse tree before parsing:
			// every byte outside strings costs a node, and every string costs a node plus the bytes charged above.
			// This deliberately overcounts punctuation/numbers and covers container entries and subsequent schema copies.
			bool quoted = false;
			bool escaped = false;
			size_t offset = 0;
			for (const char byte : encoded)
			{
				if (offset++ % 256 == 0)
					budget.CheckDeadline();
				if (!quoted)
					budget.Check(Detail::NativeConversionBudget::ValueBytes);
				if (escaped)
					escaped = false;
				else if (quoted && byte == '\\')
					escaped = true;
				else if (byte == '"')
					quoted = !quoted;
			}
			return encoded;
		}
		static void SetInteger(ScriptCall& call, int record, const char* name, uint32_t value)
		{
			record = lua_absindex(call.State, record);
			Lua::Push(call, value);
			lua_rawsetfield(call.State, record, name);
		}
		static uint32_t GetInteger(ScriptCall& call, int record, const char* name)
		{
			lua_rawgetfield(call.State, record, name);
			const auto value = Lua::Check<uint32_t>(call, -1);
			lua_pop(call.State, 1);
			return value;
		}
		static void SetLocation(ScriptCall& call, int record)
		{
			const auto location = Detail::ScriptCallerLocation(call);
			SetString(call, record, "File", location.File);
			SetInteger(call, record, "Line", location.Line);
			SetInteger(call, record, "Column", location.Column);
			if (location.JsonPointer)
				SetString(call, record, "Pointer", *location.JsonPointer);
		}
		static ErrorLocation GetLocation(ScriptCall& call, int record)
		{
			ErrorLocation location;
			location.File = GetString(call, record, "File");
			location.Line = GetInteger(call, record, "Line");
			location.Column = GetInteger(call, record, "Column");
			lua_rawgetfield(call.State, record, "Pointer");
			if (!lua_isnil(call.State, -1))
				location.JsonPointer = Lua::Check<std::string>(call, -1);
			lua_pop(call.State, 1);
			return location;
		}
		static Json EncodeField(const ScriptFieldSchema& schema, Detail::NativeConversionBudget& budget)
		{
			budget.CheckDeadline();
			Json value = { { "Name", schema.Name }, { "Type", static_cast<uint32_t>(schema.Type) }, { "Default", schema.DefaultValue.Get() }, { "Tooltip", schema.Tooltip }, { "AssetFilter", schema.Meta.AssetFilter }, { "Enum", schema.EnumValues } };
			if (schema.Meta.Min)
				value["Min"] = *schema.Meta.Min;
			if (schema.Meta.Max)
				value["Max"] = *schema.Meta.Max;
			if (schema.Meta.Step)
				value["Step"] = *schema.Meta.Step;
			if (schema.Element)
				value["Element"] = EncodeField(*schema.Element, budget);
			return value;
		}
		static Result<ScriptFieldSchema> DecodeField(const JsonReader& reader, Detail::NativeConversionBudget& budget, size_t depth = 0)
		{
			ENGINE_TRY(budget.Poll());
			if (depth >= 64)
				return MakeError(ErrorCode::Validation, "field schema exceeds 64 levels");
			ScriptFieldSchema schema;
			ENGINE_TRY_ASSIGN(schema.Name, reader.ReadMember<std::string>("Name"));
			ENGINE_TRY_ASSIGN(const auto kind, reader.ReadMember<uint32_t>("Type"));
			schema.Type = static_cast<FieldType>(kind);
			ENGINE_TRY_ASSIGN(const auto defaultValue, reader.GetMember("Default"));
			schema.DefaultValue.Set(defaultValue.GetValue());
			ENGINE_TRY_ASSIGN(schema.Tooltip, reader.ReadMember<std::string>("Tooltip"));
			ENGINE_TRY_ASSIGN(schema.Meta.AssetFilter, reader.ReadMember<std::string>("AssetFilter"));
			if (const auto field = reader.FindMember("Min"))
			{
				ENGINE_TRY_ASSIGN(schema.Meta.Min, field->ReadDouble());
			}
			if (const auto field = reader.FindMember("Max"))
			{
				ENGINE_TRY_ASSIGN(schema.Meta.Max, field->ReadDouble());
			}
			if (const auto field = reader.FindMember("Step"))
			{
				ENGINE_TRY_ASSIGN(schema.Meta.Step, field->ReadDouble());
			}
			ENGINE_TRY_ASSIGN(const auto values, reader.GetMember("Enum"));
			ENGINE_TRY_ASSIGN(const auto count, values.GetArraySize());
			for (size_t i = 0; i < count; ++i)
			{
				ENGINE_TRY(budget.Poll());
				ENGINE_TRY_ASSIGN(const auto item, values.GetElement(i));
				ENGINE_TRY_ASSIGN(auto name, item.ReadString());
				schema.EnumValues.push_back(std::move(name));
			}
			if (const auto element = reader.FindMember("Element"))
			{
				ENGINE_TRY_ASSIGN(auto child, DecodeField(*element, budget, depth + 1));
				schema.Element = CreateRef<const ScriptFieldSchema>(std::move(child));
			}
			return schema;
		}
		static ScriptFieldSchema ReadFieldDescriptor(ScriptCall& call, int index, Detail::NativeConversionBudget& budget)
		{
			if (lua_userdatatag(call.State, index) != std::to_underlying(Detail::ScriptValueTag::Field) || !PushRecord(call, index))
				Lua::RaiseError(call, "expected an authenticated Field descriptor");
			const auto encoded = ReadEncodedFields(call, -1, "Schema", budget);
			lua_pop(call.State, 1);
			auto json = JsonReader::Parse(encoded);
			budget.CheckDeadline();
			if (!json)
				Lua::RaiseError(call, json.error());
			auto schema = DecodeField(JsonReader(*json), budget);
			if (!schema)
				Lua::RaiseError(call, schema.error());
			return std::move(*schema);
		}
		static Status ValidateFields(std::string_view name, const std::vector<ScriptFieldSchema>& fields)
		{
			auto script = CreateRef<ScriptData>();
			script->Kind = ScriptKind::Behaviour;
			script->Name = name;
			script->Fields = fields;
			auto snapshot = ScriptFieldSchemaSource::Create({ { AssetHandle(1), std::move(script) } });
			if (!snapshot)
				return std::unexpected(std::move(snapshot).error());
			return {};
		}
		static void ReadOptions(ScriptCall& call, int index, ScriptFieldSchema& schema, Detail::NativeConversionBudget& budget)
		{
			if (Lua::IsNoneOrNil(call, index))
				return;
			RequireTable(call, index);
			index = lua_absindex(call.State, index);
			lua_pushnil(call.State);
			while (lua_next(call.State, index))
			{
				budget.Check(Detail::NativeConversionBudget::EntryBytes);
				const auto key = budget.ReadString(-2);
				if (key == "Min")
					schema.Meta.Min = Lua::Check<double>(call, -1);
				else if (key == "Max")
					schema.Meta.Max = Lua::Check<double>(call, -1);
				else if (key == "Step")
					schema.Meta.Step = Lua::Check<double>(call, -1);
				else if (key == "Tooltip")
				{
					schema.Tooltip = budget.ReadString(-1);
					CheckText(call, schema.Tooltip, true);
				}
				else
					Lua::RaiseError(call, "unknown Field option '" + key + "'");
				lua_pop(call.State, 1);
			}
		}
		template<FieldType Kind>
		static int FieldConstructor(ScriptCall& call)
		{
			Detail::NativeConversionBudget budget(call);
			budget.Check(Detail::NativeConversionBudget::ValueBytes);
			ScriptFieldSchema schema;
			schema.Type = Kind;
			Json value = nullptr;
			if constexpr (Kind == FieldType::Float)
				value = Lua::IsNoneOrNil(call, 1) ? 0.0f : Lua::Check<float>(call, 1);
			else if constexpr (Kind == FieldType::Int32)
				value = Lua::IsNoneOrNil(call, 1) ? 0 : Lua::Check<int32_t>(call, 1);
			else if constexpr (Kind == FieldType::Bool)
				value = !Lua::IsNoneOrNil(call, 1) && Lua::Check<bool>(call, 1);
			else if constexpr (Kind == FieldType::String)
				value = Lua::IsNoneOrNil(call, 1) ? std::string{} : budget.ReadString(1);
			else if constexpr (Kind == FieldType::Vec3)
			{
				const auto vector = Lua::IsNoneOrNil(call, 1) ? glm::vec3(0.0f) : Lua::Check<glm::vec3>(call, 1);
				value = { vector.x, vector.y, vector.z };
			}
			else if constexpr (Kind == FieldType::Color4)
			{
				const auto color = Lua::IsNoneOrNil(call, 1) ? glm::vec4(1.0f) : Lua::Check<glm::vec4>(call, 1);
				value = { color.r, color.g, color.b, color.a };
			}
			else if constexpr (Kind == FieldType::Quat)
			{
				const auto quat = Lua::IsNoneOrNil(call, 1) ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : Lua::Check<glm::quat>(call, 1);
				value = { quat.x, quat.y, quat.z, quat.w };
			}
			else if constexpr (Kind == FieldType::AssetRef)
			{
				schema.Meta.AssetFilter = budget.ReadString(1);
			}
			else if constexpr (Kind == FieldType::Enum)
			{
				RequireTable(call, 1);
				const int count = lua_objlen(call.State, 1);
				std::set<std::string> seen;
				int entries = 0;
				lua_pushnil(call.State);
				while (lua_next(call.State, 1))
				{
					budget.Check(Detail::NativeConversionBudget::EntryBytes);
					const auto key = Lua::Check<uint32_t>(call, -2);
					if (key == 0 || key > static_cast<uint32_t>(count))
						Lua::RaiseError(call, "enum values must be a dense sequence");
					++entries;
					lua_pop(call.State, 1);
				}
				if (entries != count || count == 0)
					return Lua::RaiseError(call, "enum values must be a nonempty dense sequence");
				for (int i = 1; i <= count; ++i)
				{
					budget.Check(Detail::NativeConversionBudget::ValueBytes);
					lua_rawgeti(call.State, 1, i);
					auto name = budget.ReadString(-1);
					CheckText(call, name);
					if (!seen.insert(name).second)
						return Lua::RaiseError(call, "duplicate enum value");
					schema.EnumValues.push_back(std::move(name));
					lua_pop(call.State, 1);
				}
				value = Lua::IsNoneOrNil(call, 2) ? schema.EnumValues.front() : budget.ReadString(2);
			}
			else if constexpr (Kind == FieldType::Array)
			{
				auto child = ReadFieldDescriptor(call, 1, budget);
				schema.Element = CreateRef<const ScriptFieldSchema>(std::move(child));
				value = Json::array();
			}
			constexpr bool HasOptions = Kind == FieldType::Float || Kind == FieldType::Int32 || Kind == FieldType::Bool || Kind == FieldType::String || Kind == FieldType::Vec3 || Kind == FieldType::Color4 || Kind == FieldType::Quat;
			if constexpr (HasOptions)
				ReadOptions(call, 2, schema, budget);
			constexpr int MaxArguments = HasOptions || Kind == FieldType::Enum ? 2 : (Kind == FieldType::EntityRef ? 0 : 1);
			if (Lua::GetArgumentCount(call) > MaxArguments)
				return Lua::RaiseError(call, "too many Field constructor arguments");
			schema.DefaultValue.Set(std::move(value));
			schema.Name = "Value";
			budget.CheckDeadline();
			auto valid = ValidateFields("Field", { schema });
			budget.CheckDeadline();
			if (!valid)
				return Lua::RaiseError(call, valid.error());
			schema.Name.clear();
			const auto encoded = EncodeField(schema, budget).dump();
			budget.CheckDeadline();
			static_cast<void>(lua_newuserdatataggedwithmetatable(call.State, 1, std::to_underlying(Detail::ScriptValueTag::Field)));
			const int identity = lua_gettop(call.State);
			lua_newtable(call.State);
			SetString(call, -1, "Schema", encoded);
			StoreRecord(call, identity, -1);
			lua_pop(call.State, 1);
			return 1;
		}
		static int Define(ScriptCall& call)
		{
			Detail::NativeConversionBudget budget(call);
			budget.Check(Detail::NativeConversionBudget::ValueBytes);
			const auto name = budget.ReadString(1);
			CheckText(call, name);
			RequireTable(call, 2);
			if (PushRecord(call, 2))
				return Lua::RaiseError(call, "class table is already registered");
			std::vector<ScriptFieldSchema> fields;
			lua_rawgetfield(call.State, 2, "Fields");
			if (!lua_isnil(call.State, -1))
			{
				RequireTable(call, -1);
				const int table = lua_gettop(call.State);
				lua_pushnil(call.State);
				while (lua_next(call.State, table))
				{
					budget.Check(Detail::NativeConversionBudget::EntryBytes);
					const auto fieldName = budget.ReadString(-2);
					CheckText(call, fieldName);
					auto field = ReadFieldDescriptor(call, -1, budget);
					field.Name = fieldName;
					fields.push_back(std::move(field));
					lua_pop(call.State, 1);
				}
			}
			lua_pop(call.State, 1);
			std::ranges::sort(fields, {}, &ScriptFieldSchema::Name);
			budget.CheckDeadline();
			auto valid = ValidateFields(name, fields);
			budget.CheckDeadline();
			if (!valid)
				return Lua::RaiseError(call, valid.error());
			Json schemas = Json::array();
			for (const auto& field : fields)
				schemas.push_back(EncodeField(field, budget));
			const auto encoded = schemas.dump();
			budget.CheckDeadline();
			lua_newtable(call.State);
			const int record = lua_gettop(call.State);
			SetInteger(call, record, "Kind", static_cast<uint32_t>(ScriptKind::Behaviour));
			SetString(call, record, "Name", name);
			SetString(call, record, "Fields", encoded);
			SetLocation(call, record);
			StoreRecord(call, 2, record);
			lua_pop(call.State, 1);
			lua_pushvalue(call.State, 2);
			return 1;
		}

	}

	namespace Detail {

		Status ValidateBehaviourAssignment(IScriptHost& host, AssetHandle handle)
		{
			if (!handle.IsValid())
				return {};
			auto* assets = host.GetAssets();
			if (!assets)
				return MakeError(ErrorCode::InvalidState, "script assignment requires the asset service");
			ENGINE_TRY_ASSIGN(auto loaded, assets->Load(handle));
			const auto script = AssetCast<ScriptData>(loaded);
			if (!script || script->Kind != ScriptKind::Behaviour)
				return MakeError(ErrorCode::Validation, "a Script component requires a Behaviour asset");
			return {};
		}

		void InitializeBindingMetadata(ScriptCall& call)
		{
			lua_rawgetfield(call.State, LUA_REGISTRYINDEX, Utils::BindingMetadataKey);
			const bool exists = lua_istable(call.State, -1);
			lua_pop(call.State, 1);
			if (exists)
				return;
			lua_newtable(call.State);
			lua_newtable(call.State);
			lua_pushliteral(call.State, "k");
			lua_rawsetfield(call.State, -2, "__mode");
			lua_setmetatable(call.State, -2);
			lua_rawsetfield(call.State, LUA_REGISTRYINDEX, Utils::BindingMetadataKey);
		}

		Status PushScriptRegistrationRecord(ScriptCall& call, int tableIndex)
		{
			if (!lua_istable(call.State, tableIndex))
				return MakeError(ErrorCode::InvalidArgument, "metadata identity must be a table");
			if (!Utils::PushRecord(call, tableIndex))
				lua_pushnil(call.State);
			return {};
		}

		Status SetScriptRegistrationRecord(ScriptCall& call, int tableIndex, int recordIndex)
		{
			if (!lua_istable(call.State, tableIndex) || (!lua_istable(call.State, recordIndex) && !lua_isnil(call.State, recordIndex)))
				return MakeError(ErrorCode::InvalidArgument, "metadata restoration requires a table identity and an opaque record or nil");
			tableIndex = lua_absindex(call.State, tableIndex);
			recordIndex = lua_absindex(call.State, recordIndex);
			if (lua_isnil(call.State, recordIndex))
			{
				// A failed first installation may have allocated no node. Rollback of its previous nil must not
				// attempt lua_rawset on that missing key, which can itself allocate under memory recovery.
				if (!Utils::PushRecord(call, tableIndex))
					return {};
				lua_pop(call.State, 1);
			}
			Utils::StoreRecord(call, tableIndex, recordIndex);
			return {};
		}

		Result<ScriptRegistration> InspectScriptRegistration(ScriptCall& call, int index)
		{
			NativeConversionBudget budget(call);
			ScriptRegistration registration;
			if (!lua_istable(call.State, index) || !Utils::PushRecord(call, index))
				return registration;
			const int record = lua_gettop(call.State);
			registration.Kind = static_cast<ScriptKind>(Utils::GetInteger(call, record, "Kind"));
			lua_rawgetfield(call.State, record, "Name");
			registration.Name = budget.ReadString(-1);
			lua_pop(call.State, 1);
			registration.Location = Utils::GetLocation(call, record);
			if (registration.Kind == ScriptKind::Behaviour)
			{
				const auto encoded = Utils::ReadEncodedFields(call, record, "Fields", budget);
				lua_pop(call.State, 1);
				ENGINE_TRY_ASSIGN(const auto fields, JsonReader::Parse(encoded));
				ENGINE_TRY(budget.Poll());
				JsonReader reader(fields);
				ENGINE_TRY_ASSIGN(const auto count, reader.GetArraySize());
				for (size_t i = 0; i < count; ++i)
				{
					ENGINE_TRY_ASSIGN(const auto item, reader.GetElement(i));
					ENGINE_TRY_ASSIGN(auto field, Utils::DecodeField(item, budget));
					registration.Fields.push_back(std::move(field));
				}
			}
			else
			{
				registration.CaseTimeoutTicks = Utils::GetInteger(call, record, "Timeout");
				lua_pop(call.State, 1);
			}
			ENGINE_TRY(budget.Poll());
			return registration;
		}

		Status PushSuiteBody(ScriptCall& call, int index, ScriptCollectedSuite& suite, uint32_t& defaultTimeout)
		{
			if (!lua_istable(call.State, index) || !Utils::PushRecord(call, index))
				return MakeError(ErrorCode::Validation, "expected an authenticated Test.Suite");
			const int record = lua_gettop(call.State);
			if (Utils::GetInteger(call, record, "Kind") != static_cast<uint32_t>(ScriptKind::TestSuite))
			{
				lua_pop(call.State, 1);
				return MakeError(ErrorCode::Validation, "expected an authenticated Test.Suite");
			}
			ScriptCollectedSuite collected;
			collected.Name = Utils::GetString(call, record, "Name");
			const auto location = Utils::GetLocation(call, record);
			collected.File = location.File;
			collected.Line = location.Line;
			const auto timeout = Utils::GetInteger(call, record, "Timeout");
			lua_rawgetfield(call.State, record, "Body");
			lua_remove(call.State, record);
			suite = std::move(collected);
			defaultTimeout = timeout;
			return {};
		}

		int CreateTestSuite(ScriptCall& call)
		{
			const auto name = Lua::Check<std::string>(call, 1);
			Utils::CheckText(call, name);
			if (!lua_isfunction(call.State, 2))
				return Lua::RaiseError(call, "suite body must be a function");
			uint32_t timeout = 0;
			if (!Lua::IsNoneOrNil(call, 3))
			{
				Utils::RequireTable(call, 3);
				lua_pushnil(call.State);
				while (lua_next(call.State, 3))
				{
					const auto option = Lua::Check<std::string>(call, -2);
					if (option != "CaseTimeoutTicks")
						return Lua::RaiseError(call, "unknown suite option '" + option + "'");
					timeout = Lua::Check<uint32_t>(call, -1);
					if (timeout == 0)
						return Lua::RaiseError(call, "CaseTimeoutTicks must be a positive integer");
					lua_pop(call.State, 1);
				}
			}
			lua_newtable(call.State);
			const int suite = lua_gettop(call.State);
			Utils::SetString(call, suite, "Name", name);
			lua_pushvalue(call.State, 2);
			lua_rawsetfield(call.State, suite, "Body");
			lua_newtable(call.State);
			if (timeout)
				Utils::SetInteger(call, -1, "CaseTimeoutTicks", timeout);
			lua_setreadonly(call.State, -1, 1);
			lua_rawsetfield(call.State, suite, "Options");
			lua_setreadonly(call.State, suite, 1);
			lua_newtable(call.State);
			const int record = lua_gettop(call.State);
			Utils::SetInteger(call, record, "Kind", static_cast<uint32_t>(ScriptKind::TestSuite));
			Utils::SetString(call, record, "Name", name);
			Utils::SetInteger(call, record, "Timeout", timeout);
			Utils::SetLocation(call, record);
			lua_pushvalue(call.State, 2);
			lua_rawsetfield(call.State, record, "Body");
			Utils::StoreRecord(call, suite, record);
			lua_pop(call.State, 1);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterScript(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions pure{ .Environments = ScriptApiEnvironment::All, .Mutates = false, .SetterMutates = false };
			ENGINE_TRY(api.RegisterAlias("FieldOptions", "{Min: number?, Max: number?, Step: number?, Tooltip: string?}", "Editor metadata and numeric validation bounds for a field."));
			ENGINE_TRY(api.RegisterAlias("ScriptInstance", "{Entity: Entity}", "Every behaviour instance owns an Entity identity and copies its declared field defaults."));
			static_cast<void>(api.Type("FieldDescriptor", "An authenticated immutable Field constructor descriptor."));
			ENGINE_TRY(api.RegisterAlias("CollisionContact", "{Point: vector, Normal: vector, RelativeSpeed: number, Collider: Entity, OtherCollider: Entity}", "First contact from the receiving body's side, with collider identities for compounds."));
			api.Module("Script", "Behaviour class registration.").Function("Define", Utils::Define, "<T>(name: string, class: T) -> T", "Authenticate a behaviour class and return the exact same table.", pure);
			api.Module("Field", "Persistent behaviour field schemas.")
				.Function("Number", Utils::FieldConstructor<FieldType::Float>, "(default: number?, options: FieldOptions?) -> FieldDescriptor", "Declare a finite f32 number; the omitted default is zero.", pure)
				.Function("Integer", Utils::FieldConstructor<FieldType::Int32>, "(default: number?, options: FieldOptions?) -> FieldDescriptor", "Declare an exact signed 32-bit integer; the omitted default is zero.", pure)
				.Function("Bool", Utils::FieldConstructor<FieldType::Bool>, "(default: boolean?, options: FieldOptions?) -> FieldDescriptor", "Declare a boolean; the omitted default is false.", pure)
				.Function("String", Utils::FieldConstructor<FieldType::String>, "(default: string?, options: FieldOptions?) -> FieldDescriptor", "Declare a string; the omitted default is empty.", pure)
				.Function("Vector", Utils::FieldConstructor<FieldType::Vec3>, "(default: vector?, options: FieldOptions?) -> FieldDescriptor", "Declare a native three-component vector; the omitted default is zero.", pure)
				.Function("Color", Utils::FieldConstructor<FieldType::Color4>, "(default: Color?, options: FieldOptions?) -> FieldDescriptor", "Declare a linear RGBA color; the omitted default is white.", pure)
				.Function("Quat", Utils::FieldConstructor<FieldType::Quat>, "(default: Quat?, options: FieldOptions?) -> FieldDescriptor", "Declare a finite nonzero quaternion; the omitted default is identity.", pure)
				.Function("Entity", Utils::FieldConstructor<FieldType::EntityRef>, "() -> FieldDescriptor", "Declare an Entity reference with a null default.", pure)
				.Function("Asset", Utils::FieldConstructor<FieldType::AssetRef>, "(type: string) -> FieldDescriptor", "Declare an asset reference restricted to the named asset type, with a null default.", pure)
				.Function("Enum", Utils::FieldConstructor<FieldType::Enum>, "(values: {string}, default: string?) -> FieldDescriptor", "Declare ordered unique string choices; the omitted default is the first choice.", pure)
				.Function("Array", Utils::FieldConstructor<FieldType::Array>, "(element: FieldDescriptor) -> FieldDescriptor", "Declare an empty array whose element keeps its complete nested field schema.", pure);
			auto behaviour = api.Type("Behaviour", "Optional behaviour lifecycle callbacks in execution order and canonical entity order.");
			for (const char* name : { "OnCreate", "OnStart", "OnDestroy", "OnEnable", "OnDisable" })
				behaviour.Callback(name, "(self: ScriptInstance) -> ()", "Called at the corresponding lifecycle transition.");
			for (const char* name : { "OnFixedUpdate", "OnUpdate", "OnLateUpdate" })
				behaviour.Callback(name, "(self: ScriptInstance, dt: number) -> ()", "Called for the current fixed, presentation or late presentation phase.");
			behaviour.Callback("OnCollisionEnter", "(self: ScriptInstance, other: Entity, contact: CollisionContact) -> ()", "Called when a collision begins, with the other entity and contact.");
			for (const char* name : { "OnCollisionExit", "OnTriggerEnter", "OnTriggerExit" })
				behaviour.Callback(name, "(self: ScriptInstance, other: Entity) -> ()", "Called when the corresponding collision or trigger transition occurs.");
			behaviour.Callback("OnHotReload", "(self: ScriptInstance) -> ()", "Called after the class is replaced while instance state survives.", { .Modes = RunModes::EditorOnly });
			return {};
		}

	}

}
