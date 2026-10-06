#include "EnginePCH.h"
#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

// M1 contract stub (Roadmap rule 3): stream C implements strict parsing and located reads, and the Json.h helpers.

namespace Engine {

	std::string_view JsonTypeToString(JsonType /*type*/)
	{
		return {};
	}

	JsonType GetJsonType(const Json& /*value*/)
	{
		return JsonType::Null;
	}

	Result<Json> JsonReader::Parse(std::string_view /*text*/)
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::Parse is an M1 contract stub");
	}

	JsonReader::JsonReader(const Json& value, std::string pointer)
		: m_Value(&value), m_Pointer(std::move(pointer))
	{
	}

	JsonType JsonReader::GetType() const
	{
		return JsonType::Null;
	}

	bool JsonReader::IsNull() const
	{
		return false;
	}

	bool JsonReader::IsObject() const
	{
		return false;
	}

	bool JsonReader::IsArray() const
	{
		return false;
	}

	Status JsonReader::ExpectType(JsonType /*expected*/) const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ExpectType is an M1 contract stub");
	}

	Result<bool> JsonReader::ReadBool() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadBool is an M1 contract stub");
	}

	Result<int32_t> JsonReader::ReadInt32() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadInt32 is an M1 contract stub");
	}

	Result<uint32_t> JsonReader::ReadUInt32() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadUInt32 is an M1 contract stub");
	}

	Result<int64_t> JsonReader::ReadInt64() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadInt64 is an M1 contract stub");
	}

	Result<uint64_t> JsonReader::ReadUInt64() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadUInt64 is an M1 contract stub");
	}

	Result<float> JsonReader::ReadFloat() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadFloat is an M1 contract stub");
	}

	Result<double> JsonReader::ReadDouble() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadDouble is an M1 contract stub");
	}

	Result<std::string> JsonReader::ReadString() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadString is an M1 contract stub");
	}

	Result<UUID> JsonReader::ReadUUID() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadUUID is an M1 contract stub");
	}

	bool JsonReader::HasMember(std::string_view /*key*/) const
	{
		return false;
	}

	Result<JsonReader> JsonReader::GetMember(std::string_view /*key*/) const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::GetMember is an M1 contract stub");
	}

	std::optional<JsonReader> JsonReader::FindMember(std::string_view /*key*/) const
	{
		return std::nullopt;
	}

	Result<std::vector<std::string>> JsonReader::GetMemberNames() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::GetMemberNames is an M1 contract stub");
	}

	Result<std::vector<std::string>> JsonReader::FindUnknownMembers(std::span<const std::string_view> /*knownKeys*/) const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::FindUnknownMembers is an M1 contract stub");
	}

	Result<size_t> JsonReader::GetArraySize() const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::GetArraySize is an M1 contract stub");
	}

	Result<JsonReader> JsonReader::GetElement(size_t /*index*/) const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::GetElement is an M1 contract stub");
	}

	Result<uint32_t> JsonReader::ReadFormatHeader(std::string_view /*expectedFormat*/, uint32_t /*minimumVersion*/,
		uint32_t /*supportedVersion*/) const
	{
		return MakeError(ErrorCode::Unsupported, "JsonReader::ReadFormatHeader is an M1 contract stub");
	}

	Error JsonReader::MakeLocatedError(ErrorCode code, std::string message) const
	{
		return Error(code, std::move(message));
	}

	std::string JsonReader::AppendPointer(std::string_view /*pointer*/, std::string_view /*key*/)
	{
		return {};
	}

	std::string JsonReader::AppendPointer(std::string_view /*pointer*/, size_t /*index*/)
	{
		return {};
	}

}
