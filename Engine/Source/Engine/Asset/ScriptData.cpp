#include "EnginePCH.h"
#include "Engine/Asset/ScriptData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace Engine {

	namespace Utils {

		static constexpr size_t MaxScriptSchemaDepth = 64;

		static Status CheckScriptText(std::string_view text)
		{
			if (text.size() > std::numeric_limits<uint32_t>::max() || !IsValidUtf8(text) || text.find('\0') != std::string_view::npos)
				return MakeError(ErrorCode::Validation, "script metadata must be bounded UTF-8 without NUL");
			return {};
		}

		static Status CheckScriptPath(std::string_view path)
		{
			ENGINE_TRY(VfsPath::ValidateRelativePath(path));
			if (!path.starts_with("Assets/") || path.size() == 7)
				return MakeError(ErrorCode::Validation, "script metadata path '{}' must be below Assets", path);
			return {};
		}

		static bool IsScriptFieldKind(FieldType kind)
		{
			switch (kind)
			{
				case FieldType::Float:
				case FieldType::Int32:
				case FieldType::Bool:
				case FieldType::String:
				case FieldType::Vec3:
				case FieldType::Color4:
				case FieldType::Quat:
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				case FieldType::Enum:
				case FieldType::Array:     return true;
				default:                   return false;
			}
		}

		static bool IsReservedScriptField(std::string_view name)
		{
			static constexpr std::string_view Reserved[] = { "Entity", "Fields", "OnCreate", "OnStart", "OnFixedUpdate",
				"OnUpdate", "OnLateUpdate", "OnDestroy", "OnEnable", "OnDisable", "OnCollisionEnter", "OnCollisionExit",
				"OnTriggerEnter", "OnTriggerExit", "OnHotReload" };
			return name.starts_with("__") || std::find(std::begin(Reserved), std::end(Reserved), name) != std::end(Reserved);
		}

		static Status CheckScriptField(const ScriptFieldSchema& field, bool element, std::set<const ScriptFieldSchema*>& active)
		{
			if (active.size() >= MaxScriptSchemaDepth || !active.insert(&field).second)
				return MakeError(ErrorCode::Validation, "script field schema is cyclic or exceeds {} levels", MaxScriptSchemaDepth);
			ENGINE_TRY(CheckScriptText(field.Name));
			ENGINE_TRY(CheckScriptText(field.Tooltip));
			if (element ? !field.Name.empty() : (field.Name.empty() || IsReservedScriptField(field.Name)))
				return MakeError(ErrorCode::Validation, "invalid or reserved script field name '{}'", field.Name);
			if (!IsScriptFieldKind(field.Type))
				return MakeError(ErrorCode::Validation, "unsupported script field kind");
			const auto& meta = field.Meta;
			if (meta.MinMagnitude || !meta.Unit.empty() || meta.ReadOnly || meta.Hidden || meta.Virtual || !meta.Scriptable || !meta.Serialized || meta.Modes != RunModes::All)
				return MakeError(ErrorCode::Validation, "unsupported script field metadata on '{}'", field.Name);
			for (const auto bound : { meta.Min, meta.Max, meta.Step })
			{
				if (bound && (!std::isfinite(*bound) || !IsNumericFieldType(field.Type)))
					return MakeError(ErrorCode::Validation, "numeric options require a numeric field and finite values");
			}
			if ((meta.Min && meta.Max && *meta.Min > *meta.Max) || (meta.Step && *meta.Step <= 0))
				return MakeError(ErrorCode::Validation, "invalid script field bounds or step");
			if (field.Type == FieldType::AssetRef)
			{
				const auto type = AssetTypeFromString(meta.AssetFilter);
				if (!type || *type == AssetType::None)
					return MakeError(ErrorCode::Validation, "unknown script asset filter '{}'", meta.AssetFilter);
			}
			else if (!meta.AssetFilter.empty())
				return MakeError(ErrorCode::Validation, "asset filter on a non-asset field");
			if (field.Type == FieldType::Enum)
			{
				if (field.EnumValues.empty() || field.EnumValues.size() > std::numeric_limits<uint32_t>::max())
					return MakeError(ErrorCode::Validation, "script enum must contain a bounded nonempty list");
				std::set<std::string_view> names;
				for (const auto& name : field.EnumValues)
				{
					ENGINE_TRY(CheckScriptText(name));
					if (name.empty() || !names.insert(name).second)
						return MakeError(ErrorCode::Validation, "empty or duplicate script enum value");
				}
			}
			else if (!field.EnumValues.empty())
				return MakeError(ErrorCode::Validation, "enum values on a non-enum field");
			if (field.Type == FieldType::Array)
			{
				if (!field.Element)
					return MakeError(ErrorCode::Validation, "script array needs an element schema");
				ENGINE_TRY(CheckScriptField(*field.Element, true, active));
			}
			else if (field.Element)
				return MakeError(ErrorCode::Validation, "element schema on a non-array field");
			active.erase(&field);
			return {};
		}

		struct ScriptSchemaStorage
		{
			std::vector<Scope<TypeInfo>> Types{};
			std::vector<Scope<EnumInfo>> Enums{};
			std::vector<Scope<FieldInfo>> Fields{};

			Result<const FieldInfo*> Build(const ScriptFieldSchema& schema)
			{
				TypeInfo::Specification type{};
				type.Kind = schema.Type;
				type.Name = FieldTypeToString(schema.Type);
				type.AssetTypeName = schema.Meta.AssetFilter;
				if (schema.Element)
				{
					ENGINE_TRY_ASSIGN(type.ElementSchema, Build(*schema.Element));
					type.Element = &type.ElementSchema->GetType();
				}
				if (schema.Type == FieldType::Enum)
				{
					auto enumeration = CreateScope<EnumInfo>("ScriptEnum", "Script field choices.");
					for (size_t i = 0; i < schema.EnumValues.size(); ++i)
						enumeration->AddEntry({ schema.EnumValues[i], static_cast<int64_t>(i), schema.EnumValues[i] });
					type.Enum = enumeration.get();
					Enums.push_back(std::move(enumeration));
				}
				Types.push_back(CreateScope<TypeInfo>(std::move(type)));
				FieldInfo::Specification field{};
				field.Name = schema.Name.empty() ? "Element" : schema.Name;
				field.Description = schema.Tooltip.empty() ? "Script field." : schema.Tooltip;
				field.Type = Types.back().get();
				field.Meta = schema.Meta;
				Fields.push_back(CreateScope<FieldInfo>(std::move(field)));
				const FieldInfo* result = Fields.back().get();
				ValidationContext validation;
				result->ValidateJson(JsonReader(schema.DefaultValue.Get()), ResolveContext{}, validation);
				ENGINE_TRY(validation.ToStatus("script field default"));
				return result;
			}
		};

		static Status CheckScriptFields(const ScriptData& script)
		{
			if (script.Kind != ScriptKind::Behaviour && script.Kind != ScriptKind::Module && script.Kind != ScriptKind::TestSuite)
				return MakeError(ErrorCode::Validation, "unknown script kind");
			ENGINE_TRY(CheckScriptText(script.Name));
			if ((script.Kind == ScriptKind::Module && !script.Name.empty()) || (script.Kind != ScriptKind::Module && script.Name.empty()))
				return MakeError(ErrorCode::Validation, "script name does not match its kind");
			if (script.Kind != ScriptKind::Behaviour && !script.Fields.empty())
				return MakeError(ErrorCode::Validation, "only Behaviour scripts declare fields");
			if (script.Fields.size() > std::numeric_limits<uint32_t>::max())
				return MakeError(ErrorCode::Validation, "too many script fields");
			std::string_view previous;
			for (const auto& field : script.Fields)
			{
				if (!previous.empty() && previous >= field.Name)
					return MakeError(ErrorCode::Validation, "script fields must have unique canonical names");
				std::set<const ScriptFieldSchema*> active;
				ENGINE_TRY(CheckScriptField(field, false, active));
				previous = field.Name;
			}
			return {};
		}

		static Status CheckScriptMap(const ScriptSourceMap& map)
		{
			ENGINE_TRY(CheckScriptText(map.ChunkName));
			ENGINE_TRY(CheckScriptText(map.JsonPointer));
			if (map.ChunkName.size() < 2 || (map.ChunkName.front() != '@' && map.ChunkName.front() != '='))
				return MakeError(ErrorCode::Validation, "script chunk label must begin with @ or = and have a name");
			if (map.Path.empty())
			{
				if (map.ChunkName.front() != '=')
					return MakeError(ErrorCode::Validation, "a fileless script requires a synthetic diagnostic label");
			}
			else
				ENGINE_TRY(CheckScriptPath(map.Path));
			if (map.ChunkName.front() == '@')
				ENGINE_TRY(CheckScriptPath(std::string_view(map.ChunkName).substr(1)));
			if (!map.JsonPointer.empty() && map.JsonPointer.front() != '/')
				return MakeError(ErrorCode::Validation, "invalid script source JSON pointer");
			for (size_t i = 0; i < map.JsonPointer.size(); ++i)
			{
				if (map.JsonPointer[i] == '~' && (++i == map.JsonPointer.size() || (map.JsonPointer[i] != '0' && map.JsonPointer[i] != '1')))
					return MakeError(ErrorCode::Validation, "invalid escape in script source JSON pointer");
			}
			if (map.GeneratedPrefixLines > 1 || map.LineOffsets.empty() || map.LineOffsets.front() != 0 || map.LineOffsets.size() > std::numeric_limits<uint32_t>::max())
				return MakeError(ErrorCode::Validation, "invalid script source offsets");
			for (size_t i = 0; i < map.LineOffsets.size(); ++i)
			{
				if (map.LineOffsets[i] > map.SourceByteCount || (i != 0 && map.LineOffsets[i - 1] >= map.LineOffsets[i]))
					return MakeError(ErrorCode::Validation, "script line offsets must increase within authored source bytes");
			}
			return {};
		}

		static auto ScriptRequireKey(const ScriptRequire& edge)
		{
			return std::tie(edge.From, edge.Request, edge.Path, edge.Handle);
		}

		static Status CheckScriptArtifact(const ScriptData& script)
		{
			ENGINE_TRY(CheckScriptFields(script));
			ENGINE_TRY(CheckScriptMap(script.SourceMap));
			// An envelope check is not bytecode verification. Producers must be trusted engine compilers/cache/paks.
			if (script.Bytecode.empty() || script.Bytecode.front() == std::byte{ 0 } || script.Bytecode.size() > std::numeric_limits<uint32_t>::max() || script.Requires.size() > std::numeric_limits<uint32_t>::max())
				return MakeError(ErrorCode::Validation, "missing, excessive or error-encoded script bytecode/dependencies");
			for (size_t i = 0; i < script.Requires.size(); ++i)
			{
				const auto& edge = script.Requires[i];
				ENGINE_TRY(CheckScriptPath(edge.From));
				ENGINE_TRY(CheckScriptPath(edge.Path));
				ENGINE_TRY(CheckScriptText(edge.Request));
				if (!edge.From.ends_with(".luau") || !edge.Path.ends_with(".luau") || !edge.Handle.IsValid() || (!edge.Request.starts_with("./") && !edge.Request.starts_with("../")) || (i != 0 && ScriptRequireKey(script.Requires[i - 1]) >= ScriptRequireKey(edge)))
					return MakeError(ErrorCode::Validation, "invalid or noncanonical script require record");
			}
			return {};
		}

		static Status WriteScriptField(BinaryWriter& writer, const ScriptFieldSchema& field)
		{
			writer.WriteString(field.Name);
			writer.WriteString(FieldTypeToString(field.Type));
			ENGINE_TRY_ASSIGN(const std::string defaults, JsonWriter::Write(field.DefaultValue.Get(), JsonStyle::Minified));
			ENGINE_TRY(CheckScriptText(defaults));
			writer.WriteString(defaults);
			// Binary64 retains exact bounds; JsonWriter deliberately rounds JSON numbers to the engine's f32 type.
			for (const auto bound : { field.Meta.Min, field.Meta.Max, field.Meta.Step })
			{
				writer.WriteBool(bound.has_value());
				if (bound)
					writer.WriteF64(*bound);
			}
			writer.WriteString(field.Tooltip);
			writer.WriteString(field.Meta.AssetFilter);
			writer.WriteU32(static_cast<uint32_t>(field.EnumValues.size()));
			for (const auto& value : field.EnumValues)
				writer.WriteString(value);
			writer.WriteBool(field.Element != nullptr);
			if (field.Element)
				ENGINE_TRY(WriteScriptField(writer, *field.Element));
			return {};
		}

		static Result<uint32_t> ReadScriptCount(BinaryReader& reader, size_t minimumSize)
		{
			ENGINE_TRY_ASSIGN(const uint32_t count, reader.ReadU32());
			if (count > reader.GetRemaining() / minimumSize)
				return MakeError(ErrorCode::Parse, "script record count exceeds remaining payload");
			return count;
		}

		static Result<ScriptFieldSchema> ReadScriptField(BinaryReader& reader, size_t depth)
		{
			if (depth >= MaxScriptSchemaDepth)
				return MakeError(ErrorCode::Validation, "script schema exceeds maximum nesting");
			ScriptFieldSchema field;
			ENGINE_TRY_ASSIGN(field.Name, reader.ReadString());
			ENGINE_TRY_ASSIGN(const std::string kind, reader.ReadString());
			std::optional<FieldType> parsedKind;
			for (const auto candidate : { FieldType::Bool, FieldType::Int32, FieldType::Float, FieldType::Vec3, FieldType::Quat,
					 FieldType::Color4, FieldType::String, FieldType::EntityRef, FieldType::AssetRef, FieldType::Enum, FieldType::Array })
			{
				if (FieldTypeToString(candidate) == kind)
					parsedKind = candidate;
			}
			if (!parsedKind || !IsScriptFieldKind(*parsedKind))
				return MakeError(ErrorCode::Validation, "unknown script field kind '{}'", kind);
			field.Type = *parsedKind;
			ENGINE_TRY_ASSIGN(const std::string defaults, reader.ReadString());
			ENGINE_TRY_ASSIGN(Json value, JsonReader::Parse(defaults));
			ENGINE_TRY_ASSIGN(const std::string canonicalDefault, JsonWriter::Write(value, JsonStyle::Minified));
			if (defaults != canonicalDefault)
				return MakeError(ErrorCode::Validation, "noncanonical script field default JSON");
			field.DefaultValue = VariantValue(std::move(value));
			for (auto* bound : { &field.Meta.Min, &field.Meta.Max, &field.Meta.Step })
			{
				ENGINE_TRY_ASSIGN(const bool present, reader.ReadBool());
				if (present)
				{
					ENGINE_TRY_ASSIGN(const double number, reader.ReadF64());
					*bound = number;
				}
			}
			ENGINE_TRY_ASSIGN(field.Tooltip, reader.ReadString());
			ENGINE_TRY_ASSIGN(field.Meta.AssetFilter, reader.ReadString());
			ENGINE_TRY_ASSIGN(const uint32_t choices, ReadScriptCount(reader, 4));
			for (uint32_t i = 0; i < choices; ++i)
			{
				ENGINE_TRY_ASSIGN(std::string name, reader.ReadString());
				field.EnumValues.push_back(std::move(name));
			}
			ENGINE_TRY_ASSIGN(const bool element, reader.ReadBool());
			if (element)
			{
				ENGINE_TRY_ASSIGN(ScriptFieldSchema nested, ReadScriptField(reader, depth + 1));
				field.Element = CreateRef<const ScriptFieldSchema>(std::move(nested));
			}
			return field;
		}

	}

	struct ScriptFieldSchemaSource::State
	{
		std::map<AssetHandle, AssetRef<ScriptData>> Scripts{};
		Utils::ScriptSchemaStorage Storage{};
		std::map<AssetHandle, std::map<std::string, const FieldInfo*, std::less<>>> Fields{};
	};

	ScriptFieldSchemaSource::ScriptFieldSchemaSource(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ScriptFieldSchemaSource::~ScriptFieldSchemaSource() = default;

	Result<Buffer> CookScript(const ScriptData& script, uint32_t importerVersion)
	{
		ENGINE_TRY(Utils::CheckScriptArtifact(script));
		Utils::ScriptSchemaStorage validation;
		for (const auto& field : script.Fields)
			ENGINE_TRY(validation.Build(field));
		BinaryWriter writer;
		writer.WriteU32(ScriptData::CompilerAbiVersion);
		writer.WriteU8(static_cast<uint8_t>(script.Kind));
		writer.WriteU8(0);
		writer.WriteU16(0);
		writer.WriteString(script.Name);
		writer.WriteU32(static_cast<uint32_t>(script.Bytecode.size()));
		writer.WriteBytes(script.Bytecode);
		writer.WriteU32(static_cast<uint32_t>(script.Fields.size()));
		for (const auto& field : script.Fields)
			ENGINE_TRY(Utils::WriteScriptField(writer, field));
		writer.WriteU32(static_cast<uint32_t>(script.Requires.size()));
		for (const auto& edge : script.Requires)
		{
			writer.WriteString(edge.From);
			writer.WriteString(edge.Request);
			writer.WriteString(edge.Path);
			writer.WriteU64(edge.Handle.GetValue());
		}
		const auto& map = script.SourceMap;
		writer.WriteString(map.ChunkName);
		writer.WriteString(map.Path);
		writer.WriteU64(map.SourceHash);
		writer.WriteU32(map.SourceByteCount);
		writer.WriteU32(static_cast<uint32_t>(map.LineOffsets.size()));
		for (const auto offset : map.LineOffsets)
			writer.WriteU32(offset);
		writer.WriteString(map.JsonPointer);
		writer.WriteU32(map.GeneratedPrefixLines);
		return WriteCookedArtifact(AssetType::Script, ScriptData::FormatVersion, importerVersion, writer.GetData());
	}

	Result<AssetRef<ScriptData>> LoadCookedScript(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const auto artifact, ReadCookedArtifact(cooked, AssetType::Script, ScriptData::FormatVersion));
		BinaryReader reader(artifact.Payload);
		ENGINE_TRY_ASSIGN(const uint32_t abi, reader.ReadU32());
		if (abi != ScriptData::CompilerAbiVersion)
			return MakeError(ErrorCode::UnsupportedVersion, "unsupported script compiler ABI {}", abi);
		auto script = CreateRef<ScriptData>();
		ENGINE_TRY_ASSIGN(const uint8_t kind, reader.ReadU8());
		script->Kind = static_cast<ScriptKind>(kind);
		ENGINE_TRY_ASSIGN(const uint8_t reservedByte, reader.ReadU8());
		ENGINE_TRY_ASSIGN(const uint16_t reservedWord, reader.ReadU16());
		if (reservedByte != 0 || reservedWord != 0)
			return MakeError(ErrorCode::Parse, "nonzero reserved script header bytes");
		ENGINE_TRY_ASSIGN(script->Name, reader.ReadString());
		ENGINE_TRY_ASSIGN(const uint32_t byteCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const auto bytes, reader.ReadBytes(byteCount));
		script->Bytecode.assign(bytes.begin(), bytes.end());
		ENGINE_TRY_ASSIGN(const uint32_t fieldCount, Utils::ReadScriptCount(reader, 24));
		for (uint32_t i = 0; i < fieldCount; ++i)
		{
			ENGINE_TRY_ASSIGN(ScriptFieldSchema field, Utils::ReadScriptField(reader, 0));
			script->Fields.push_back(std::move(field));
		}
		ENGINE_TRY_ASSIGN(const uint32_t requireCount, Utils::ReadScriptCount(reader, 20));
		for (uint32_t i = 0; i < requireCount; ++i)
		{
			ScriptRequire edge;
			ENGINE_TRY_ASSIGN(edge.From, reader.ReadString());
			ENGINE_TRY_ASSIGN(edge.Request, reader.ReadString());
			ENGINE_TRY_ASSIGN(edge.Path, reader.ReadString());
			ENGINE_TRY_ASSIGN(const uint64_t handle, reader.ReadU64());
			edge.Handle = AssetHandle(handle);
			script->Requires.push_back(std::move(edge));
		}
		auto& map = script->SourceMap;
		ENGINE_TRY_ASSIGN(map.ChunkName, reader.ReadString());
		ENGINE_TRY_ASSIGN(map.Path, reader.ReadString());
		ENGINE_TRY_ASSIGN(map.SourceHash, reader.ReadU64());
		ENGINE_TRY_ASSIGN(map.SourceByteCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t lineCount, Utils::ReadScriptCount(reader, 4));
		ENGINE_TRY_ASSIGN(map.LineOffsets, reader.ReadArray<uint32_t>(lineCount));
		ENGINE_TRY_ASSIGN(map.JsonPointer, reader.ReadString());
		ENGINE_TRY_ASSIGN(map.GeneratedPrefixLines, reader.ReadU32());
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "trailing script payload bytes");
		ENGINE_TRY(Utils::CheckScriptArtifact(*script));
		Utils::ScriptSchemaStorage validation;
		for (const auto& field : script->Fields)
			ENGINE_TRY(validation.Build(field));
		return AssetRef<ScriptData>(std::move(script));
	}

	Result<Ref<const ScriptFieldSchemaSource>> ScriptFieldSchemaSource::Create(std::map<AssetHandle, AssetRef<ScriptData>> scripts)
	{
		for (const auto& [handle, script] : scripts)
		{
			if (!handle.IsValid() || !script)
				return MakeError(ErrorCode::Validation, "script schema snapshot contains a null handle or asset");
			ENGINE_TRY(Utils::CheckScriptFields(*script));
		}
		auto source = CreateRef<ScriptFieldSchemaSource>(ConstructionKey{});
		source->m_State->Scripts = std::move(scripts);
		for (const auto& [handle, script] : source->m_State->Scripts)
		{
			auto& fields = source->m_State->Fields[handle];
			for (const auto& schema : script->Fields)
			{
				ENGINE_TRY_ASSIGN(const FieldInfo* field, source->m_State->Storage.Build(schema));
				fields.emplace(schema.Name, field);
			}
		}
		return Ref<const ScriptFieldSchemaSource>(std::move(source));
	}

	Result<const FieldInfo*> ScriptFieldSchemaSource::FindField(UUID owner, std::string_view name) const
	{
		const auto found = m_State->Fields.find(owner);
		if (found == m_State->Fields.end())
			return MakeError(ErrorCode::NotFound, "unknown script schema {}", owner);
		if (const auto field = found->second.find(name); field != found->second.end())
			return field->second;
		const auto names = GetFieldNames(owner);
		const auto suggestions = FuzzySuggest(name, names);
		return std::unexpected(Error(ErrorCode::NotFound, std::format("unknown script field '{}'", name)).WithHint(MakeDidYouMeanHint(suggestions)));
	}

	std::vector<std::string> ScriptFieldSchemaSource::GetFieldNames(UUID owner) const
	{
		std::vector<std::string> names;
		if (const auto found = m_State->Fields.find(owner); found != m_State->Fields.end())
		{
			for (const auto& [name, field] : found->second)
			{
				static_cast<void>(field);
				names.push_back(name);
			}
		}
		return names;
	}

	Result<const ScriptFieldSchema*> ScriptFieldSchemaSource::FindSchema(UUID owner, std::string_view name) const
	{
		ENGINE_TRY(FindField(owner, name));
		const auto& fields = m_State->Scripts.find(owner)->second->Fields;
		const auto found = std::lower_bound(fields.begin(), fields.end(), name, [](const ScriptFieldSchema& field, std::string_view key)
		{
			return field.Name < key;
		});
		return &*found;
	}

}
