#include "EnginePCH.h"
#include "Engine/Scripting/LuaHelpers.h"

#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/Private/NativeConversionBudget.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <lualib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Engine {

	Status ScriptCall::CheckWritable() const
	{
		if (Sandbox == nullptr || Engine == nullptr || Execution == nullptr || Execution->Origin == ScriptExecutionOrigin::Pure)
			return MakeError(ErrorCode::InvalidState, "{} requires an active writable script engine", MemberName);
		ENGINE_TRY(Detail::ScriptEngineAccess::CheckExecution(*this));
		return Detail::SandboxAccess::CheckWritable(*Sandbox);
	}

	Status ScriptCall::PrepareHostMutation() const
	{
		ENGINE_TRY(CheckWritable());
		if ((Execution->Origin == ScriptExecutionOrigin::Eval || Execution->Origin == ScriptExecutionOrigin::TestDriver) && !Execution->ExternalMutationNotified)
		{
			Execution->ExternalMutationNotified = true;
			Engine->GetHost().OnExternalMutation(MemberName);
		}
		return {};
	}

	namespace Utils {

		static void RequireType(ScriptCall& call, int index, int type)
		{
			if (lua_type(call.State, index) != type)
				Lua::RaiseError(call, std::format("argument #{} expected {}, got {}", index, lua_typename(call.State, type), luaL_typename(call.State, index)));
		}

		static void Finite(ScriptCall& call, double value, int index)
		{
			if (!std::isfinite(value))
				Lua::RaiseError(call, std::format("argument #{} contains NaN or infinity", index));
		}

		template<typename T>
		static T& Payload(ScriptCall& call, int index, int tag)
		{
			void* data = lua_touserdatatagged(call.State, index, tag);
			if (data == nullptr || static_cast<size_t>(lua_objlen(call.State, index)) != sizeof(T))
				Lua::RaiseError(call, std::format("argument #{} expected {}, got {}", index, lua_getuserdataname(call.State, tag), luaL_typename(call.State, index)));
			return *static_cast<T*>(data);
		}

		template<typename T>
		static void PushPayload(ScriptCall& call, const T& value, int tag)
		{
			static_assert(std::is_trivially_destructible_v<T>);
			std::construct_at(static_cast<T*>(lua_newuserdatataggedwithmetatable(call.State, sizeof(T), tag)), value);
		}

		static ScriptApiRegistry& Api(ScriptCall& call)
		{
			return *Detail::SandboxAccess::GetApi(*call.Sandbox);
		}

		static ScriptError CaptureFailure(ScriptCall& call, ScriptErrorKind kind);

		static Result<std::string> ReadJsonString(ScriptCall& call, int index, Detail::NativeConversionBudget& budget)
		{
			size_t length = 0;
			const char* text = lua_tolstring(call.State, index, &length);
			// Charge before either scanning or copying a script-owned string, including object keys.
			ENGINE_TRY(budget.Charge(length, Detail::NativeConversionBudget::StringByteCopies));
			if (!IsValidUtf8(std::string_view(text, length)))
				return MakeError(ErrorCode::Script, "JSON string or object key is not valid UTF-8");
			return std::string(text, length);
		}

		static Result<Json> ReadJsonScalar(ScriptCall& call, int index, Detail::NativeConversionBudget& budget)
		{
			lua_State* state = call.State;
			switch (lua_type(state, index))
			{
				case LUA_TNIL:     return Json(nullptr);
				case LUA_TBOOLEAN: return Json(lua_toboolean(state, index) != 0);
				case LUA_TNUMBER:
				{
					const double number = lua_tonumber(state, index);
					if (!std::isfinite(number))
						return MakeError(ErrorCode::Script, "JSON value contains a non-finite number");
					return Json(number);
				}
				case LUA_TSTRING:
				{
					ENGINE_TRY_ASSIGN(auto text, ReadJsonString(call, index, budget));
					return Json(std::move(text));
				}
				default: return MakeError(ErrorCode::Script, "value is not a JSON scalar or table");
			}
		}

		struct JsonReadFrame
		{
			int TableIndex = 0;
			std::map<std::string, Json> Object{};
			std::map<uint32_t, Json> Array{};
			std::string Key{};
			uint32_t ArrayKey = 0;
		};

		static Result<bool> NextJsonEntry(ScriptCall& call, JsonReadFrame& frame, Detail::NativeConversionBudget& budget)
		{
			lua_State* state = call.State;
			if (!lua_next(state, frame.TableIndex))
				return false;
			ENGINE_TRY(budget.Charge(Detail::NativeConversionBudget::EntryBytes));
			if (lua_type(state, -2) == LUA_TSTRING)
			{
				ENGINE_TRY_ASSIGN(frame.Key, ReadJsonString(call, -2, budget));
				frame.ArrayKey = 0;
			}
			else if (lua_type(state, -2) == LUA_TNUMBER)
			{
				const double key = lua_tonumber(state, -2);
				if (!std::isfinite(key) || key < 1 || key > std::numeric_limits<uint32_t>::max() || key != std::floor(key))
					return MakeError(ErrorCode::Script, "JSON array keys must be consecutive positive integers");
				frame.ArrayKey = static_cast<uint32_t>(key);
			}
			else
				return MakeError(ErrorCode::Script, "JSON table keys must be strings or consecutive positive integers");
			return true;
		}

		static Result<Json> FinishJsonFrame(JsonReadFrame& frame, Detail::NativeConversionBudget& budget)
		{
			if (!frame.Object.empty() && !frame.Array.empty())
				return MakeError(ErrorCode::Script, "mixed table keys are not JSON serializable");
			if (!frame.Array.empty())
			{
				Json result = Json::array();
				uint32_t expected = 1;
				for (auto& [key, value] : frame.Array)
				{
					ENGINE_TRY(budget.Poll());
					if (key != expected++)
						return MakeError(ErrorCode::Script, "JSON array indices must be consecutive");
					result.push_back(std::move(value));
				}
				return result;
			}
			Json result = Json::object();
			// ordered_json grows a vector of const-key pairs whose relocation can copy existing JSON values.
			// Create every slot while its value is null, then attach completed subtrees without growing the object.
			for (const auto& entry : frame.Object)
			{
				ENGINE_TRY(budget.Poll());
				result[entry.first] = nullptr;
			}
			for (auto& [key, value] : frame.Object)
			{
				ENGINE_TRY(budget.Poll());
				result[key] = std::move(value);
			}
			return result;
		}

		static Result<Json> ReadJson(ScriptCall& call, int index, std::vector<const void*>& ancestors, Detail::NativeConversionBudget& budget)
		{
			lua_State* state = call.State;
			index = lua_absindex(state, index);
			struct RestoreTraversal
			{
				lua_State* State = nullptr;
				int Top = 0;
				std::vector<const void*>& Ancestors;
				size_t AncestorCount = 0;
				int Exceptions = std::uncaught_exceptions();
				~RestoreTraversal()
				{
					// Luau reads the error object from the stack after C++ unwinding.
					if (std::uncaught_exceptions() == Exceptions)
						lua_settop(State, Top);
					Ancestors.resize(AncestorCount);
				}
			} restore{ state, lua_gettop(state), ancestors, ancestors.size() };
			// At MaxJsonDepth, native recursion exhausts the default Windows stack in Debug. Suspend iteration on
			// the VM stack and keep native frames on the heap, charged by the same per-value allowance.
			// Stable ownership also prevents vector growth from copying maps that already own completed deep siblings.
			std::vector<Scope<JsonReadFrame>> frames;
			Json value;
			bool completed = false;
			while (true)
			{
				if (!completed)
				{
					ENGINE_TRY(budget.Charge(Detail::NativeConversionBudget::ValueBytes));
					if (!lua_checkstack(state, 3))
						return MakeError(ErrorCode::Script, "JSON conversion exceeds the VM stack limit");
					if (lua_istable(state, index))
					{
						const void* identity = lua_topointer(state, index);
						if (ancestors.size() >= MaxJsonDepth)
							return MakeError(ErrorCode::Script, "JSON value exceeds the nesting limit");
						if (std::ranges::find(ancestors, identity) != ancestors.end())
							return MakeError(ErrorCode::Script, "JSON value contains a cycle");
						ancestors.push_back(identity);
						frames.push_back(CreateScope<JsonReadFrame>());
						frames.back()->TableIndex = index;
						lua_pushnil(state);
					}
					else
					{
						ENGINE_TRY_ASSIGN(value, ReadJsonScalar(call, index, budget));
						completed = true;
					}
				}
				if (completed)
				{
					if (frames.empty())
						return value;
					auto& parent = *frames.back();
					if (parent.ArrayKey != 0)
						parent.Array.emplace(parent.ArrayKey, std::move(value));
					else
						parent.Object.emplace(std::move(parent.Key), std::move(value));
					lua_pop(state, 1);
					completed = false;
				}
				ENGINE_TRY_ASSIGN(const bool next, NextJsonEntry(call, *frames.back(), budget));
				if (next)
					index = lua_gettop(state);
				else
				{
					ENGINE_TRY_ASSIGN(value, FinishJsonFrame(*frames.back(), budget));
					frames.pop_back();
					ancestors.pop_back();
					completed = true;
				}
			}
		}

		static Result<Json> ReadJson(ScriptCall& call, int index, std::vector<const void*>& ancestors)
		{
			Detail::NativeConversionBudget budget(call);
			auto result = ReadJson(call, index, ancestors, budget);
			if (result)
				if (const auto status = budget.Poll(); !status)
					result = std::unexpected(status.error());
			return result;
		}

		static void PushJsonScalar(ScriptCall& call, const Json& value)
		{
			if (value.is_discarded() || value.is_binary())
				Lua::RaiseError(call, "value is not a JSON scalar, object or array");
			const JsonReader reader(value);
			if (reader.IsNull())
				Lua::PushNil(call);
			else if (value.is_boolean())
			{
				auto result = reader.ReadBool();
				if (!result)
					Lua::RaiseError(call, result.error());
				Lua::Push(call, *result);
			}
			else if (value.is_number())
			{
				auto result = reader.ReadDouble();
				if (!result)
					Lua::RaiseError(call, result.error());
				Lua::Push(call, *result);
			}
			else
			{
				auto result = reader.ReadString();
				if (!result)
					Lua::RaiseError(call, result.error());
				if (!IsValidUtf8(*result))
					Lua::RaiseError(call, "JSON string is not valid UTF-8");
				Lua::PushString(call, *result);
			}
		}

		struct JsonPushFrame
		{
			const Json* Source = nullptr; // Borrowed from the immutable input for this call only.
			Json::const_iterator Next{};
			int TableIndex = 0;
			int ArrayIndex = 1;
		};

		static void PushJson(ScriptCall& call, const Json& value)
		{
			std::vector<JsonPushFrame> frames;
			const Json* next = &value;
			bool completed = false;
			Detail::NativeConversionBudget budget(call);
			while (true)
			{
				budget.CheckDeadline();
				if (!completed)
				{
					if (!lua_checkstack(call.State, 3))
						Lua::RaiseError(call, "JSON conversion exceeds the VM stack limit");
					if (frames.size() > MaxJsonDepth)
						Lua::RaiseError(call, "value exceeds maximum nesting");
					if (next->is_array() || next->is_object())
					{
						lua_newtable(call.State);
						frames.push_back({ .Source = next, .Next = next->begin(), .TableIndex = lua_gettop(call.State) });
					}
					else
					{
						PushJsonScalar(call, *next);
						completed = true;
					}
				}
				if (completed)
				{
					if (frames.empty())
						return;
					auto& parent = frames.back();
					if (parent.Source->is_array())
						lua_rawseti(call.State, parent.TableIndex, parent.ArrayIndex++);
					else
						lua_rawset(call.State, parent.TableIndex);
					completed = false;
				}
				auto& frame = frames.back();
				if (frame.Next == frame.Source->end())
				{
					frames.pop_back();
					completed = true;
				}
				else
				{
					if (frame.Source->is_object())
					{
						if (!IsValidUtf8(frame.Next.key()))
							Lua::RaiseError(call, "JSON object key is not valid UTF-8");
						Lua::PushString(call, frame.Next.key());
					}
					next = &frame.Next.value();
					++frame.Next;
				}
			}
		}

		static std::optional<Value> ReadValueScalar(ScriptCall& call, int index, const TypeInfo& type,
			std::vector<const void*>& ancestors, Detail::NativeConversionBudget& budget)
		{
			switch (type.GetKind())
			{
				case FieldType::Bool:   return Value::FromBool(Lua::Check<bool>(call, index));
				case FieldType::Int32:  return Value::FromInt32(Lua::Check<int32_t>(call, index));
				case FieldType::UInt32: return Value::FromUInt32(Lua::Check<uint32_t>(call, index));
				case FieldType::Float:  return Value::FromFloat(Lua::Check<float>(call, index));
				case FieldType::Vec2:   return Value::FromVec2(Lua::Check<glm::vec2>(call, index));
				case FieldType::Vec3:   return Value::FromVec3(Lua::Check<glm::vec3>(call, index));
				case FieldType::Vec4:   return Value::FromVec4(Lua::Check<glm::vec4>(call, index));
				case FieldType::Quat:   return Value::FromQuat(Lua::Check<glm::quat>(call, index));
				case FieldType::Color3: return Value::FromColor3(glm::vec3(Lua::Check<glm::vec4>(call, index)));
				case FieldType::Color4: return Value::FromColor4(Lua::Check<glm::vec4>(call, index));
				case FieldType::String: return Value::FromString(budget.ReadString(index));
				case FieldType::EntityRef:
				{
					if (Lua::IsNoneOrNil(call, index))
						return Value::FromEntityRef({});
					const auto identity = Lua::Check<ScriptEntityIdentity>(call, index);
					if (!call.Engine)
						Lua::RaiseError(call, "entity references require an active script engine");
					auto valid = ScriptProxy::ValidateEntity(call.Engine->GetHost(), identity);
					if (!valid)
						Lua::RaiseError(call, valid.error());
					return Value::FromEntityRef(identity.ID);
				}
				case FieldType::AssetRef:
					return Value::FromAssetRef(Lua::IsNoneOrNil(call, index) ? UUID{} : Lua::Check<AssetHandle>(call, index));
				case FieldType::Enum:
				{
					const std::string name = budget.ReadString(index);
					const EnumEntry* entry = type.GetEnum()->FindByNameIgnoreCase(name);
					if (!entry)
						Lua::RaiseError(call, std::format("unknown {} value '{}'", type.GetName(), name));
					return Value::FromEnum(entry->Value);
				}
				case FieldType::Variant:
				{
					auto json = ReadJson(call, index, ancestors, budget);
					if (!json)
						Lua::RaiseError(call, json.error());
					return Value::FromVariant(VariantValue(std::move(*json)));
				}
				default: return std::nullopt;
			}
		}

		struct ValueReadFrame
		{
			const TypeInfo* Type = nullptr;
			const FieldInfo* Field = nullptr;
			ResolveContext Resolve{};
			int TableIndex = 0;
			size_t Count = 0;
			size_t Next = 0;
			bool Variant = false;
			std::vector<Value> Values{};
			std::vector<std::string> Keys{};
		};

		static ValueReadFrame BeginValueTable(ScriptCall& call, int index, const TypeInfo& type, const FieldInfo* field,
			const ResolveContext& resolve, Detail::NativeConversionBudget& budget)
		{
			lua_State* state = call.State;
			ValueReadFrame frame{ .Type = &type, .Field = field, .Resolve = resolve, .TableIndex = index };
			if (type.GetKind() == FieldType::Array || type.GetKind() == FieldType::Bool3)
			{
				const int count = lua_objlen(state, index);
				int actual = 0;
				lua_pushnil(state);
				while (lua_next(state, index))
				{
					budget.Check(Detail::NativeConversionBudget::EntryBytes);
					const uint32_t key = Lua::Check<uint32_t>(call, -2);
					if (key == 0 || key > static_cast<uint32_t>(count))
						Lua::RaiseError(call, "array must have consecutive integer keys");
					++actual;
					lua_pop(state, 1);
				}
				if (actual != count)
					Lua::RaiseError(call, "array contains holes");
				if (type.GetKind() == FieldType::Bool3 && count != 3)
					Lua::RaiseError(call, "expected three booleans");
				frame.Count = static_cast<size_t>(count);
				return frame;
			}
			lua_pushnil(state);
			while (lua_next(state, index))
			{
				budget.Check(Detail::NativeConversionBudget::EntryBytes);
				frame.Keys.push_back(budget.ReadString(-2));
				lua_pop(state, 1);
			}
			std::ranges::sort(frame.Keys);
			budget.CheckDeadline();
			if (type.GetKind() == FieldType::Struct)
			{
				for (const auto& key : frame.Keys)
				{
					budget.CheckDeadline();
					if (!type.GetStruct()->FindField(key))
						Lua::RaiseError(call, std::format("unknown {} field '{}'", type.GetName(), key));
				}
				frame.Keys.clear();
				for (const auto& member : type.GetStruct()->GetFields())
				{
					budget.Check(Detail::NativeConversionBudget::EntryBytes);
					budget.Check(member->GetName().size(), Detail::NativeConversionBudget::StringByteCopies);
					frame.Keys.push_back(member->GetName());
				}
			}
			frame.Count = frame.Keys.size();
			return frame;
		}

		static Value FinishValueTable(ValueReadFrame& frame)
		{
			switch (frame.Type->GetKind())
			{
				case FieldType::Array:  return Value::FromArray(std::move(frame.Values));
				case FieldType::Struct: return Value::FromStruct(std::move(frame.Keys), std::move(frame.Values));
				case FieldType::Bool3:
					return Value::FromBool3(glm::bvec3(frame.Values[0].AsBool(), frame.Values[1].AsBool(), frame.Values[2].AsBool()));
				default: return Value::FromMap(std::move(frame.Keys), std::move(frame.Values));
			}
		}

		static Value ReadValue(ScriptCall& call, int index, const TypeInfo& type, const FieldInfo* field,
			ResolveContext resolve, std::vector<const void*>& ancestors, Detail::NativeConversionBudget& budget)
		{
			lua_State* state = call.State;
			index = lua_absindex(state, index);
			struct RestoreTraversal
			{
				lua_State* State = nullptr;
				int Top = 0;
				std::vector<const void*>& Ancestors;
				size_t AncestorCount = 0;
				int Exceptions = std::uncaught_exceptions();
				~RestoreTraversal()
				{
					// Luau reads the error object from the stack after C++ unwinding.
					if (std::uncaught_exceptions() == Exceptions)
						lua_settop(State, Top);
					Ancestors.resize(AncestorCount);
				}
			} restore{ state, lua_gettop(state), ancestors, ancestors.size() };
			std::vector<ValueReadFrame> frames;
			const TypeInfo* nextType = &type;
			Value value;
			bool completed = false;
			while (true)
			{
				if (!completed)
				{
					budget.Check(Detail::NativeConversionBudget::ValueBytes);
					if (!lua_checkstack(state, 3))
						Lua::RaiseError(call, "field conversion exceeds the VM stack limit");
					if (nextType == nullptr)
					{
						// Bool3 elements have no separate TypeInfo.
						value = Value::FromBool(Lua::Check<bool>(call, index));
						completed = true;
					}
					else if (nextType->GetKind() == FieldType::Variant && field && field->GetResolver())
					{
						auto schema = field->GetResolver()(resolve);
						if (!schema)
							Lua::RaiseError(call, schema.error());
						nextType = &(*schema)->GetType();
						field = *schema;
						frames.push_back({ .Type = nextType, .Variant = true });
						continue;
					}
					else if (auto scalar = ReadValueScalar(call, index, *nextType, ancestors, budget))
					{
						value = std::move(*scalar);
						completed = true;
					}
					else
					{
						RequireType(call, index, LUA_TTABLE);
						const void* identity = lua_topointer(state, index);
						if (ancestors.size() >= MaxJsonDepth || std::ranges::find(ancestors, identity) != ancestors.end())
							Lua::RaiseError(call, "cyclic or excessively nested reflected value");
						ancestors.push_back(identity);
						frames.push_back(BeginValueTable(call, index, *nextType, field, resolve, budget));
					}
				}
				if (completed)
				{
					if (frames.empty())
						return value;
					auto& parent = frames.back();
					if (parent.Variant)
					{
						budget.CheckDeadline();
						auto json = ValueToJson(value, *parent.Type);
						budget.CheckDeadline();
						if (!json)
							Lua::RaiseError(call, json.error());
						value = Value::FromVariant(VariantValue(std::move(*json)));
						frames.pop_back();
						continue;
					}
					parent.Values.push_back(std::move(value));
					lua_pop(state, 1);
					completed = false;
				}
				auto& frame = frames.back();
				if (frame.Next == frame.Count)
				{
					value = FinishValueTable(frame);
					frames.pop_back();
					ancestors.pop_back();
					completed = true;
					continue;
				}
				const size_t element = frame.Next++;
				resolve = frame.Resolve;
				const auto kind = frame.Type->GetKind();
				if (kind == FieldType::Array || kind == FieldType::Bool3)
				{
					lua_rawgeti(state, frame.TableIndex, static_cast<int>(element + 1));
					nextType = frame.Type->GetElement();
					field = frame.Type->GetElementSchema();
				}
				else
				{
					Lua::PushString(call, frame.Keys[element]);
					lua_rawget(state, frame.TableIndex);
					field = kind == FieldType::Struct ? frame.Type->GetStruct()->GetFields()[element].get() : frame.Field;
					nextType = kind == FieldType::Struct ? &field->GetType() : frame.Type->GetElement();
					if (kind == FieldType::Map)
						resolve.Key = frame.Keys[element];
				}
				index = lua_gettop(state);
			}
		}

		static bool PushValueScalar(ScriptCall& call, const Value& value, const TypeInfo& type)
		{
			if (value.IsNull())
			{
				Lua::PushNil(call);
				return true;
			}
			switch (value.GetKind())
			{
				case FieldType::Bool:   Lua::Push(call, value.AsBool()); return true;
				case FieldType::Int32:  Lua::Push(call, value.AsInt32()); return true;
				case FieldType::UInt32: Lua::Push(call, value.AsUInt32()); return true;
				case FieldType::Float:  Lua::Push(call, value.AsFloat()); return true;
				case FieldType::Vec2:   Lua::Push(call, value.AsVec2()); return true;
				case FieldType::Vec3:   Lua::Push(call, value.AsVec3()); return true;
				case FieldType::Vec4:
				case FieldType::Color4: Lua::Push(call, value.AsVec4()); return true;
				case FieldType::Color3: Lua::Push(call, glm::vec4(value.AsVec3(), 1.0f)); return true;
				case FieldType::Quat:   Lua::Push(call, value.AsQuat()); return true;
				case FieldType::String: Lua::PushString(call, value.AsString()); return true;
				case FieldType::EntityRef:
					Lua::Push(call, ScriptEntityIdentity{ value.AsUUID(), call.Engine ? call.Engine->GetHost().GetSceneGeneration() : 0 });
					return true;
				case FieldType::AssetRef: Lua::Push(call, value.AsUUID()); return true;
				case FieldType::Enum:
				{
					const EnumEntry* entry = type.GetEnum()->FindByValue(value.AsEnum());
					if (!entry)
						Lua::RaiseError(call, "invalid reflected enum value");
					Lua::PushString(call, entry->Name);
					return true;
				}
				case FieldType::Bool3:
					lua_newtable(call.State);
					for (int i = 0; i < 3; ++i)
					{
						Lua::Push(call, value.AsBool3()[i]);
						lua_rawseti(call.State, -2, i + 1);
					}
					return true;
				default: return false;
			}
		}

		struct ValuePushFrame
		{
			const Value* Source = nullptr; // Borrowed from the input or Owned for this traversal only.
			const TypeInfo* Type = nullptr;
			const FieldInfo* Field = nullptr;
			ResolveContext Resolve{};
			Scope<Value> Owned{};
			int TableIndex = 0;
			size_t Next = 0;
			size_t Depth = 0;
			bool Variant = false;
		};

		static void PushValue(ScriptCall& call, const Value& value, const TypeInfo& type, size_t depth = 0, const FieldInfo* field = nullptr, ResolveContext resolve = {})
		{
			std::vector<ValuePushFrame> frames;
			const Value* next = &value;
			const TypeInfo* nextType = &type;
			bool completed = false;
			Detail::NativeConversionBudget budget(call);
			while (true)
			{
				budget.CheckDeadline();
				if (!completed)
				{
					if (!lua_checkstack(call.State, 3))
						Lua::RaiseError(call, "field conversion exceeds the VM stack limit");
					if (depth > MaxJsonDepth)
						Lua::RaiseError(call, "value exceeds maximum nesting");
					if (!next->IsNull() && next->GetKind() == FieldType::Variant)
					{
						if (field && field->GetResolver())
						{
							auto schema = field->GetResolver()(resolve);
							if (!schema)
								Lua::RaiseError(call, schema.error());
							auto typed = ValueFromJson(JsonReader(next->AsVariant().Get()), (*schema)->GetType());
							budget.CheckDeadline();
							if (!typed)
								Lua::RaiseError(call, typed.error());
							ValuePushFrame frame{ .Owned = CreateScope<Value>(std::move(*typed)), .Variant = true };
							next = frame.Owned.get();
							nextType = &(*schema)->GetType();
							field = *schema;
							frames.push_back(std::move(frame));
							++depth;
							continue;
						}
						PushJson(call, next->AsVariant().Get());
						completed = true;
					}
					else if (PushValueScalar(call, *next, *nextType))
						completed = true;
					else
					{
						lua_newtable(call.State);
						frames.push_back({ .Source = next, .Type = nextType, .Field = field, .Resolve = resolve, .TableIndex = lua_gettop(call.State), .Depth = depth });
					}
				}
				if (completed)
				{
					if (frames.empty())
						return;
					auto& parent = frames.back();
					if (parent.Variant)
					{
						frames.pop_back();
						continue;
					}
					if (parent.Source->GetKind() == FieldType::Array)
						lua_rawseti(call.State, parent.TableIndex, static_cast<int>(parent.Next));
					else
						lua_rawset(call.State, parent.TableIndex);
					completed = false;
				}
				auto& frame = frames.back();
				const auto elements = frame.Source->GetElements();
				if (frame.Next == elements.size())
				{
					frames.pop_back();
					completed = true;
					continue;
				}
				const size_t element = frame.Next++;
				resolve = frame.Resolve;
				depth = frame.Depth + 1;
				next = &elements[element];
				if (frame.Source->GetKind() == FieldType::Array)
				{
					nextType = frame.Type->GetElement();
					field = frame.Type->GetElementSchema();
				}
				else
				{
					const auto& key = frame.Source->GetKeys()[element];
					Lua::PushString(call, key);
					field = frame.Source->GetKind() == FieldType::Struct ? frame.Type->GetStruct()->FindField(key) : frame.Field;
					nextType = frame.Source->GetKind() == FieldType::Struct ? &field->GetType() : frame.Type->GetElement();
					resolve.Key = key;
				}
			}
		}

		static void WriteErrorText(BinaryWriter& writer, std::string_view text)
		{
			// Script errors may contain arbitrary Lua byte strings, unlike persisted UTF-8 document strings.
			writer.WriteU64(text.size());
			writer.WriteBytes(AsBytes(text));
		}
		static Result<std::string> ReadErrorText(BinaryReader& reader)
		{
			ENGINE_TRY_ASSIGN(const auto size, reader.ReadU64());
			if (size > reader.GetRemaining())
				return MakeError(ErrorCode::Parse, "truncated forwarded script error");
			ENGINE_TRY_ASSIGN(const auto bytes, reader.ReadBytes(static_cast<size_t>(size)));
			return std::string(AsStringView(bytes));
		}
		static Result<ScriptError> ReadForwardedError(lua_State* state, int index)
		{
			const void* bytes = lua_touserdatatagged(state, index, std::to_underlying(Detail::ScriptValueTag::ForwardedError));
			if (!bytes)
				return MakeError(ErrorCode::NotFound, "not a forwarded script error");
			BinaryReader reader({ static_cast<const std::byte*>(bytes), static_cast<size_t>(lua_objlen(state, index)) });
			ScriptError error;
			ENGINE_TRY_ASSIGN(error.ID, reader.ReadU64());
			ENGINE_TRY_ASSIGN(const auto kind, reader.ReadU8());
			if (kind > std::to_underlying(ScriptErrorKind::Memory))
				return MakeError(ErrorCode::Parse, "invalid forwarded script error kind");
			error.Kind = static_cast<ScriptErrorKind>(kind);
			ENGINE_TRY_ASSIGN(error.Script, ReadErrorText(reader));
			ENGINE_TRY_ASSIGN(error.Line, reader.ReadU32());
			ENGINE_TRY_ASSIGN(error.Column, reader.ReadU32());
			ENGINE_TRY_ASSIGN(error.Message, ReadErrorText(reader));
			ENGINE_TRY_ASSIGN(error.Callback, ReadErrorText(reader));
			ENGINE_TRY_ASSIGN(const auto entity, reader.ReadU64());
			error.Entity = UUID(entity);
			ENGINE_TRY_ASSIGN(error.EntityName, ReadErrorText(reader));
			ENGINE_TRY_ASSIGN(error.Tick, reader.ReadU64());
			ENGINE_TRY_ASSIGN(error.Count, reader.ReadU64());
			ENGINE_TRY_ASSIGN(error.JsonPointer, ReadErrorText(reader));
			ENGINE_TRY_ASSIGN(const auto frames, reader.ReadU64());
			if (frames > reader.GetRemaining() / 20)
				return MakeError(ErrorCode::Parse, "truncated forwarded script traceback");
			for (uint64_t i = 0; i < frames; ++i)
			{
				ScriptTraceFrame frame;
				ENGINE_TRY_ASSIGN(frame.Script, ReadErrorText(reader));
				ENGINE_TRY_ASSIGN(frame.Line, reader.ReadU32());
				ENGINE_TRY_ASSIGN(frame.Function, ReadErrorText(reader));
				error.Traceback.push_back(std::move(frame));
			}
			if (!reader.IsAtEnd())
				return MakeError(ErrorCode::Parse, "trailing forwarded script error bytes");
			return error;
		}
		static int ForwardedErrorText(lua_State* state)
		{
			const auto error = ReadForwardedError(state, 1);
			if (error)
				lua_pushlstring(state, error->Message.data(), error->Message.size());
			else
				lua_pushliteral(state, "invalid forwarded script error");
			return 1;
		}
		static void PushForwardedError(ScriptCall& call, const ScriptError& error)
		{
			BinaryWriter writer;
			writer.WriteU64(error.ID);
			writer.WriteU8(std::to_underlying(error.Kind));
			WriteErrorText(writer, error.Script);
			writer.WriteU32(error.Line);
			writer.WriteU32(error.Column);
			WriteErrorText(writer, error.Message);
			WriteErrorText(writer, error.Callback);
			writer.WriteU64(error.Entity.GetValue());
			WriteErrorText(writer, error.EntityName);
			writer.WriteU64(error.Tick);
			writer.WriteU64(error.Count);
			WriteErrorText(writer, error.JsonPointer);
			writer.WriteU64(error.Traceback.size());
			for (const auto& frame : error.Traceback)
			{
				WriteErrorText(writer, frame.Script);
				writer.WriteU32(frame.Line);
				WriteErrorText(writer, frame.Function);
			}
			constexpr int Tag = std::to_underlying(Detail::ScriptValueTag::ForwardedError);
			lua_getuserdatametatable(call.State, Tag);
			const bool initialized = lua_istable(call.State, -1);
			lua_pop(call.State, 1);
			if (!initialized)
			{
				lua_newtable(call.State);
				lua_pushliteral(call.State, "ScriptError");
				lua_rawsetfield(call.State, -2, "__type");
				lua_pushliteral(call.State, "locked");
				lua_rawsetfield(call.State, -2, "__metatable");
				lua_pushcfunction(call.State, ForwardedErrorText, "ScriptError.__tostring");
				lua_rawsetfield(call.State, -2, "__tostring");
				lua_setreadonly(call.State, -1, 1);
				lua_setuserdatametatable(call.State, Tag);
			}
			void* payload = lua_newuserdatataggedwithmetatable(call.State, writer.GetSize(), Tag);
			std::memcpy(payload, writer.GetData().data(), writer.GetSize());
		}
		static ScriptError CaptureFailure(ScriptCall& call, ScriptErrorKind kind)
		{
			if (lua_userdatatag(call.State, -1) == std::to_underlying(Detail::ScriptValueTag::ForwardedError))
			{
				auto forwarded = ReadForwardedError(call.State, -1);
				if (forwarded && (kind == ScriptErrorKind::Runtime || kind == forwarded->Kind))
					return std::move(*forwarded);
			}
			const auto context = Detail::SandboxAccess::GetContext(*call.Sandbox);
			ScriptError error;
			error.Kind = kind;
			error.Script = context.Script;
			error.Callback = context.Callback;
			error.Entity = context.Entity;
			error.EntityName = context.EntityName;
			error.Tick = context.Tick;
			if (lua_type(call.State, -1) == LUA_TSTRING)
			{
				size_t length = 0;
				const char* message = lua_tolstring(call.State, -1, &length);
				error.Message.assign(message, length);
			}
			else
				error.Message = "script raised a non-string error";
			for (int level = 0; level < lua_stackdepth(call.State); ++level)
			{
				lua_Debug frame{};
				if (!lua_getinfo(call.State, level, "sln", &frame) || !frame.source)
					continue;
				const auto location = Detail::SandboxAccess::Locate(*call.Sandbox, frame.source, frame.currentline > 0 ? static_cast<uint32_t>(frame.currentline) : 0);
				if (location.File.empty())
					continue;
				if (error.Traceback.empty())
				{
					error.Script = location.File;
					error.Line = location.Line;
					error.Column = location.Column;
					error.JsonPointer = location.JsonPointer.value_or("");
					const std::string prefix = std::format("{}:{}: ", frame.source[0] == '@' || frame.source[0] == '=' ? frame.source + 1 : frame.source, frame.currentline);
					if (error.Message.starts_with(prefix))
						error.Message.erase(0, prefix.size());
				}
				error.Traceback.push_back({ location.File, location.Line, frame.name ? frame.name : "" });
			}
			if (kind == ScriptErrorKind::Timeout)
				error.Message = std::format("script exceeded {} ms in {} (possible infinite loop)", Detail::SandboxAccess::GetBudgetMs(*call.Sandbox), context.Callback);
			if (kind == ScriptErrorKind::Memory)
				error.Message = std::format("script exceeded memory limit {} MB in {}", call.Sandbox->GetMemoryState().SoftLimitBytes / (1024 * 1024), context.Callback);
			return error;
		}

		static int ErrorHandler(lua_State* state)
		{
			Sandbox* owner = Detail::SandboxAccess::FromState(state);
			auto result = owner ? Detail::SandboxAccess::ThreadCall(*owner, state) : Result<ScriptCall>(MakeError(ErrorCode::InvalidState, "unowned script state"));
			if (!result)
			{
				lua_pushliteral(state, "invalid script error context");
				return 1;
			}
			auto& call = *result;
			const auto kind = Detail::SandboxAccess::ClassifyFailure(*call.Sandbox, LUA_ERRRUN).value_or(ScriptErrorKind::Runtime);
			auto error = CaptureFailure(call, kind);
			if (call.Execution && (kind == ScriptErrorKind::Memory || kind == ScriptErrorKind::Timeout))
			{
				if (call.Execution->SafetyFailure && call.Execution->SafetyFailure->Kind == kind)
					error = *call.Execution->SafetyFailure;
				else
					call.Execution->SafetyFailure = error;
			}
			Detail::SandboxAccess::RecordFailure(*call.Sandbox, error);
			lua_pushvalue(state, 1);
			return 1;
		}

		static int NativeEntry(lua_State* state)
		{
			Sandbox* owner = Detail::SandboxAccess::FromState(state);
			auto result = owner ? Detail::SandboxAccess::ThreadCall(*owner, state) : Result<ScriptCall>(MakeError(ErrorCode::InvalidState, "unowned script state"));
			if (!result)
				luaL_error(state, "%s", result.error().GetMessageText().c_str());
			ScriptNativeFunction function = result->Execution ? result->Execution->NativeEntry : nullptr;
			if (!function)
				return Lua::RaiseError(*result, "missing protected native entry");
			return function(*result);
		}

		static int Bootstrap(lua_State* state)
		{
			// cpcall's null lightuserdata argument is discarded before any native/user code runs. No C++ address enters Lua.
			lua_settop(state, 0);
			if (!lua_checkstack(state, 8))
				luaL_error(state, "stack limit");
			lua_pushcfunction(state, ErrorHandler, "Engine.ErrorHandler");
			lua_rawsetfield(state, LUA_REGISTRYINDEX, "Engine.ErrorHandler");
			lua_pushcfunction(state, NativeEntry, "Engine.NativeEntry");
			lua_rawsetfield(state, LUA_REGISTRYINDEX, "Engine.NativeEntry");
			return 0;
		}

		struct ProtectedScope
		{
			ScriptCall& Call;
			ScriptExecutionContext* Previous = nullptr;
			lua_State* PreviousThread = nullptr;
			~ProtectedScope()
			{
				if (Call.Engine)
					static_cast<void>(Detail::ScriptEngineAccess::ExchangeActiveThread(*Call.Engine, PreviousThread));
				Detail::SandboxAccess::LeaveProtected(*Call.Sandbox);
				Call.Execution = Previous;
			}
		};

		static ScriptCallResult Outcome(ScriptCall& call, int status, int count, bool captured)
		{
			static_cast<void>(Detail::SandboxAccess::CheckInterrupt(*call.Sandbox, -1));
			const auto kind = Detail::SandboxAccess::ClassifyFailure(*call.Sandbox, status);
			if (kind)
			{
				const auto previous = captured && status != LUA_ERRMEM && status != LUA_ERRERR ? call.Sandbox->GetLastError() : std::optional<ScriptError>{};
				ScriptError error = previous ? *previous : CaptureFailure(call, *kind);
				// A child compile/type failure crosses lua_error as ERRRUN. Preserve its typed diagnostic; safety wins.
				if (*kind == ScriptErrorKind::Memory || *kind == ScriptErrorKind::Timeout)
				{
					error.Kind = *kind;
					if (call.Execution)
					{
						if (call.Execution->SafetyFailure && call.Execution->SafetyFailure->Kind == *kind)
							error = *call.Execution->SafetyFailure;
						else
							call.Execution->SafetyFailure = error;
					}
				}
				Detail::SandboxAccess::RecordFailure(*call.Sandbox, error);
				return { .Failure = std::move(error) };
			}
			return { .ResultCount = count, .Yielded = status == LUA_YIELD };
		}

	}

	namespace Detail {

		NativeConversionBudget::NativeConversionBudget(ScriptCall& call)
			: m_Call(call)
		{
		}

		Status NativeConversionBudget::Poll() const
		{
			if (const auto fault = SandboxAccess::CheckInterrupt(*m_Call.Sandbox, -1))
			{
				// Preserve the caller before a Lua pcall can unwind it; the outer boundary still enforces the latch.
				auto error = Utils::CaptureFailure(m_Call, *fault);
				if (m_Call.Execution)
				{
					if (m_Call.Execution->SafetyFailure && m_Call.Execution->SafetyFailure->Kind == *fault)
						error = *m_Call.Execution->SafetyFailure;
					else
						m_Call.Execution->SafetyFailure = error;
				}
				SandboxAccess::RecordFailure(*m_Call.Sandbox, error);
				return MakeError(*fault == ScriptErrorKind::Timeout ? ErrorCode::Timeout : ErrorCode::Script,
					"{}", *fault == ScriptErrorKind::Timeout ? "native conversion exceeded the script deadline" : "script memory limit exceeded");
			}
			return {};
		}

		Status NativeConversionBudget::Charge(uint64_t count, uint64_t bytesPerItem)
		{
			ENGINE_TRY(Poll());
			const uint64_t limit = std::min(MaximumBytes, m_Call.Sandbox->GetMemoryState().SoftLimitBytes);
			if (m_UsedBytes > limit || count > (limit - m_UsedBytes) / bytesPerItem)
				return MakeError(ErrorCode::Script, "conversion exceeds its native budget of {} bytes", limit);
			m_UsedBytes += count * bytesPerItem;
			return {};
		}

		void NativeConversionBudget::Check(uint64_t count, uint64_t bytesPerItem)
		{
			if (const auto status = Charge(count, bytesPerItem); !status)
				Lua::RaiseError(m_Call, status.error());
		}

		void NativeConversionBudget::CheckDeadline() const
		{
			if (const auto status = Poll(); !status)
				Lua::RaiseError(m_Call, status.error());
		}

		std::string NativeConversionBudget::ReadString(int index)
		{
			Utils::RequireType(m_Call, index, LUA_TSTRING);
			size_t length = 0;
			const char* text = lua_tolstring(m_Call.State, index, &length);
			Check(length, StringByteCopies);
			return std::string(text, length);
		}

		Result<Json> SandboxAccess::ReadJson(ScriptCall& call, int index)
		{
			std::vector<const void*> ancestors;
			return Utils::ReadJson(call, index, ancestors);
		}

		ErrorLocation ScriptCallerLocation(ScriptCall& call, int level)
		{
			for (; level < lua_stackdepth(call.State); ++level)
			{
				lua_Debug frame{};
				if (lua_getinfo(call.State, level, "sl", &frame) && frame.source)
				{
					auto result = SandboxAccess::Locate(*call.Sandbox, frame.source, frame.currentline > 0 ? static_cast<uint32_t>(frame.currentline) : 0);
					if (!result.File.empty())
						return result;
				}
			}
			return {};
		}

	}

	namespace Lua {

		ScriptEngine* GetEngine(ScriptCall& call)
		{
			return call.Engine;
		}

		template<>
		bool Check<bool>(ScriptCall& call, int index)
		{
			Utils::RequireType(call, index, LUA_TBOOLEAN);
			return lua_toboolean(call.State, index) != 0;
		}
		template<>
		double Check<double>(ScriptCall& call, int index)
		{
			Utils::RequireType(call, index, LUA_TNUMBER);
			const double value = lua_tonumber(call.State, index);
			Utils::Finite(call, value, index);
			return value;
		}
		template<>
		float Check<float>(ScriptCall& call, int index)
		{
			const double value = Check<double>(call, index);
			if (value < -std::numeric_limits<float>::max() || value > std::numeric_limits<float>::max())
				RaiseError(call, "number exceeds finite float range");
			return static_cast<float>(value);
		}
		template<>
		int32_t Check<int32_t>(ScriptCall& call, int index)
		{
			const double value = Check<double>(call, index);
			if (std::trunc(value) != value || value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max())
				RaiseError(call, "expected an exact signed 32-bit integer");
			return static_cast<int32_t>(value);
		}
		template<>
		uint32_t Check<uint32_t>(ScriptCall& call, int index)
		{
			const double value = Check<double>(call, index);
			if (std::trunc(value) != value || value < 0 || value > std::numeric_limits<uint32_t>::max())
				RaiseError(call, "expected an exact unsigned 32-bit integer");
			return static_cast<uint32_t>(value);
		}
		template<>
		std::string Check<std::string>(ScriptCall& call, int index)
		{
			Utils::RequireType(call, index, LUA_TSTRING);
			size_t length = 0;
			const char* value = lua_tolstring(call.State, index, &length);
			return std::string(value, length);
		}
		template<>
		glm::vec3 Check<glm::vec3>(ScriptCall& call, int index)
		{
			Utils::RequireType(call, index, LUA_TVECTOR);
			const float* v = lua_tovector(call.State, index);
			for (int i = 0; i < 3; ++i)
				Utils::Finite(call, v[i], index);
			return { v[0], v[1], v[2] };
		}
		template<>
		glm::vec2 Check<glm::vec2>(ScriptCall& call, int index)
		{
			const auto v = Check<glm::vec3>(call, index);
			if (v.z != 0)
				RaiseError(call, "vector2 requires z == 0");
			return glm::vec2(v);
		}
		glm::vec4& CheckColorStorage(ScriptCall& call, int index)
		{
			return Utils::Payload<glm::vec4>(call, index, std::to_underlying(Detail::ScriptValueTag::Color));
		}
		glm::quat& CheckQuatStorage(ScriptCall& call, int index)
		{
			return Utils::Payload<glm::quat>(call, index, std::to_underlying(Detail::ScriptValueTag::Quat));
		}
		template<>
		glm::vec4 Check<glm::vec4>(ScriptCall& call, int index)
		{
			const auto value = CheckColorStorage(call, index);
			for (int i = 0; i < 4; ++i)
				Utils::Finite(call, value[i], index);
			return value;
		}
		template<>
		glm::quat Check<glm::quat>(ScriptCall& call, int index)
		{
			const auto value = CheckQuatStorage(call, index);
			bool nonzero = false;
			for (int i = 0; i < 4; ++i)
			{
				Utils::Finite(call, value[i], index);
				nonzero |= value[i] != 0;
			}
			if (!nonzero)
				RaiseError(call, "quaternion must be nonzero");
			return value;
		}
		template<>
		AssetHandle Check<AssetHandle>(ScriptCall& call, int index)
		{
			return Utils::Payload<AssetHandle>(call, index, std::to_underlying(Detail::ScriptValueTag::AssetRef));
		}
		template<>
		ScriptEntityIdentity Check<ScriptEntityIdentity>(ScriptCall& call, int index)
		{
			return Utils::Payload<ScriptEntityIdentity>(call, index, std::to_underlying(Detail::ScriptValueTag::Entity));
		}
		template<>
		ScriptProxyIdentity Check<ScriptProxyIdentity>(ScriptCall& call, int index)
		{
			const int tag = lua_userdatatag(call.State, index);
			if (tag < std::to_underlying(Detail::ScriptValueTag::ComponentBegin))
				RaiseError(call, "expected a component proxy");
			const auto result = Utils::Payload<ScriptProxyIdentity>(call, index, tag);
			if (!call.Engine || result.ComponentTypeIndex >= call.Engine->GetHost().GetTypes().GetComponents().size())
				RaiseError(call, "invalid component proxy type");
			const auto* type = call.Engine->GetHost().GetTypes().GetComponents()[result.ComponentTypeIndex];
			if (tag != Detail::GetScriptValueTag(Utils::Api(call), type->GetName()))
				RaiseError(call, "component proxy tag mismatch");
			return result;
		}
		template<>
		void Push<bool>(ScriptCall& call, const bool& value)
		{
			lua_pushboolean(call.State, value);
		}
		template<>
		void Push<int32_t>(ScriptCall& call, const int32_t& value)
		{
			lua_pushnumber(call.State, value);
		}
		template<>
		void Push<uint32_t>(ScriptCall& call, const uint32_t& value)
		{
			lua_pushnumber(call.State, value);
		}
		template<>
		void Push<double>(ScriptCall& call, const double& value)
		{
			Utils::Finite(call, value, 0);
			lua_pushnumber(call.State, value);
		}
		template<>
		void Push<float>(ScriptCall& call, const float& value)
		{
			Push(call, static_cast<double>(value));
		}
		template<>
		void Push<std::string>(ScriptCall& call, const std::string& value)
		{
			PushString(call, value);
		}
		template<>
		void Push<glm::vec3>(ScriptCall& call, const glm::vec3& value)
		{
			for (int i = 0; i < 3; ++i)
				Utils::Finite(call, value[i], 0);
			lua_pushvector(call.State, value.x, value.y, value.z);
		}
		template<>
		void Push<glm::vec2>(ScriptCall& call, const glm::vec2& value)
		{
			Push(call, glm::vec3(value, 0.0f));
		}
		template<>
		void Push<glm::vec4>(ScriptCall& call, const glm::vec4& value)
		{
			for (int i = 0; i < 4; ++i)
				Utils::Finite(call, value[i], 0);
			Utils::PushPayload(call, value, std::to_underlying(Detail::ScriptValueTag::Color));
		}
		template<>
		void Push<glm::quat>(ScriptCall& call, const glm::quat& value)
		{
			bool nonzero = false;
			for (int i = 0; i < 4; ++i)
			{
				Utils::Finite(call, value[i], 0);
				nonzero |= value[i] != 0;
			}
			if (!nonzero)
				RaiseError(call, "quaternion must be nonzero");
			Utils::PushPayload(call, value, std::to_underlying(Detail::ScriptValueTag::Quat));
		}
		template<>
		void Push<AssetHandle>(ScriptCall& call, const AssetHandle& value)
		{
			if (!value.IsValid())
				PushNil(call);
			else
				Utils::PushPayload(call, value, std::to_underlying(Detail::ScriptValueTag::AssetRef));
		}
		template<>
		void Push<ScriptEntityIdentity>(ScriptCall& call, const ScriptEntityIdentity& value)
		{
			if (!value.ID.IsValid() || value.SceneGeneration == 0)
				PushNil(call);
			else
				Utils::PushPayload(call, value, std::to_underlying(Detail::ScriptValueTag::Entity));
		}
		template<>
		void Push<ScriptProxyIdentity>(ScriptCall& call, const ScriptProxyIdentity& value)
		{
			if (!value.Entity.ID.IsValid() || value.Entity.SceneGeneration == 0)
			{
				PushNil(call);
				return;
			}
			if (!call.Engine || value.ComponentTypeIndex >= call.Engine->GetHost().GetTypes().GetComponents().size())
			{
				RaiseError(call, "invalid component type");
				return;
			}
			Utils::PushPayload(call, value, Detail::GetScriptValueTag(Utils::Api(call), call.Engine->GetHost().GetTypes().GetComponents()[value.ComponentTypeIndex]->GetName()));
		}
		Random& CheckRandomStorage(ScriptCall& call, int index)
		{
			return Utils::Payload<Random>(call, index, std::to_underlying(Detail::ScriptValueTag::RandomGenerator));
		}
		void PushRandomGenerator(ScriptCall& call, const Random& random)
		{
			Utils::PushPayload(call, random, std::to_underlying(Detail::ScriptValueTag::RandomGenerator));
		}
		ScriptReference CheckTaskHandle(ScriptCall& call, int index)
		{
			return Utils::Payload<ScriptReference>(call, index, std::to_underlying(Detail::ScriptValueTag::TaskHandle));
		}
		void PushTaskHandle(ScriptCall& call, ScriptReference reference)
		{
			if (reference.IsNull())
				PushNil(call);
			else
				Utils::PushPayload(call, reference, std::to_underlying(Detail::ScriptValueTag::TaskHandle));
		}
		int GetArgumentCount(ScriptCall& call)
		{
			return lua_gettop(call.State);
		}
		bool IsNoneOrNil(ScriptCall& call, int index)
		{
			return lua_isnoneornil(call.State, index);
		}
		void PushNil(ScriptCall& call)
		{
			lua_pushnil(call.State);
		}
		void PushString(ScriptCall& call, std::string_view value)
		{
			lua_pushlstring(call.State, value.data(), value.size());
		}
		int RaiseError(ScriptCall& call, std::string_view message)
		{
			const std::string located = call.MemberName.empty() ? std::string(message) : std::format("{}: {}", call.MemberName, message);
			PushString(call, located);
			lua_error(call.State);
		}
		int RaiseError(ScriptCall& call, const Error& error)
		{
			return RaiseError(call, error.ToString());
		}
		int RaiseError(ScriptCall& call, const ScriptError& error)
		{
			Utils::PushForwardedError(call, error);
			lua_error(call.State);
		}
		int64_t CheckEnum(ScriptCall& call, int index, std::string_view enumName)
		{
			const auto* table = Utils::Api(call).FindEnum(enumName);
			if (!table || !call.Member)
			{
				RaiseError(call, "unregistered enum argument");
				return 0;
			}
			const ScriptEnumParameter* slot = nullptr;
			for (const auto& item : call.Member->Options.EnumParameters)
				if (item.ArgumentIndex == index && item.EnumName == enumName)
					slot = &item;
			if (!slot)
			{
				RaiseError(call, "enum argument does not match registration");
				return 0;
			}
			std::string name = IsNoneOrNil(call, index) && slot->Optional && !slot->DefaultValue.empty() ? slot->DefaultValue : Check<std::string>(call, index);
			for (const auto& entry : table->Values)
				if (entry.Name.size() == name.size() && std::equal(entry.Name.begin(), entry.Name.end(), name.begin(), [](char a, char b)
				{
					return (a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) == (b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b);
				}))
					return entry.Value;
			std::string choices;
			for (const auto& entry : table->Values)
			{
				if (!choices.empty())
					choices += ", ";
				choices += entry.Name;
			}
			RaiseError(call, std::format("unknown {} '{}'; expected one of {}", enumName, name, choices));
			return 0;
		}
		Json CheckJson(ScriptCall& call, int index)
		{
			auto result = Detail::SandboxAccess::ReadJson(call, index);
			if (!result)
				RaiseError(call, result.error());
			return std::move(*result);
		}
		void PushJson(ScriptCall& call, const Json& value)
		{
			Utils::PushJson(call, value);
		}
		Value CheckValue(ScriptCall& call, int index, const FieldInfo& field)
		{
			ResolveContext resolve{};
			if (call.Engine)
			{
				resolve.Registry = &call.Engine->GetHost().GetTypes();
				resolve.Schemas = call.Engine->GetHost().GetFieldSchemas();
				if (lua_userdatatag(call.State, 1) >= std::to_underlying(Detail::ScriptValueTag::ComponentBegin))
				{
					const auto proxy = Check<ScriptProxyIdentity>(call, 1);
					auto valid = ScriptProxy::ValidateComponent(call.Engine->GetHost(), proxy);
					if (!valid)
						RaiseError(call, valid.error());
					const auto* component = resolve.Registry->GetComponents()[proxy.ComponentTypeIndex];
					resolve.OwnerType = component;
					resolve.Owner = component->GetHostOps()->GetConst(call.Engine->GetHost().GetScene().FindEntityByID(proxy.Entity.ID));
				}
			}
			return CheckValueForOwner(call, index, field, resolve);
		}
		Value CheckValueForOwner(ScriptCall& call, int index, const FieldInfo& field, const ResolveContext& resolve)
		{
			std::vector<const void*> ancestors;
			Detail::NativeConversionBudget budget(call);
			auto value = Utils::ReadValue(call, index, field.GetType(), &field, resolve, ancestors, budget);
			budget.CheckDeadline();
			return value;
		}
		void PushValue(ScriptCall& call, const Value& value, const FieldInfo& field)
		{
			ResolveContext resolve{};
			if (call.Engine)
			{
				resolve.Registry = &call.Engine->GetHost().GetTypes();
				resolve.Schemas = call.Engine->GetHost().GetFieldSchemas();
				if (lua_userdatatag(call.State, 1) >= std::to_underlying(Detail::ScriptValueTag::ComponentBegin))
				{
					const auto proxy = Check<ScriptProxyIdentity>(call, 1);
					auto valid = ScriptProxy::ValidateComponent(call.Engine->GetHost(), proxy);
					if (!valid)
						RaiseError(call, valid.error());
					const auto* component = resolve.Registry->GetComponents()[proxy.ComponentTypeIndex];
					resolve.OwnerType = component;
					resolve.Owner = component->GetHostOps()->GetConst(call.Engine->GetHost().GetScene().FindEntityByID(proxy.Entity.ID));
				}
			}
			Utils::PushValue(call, value, field.GetType(), 0, &field, resolve);
		}

		Result<ScriptCallResult> ProtectedCall(ScriptCall& call, int argumentCount, int resultCount)
		{
			if (!call.State || !call.Sandbox || argumentCount < 0 || resultCount < LUA_MULTRET || lua_gettop(call.State) < argumentCount + 1 || !lua_isfunction(call.State, -argumentCount - 1))
				return MakeError(ErrorCode::InvalidArgument, "invalid protected call frame");
			ENGINE_TRY(Detail::SandboxAccess::ThreadCall(*call.Sandbox, call.State));
			ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*call.Sandbox, call.Execution ? call.Execution->Origin : ScriptExecutionOrigin::Pure));
			Utils::ProtectedScope scope{ call, call.Execution, call.Engine ? Detail::ScriptEngineAccess::ExchangeActiveThread(*call.Engine, call.State) : nullptr };
			call.Execution = Detail::SandboxAccess::GetExecution(*call.Sandbox);
			const int base = lua_gettop(call.State) - argumentCount - 1;
			int status = lua_cpcall(call.State, Utils::Bootstrap, nullptr);
			if (status != LUA_OK)
			{
				auto result = Utils::Outcome(call, status, 0, false);
				lua_settop(call.State, base);
				return result;
			}
			lua_rawgetfield(call.State, LUA_REGISTRYINDEX, "Engine.ErrorHandler");
			lua_insert(call.State, base + 1);
			status = lua_pcall(call.State, argumentCount, resultCount, base + 1);
			auto result = Utils::Outcome(call, status, lua_gettop(call.State) - base - 1, status != LUA_OK);
			if (result.Failure)
				lua_settop(call.State, base);
			else
				lua_remove(call.State, base + 1);
			return result;
		}

		Result<ScriptCallResult> ProtectedCall(ScriptCall& call, ScriptNativeFunction function)
		{
			if (!call.State || !call.Sandbox || !function)
				return MakeError(ErrorCode::InvalidArgument, "invalid native protected entry");
			ENGINE_TRY(Detail::SandboxAccess::ThreadCall(*call.Sandbox, call.State));
			ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*call.Sandbox, call.Execution ? call.Execution->Origin : ScriptExecutionOrigin::Pure));
			Utils::ProtectedScope scope{ call, call.Execution, call.Engine ? Detail::ScriptEngineAccess::ExchangeActiveThread(*call.Engine, call.State) : nullptr };
			call.Execution = Detail::SandboxAccess::GetExecution(*call.Sandbox);
			struct NativeScope
			{
				ScriptExecutionContext& Context;
				ScriptNativeFunction Previous;
				~NativeScope() { Context.NativeEntry = Previous; }
			} native{ *call.Execution, call.Execution->NativeEntry };
			call.Execution->NativeEntry = function;
			const int base = lua_gettop(call.State);
			int status = lua_cpcall(call.State, Utils::Bootstrap, nullptr);
			if (status != LUA_OK)
			{
				auto result = Utils::Outcome(call, status, 0, false);
				lua_settop(call.State, base);
				return result;
			}
			lua_rawgetfield(call.State, LUA_REGISTRYINDEX, "Engine.ErrorHandler");
			lua_rawgetfield(call.State, LUA_REGISTRYINDEX, "Engine.NativeEntry");
			status = lua_pcall(call.State, 0, LUA_MULTRET, base + 1);
			auto result = Utils::Outcome(call, status, lua_gettop(call.State) - base - 1, status != LUA_OK);
			if (result.Failure)
				lua_settop(call.State, base);
			else
				lua_remove(call.State, base + 1);
			return result;
		}

		Result<ScriptCallResult> ProtectedResume(ScriptCall& call, lua_State* from, int argumentCount)
		{
			if (!call.State || !call.Sandbox || lua_mainthread(call.State) == call.State || argumentCount < 0 || argumentCount > lua_gettop(call.State) || (from && lua_mainthread(from) != lua_mainthread(call.State)))
				return MakeError(ErrorCode::InvalidArgument, "invalid protected coroutine resume");
			ENGINE_TRY(Detail::SandboxAccess::ThreadCall(*call.Sandbox, call.State));
			ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*call.Sandbox, call.Execution ? call.Execution->Origin : ScriptExecutionOrigin::Pure));
			Utils::ProtectedScope scope{ call, call.Execution, call.Engine ? Detail::ScriptEngineAccess::ExchangeActiveThread(*call.Engine, call.State) : nullptr };
			call.Execution = Detail::SandboxAccess::GetExecution(*call.Sandbox);
			const int status = lua_resume(call.State, from, argumentCount);
			return Utils::Outcome(call, status, lua_gettop(call.State), false);
		}

	}

}
