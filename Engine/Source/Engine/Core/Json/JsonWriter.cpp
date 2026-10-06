#include "EnginePCH.h"
#include "Engine/Core/Json/JsonWriter.h"

#include <nlohmann/json.hpp>

// M1 contract stub (Roadmap rule 3): stream C implements the canonical writer documented in JsonWriter.h.

namespace Engine {

	struct JsonWriter::State
	{
	};

	JsonWriter::JsonWriter(JsonStyle /*style*/)
		: m_State(CreateScope<State>())
	{
	}

	JsonWriter::~JsonWriter() = default;

	void JsonWriter::BeginObject()
	{
	}

	void JsonWriter::EndObject()
	{
	}

	void JsonWriter::BeginArray()
	{
	}

	void JsonWriter::EndArray()
	{
	}

	void JsonWriter::WriteKey(std::string_view /*key*/)
	{
	}

	void JsonWriter::WriteNull()
	{
	}

	void JsonWriter::WriteBool(bool /*value*/)
	{
	}

	void JsonWriter::WriteInt(int64_t /*value*/)
	{
	}

	void JsonWriter::WriteUInt(uint64_t /*value*/)
	{
	}

	void JsonWriter::WriteFloat(float /*value*/)
	{
	}

	void JsonWriter::WriteString(std::string_view /*value*/)
	{
	}

	void JsonWriter::WriteUUID(UUID /*value*/)
	{
	}

	void JsonWriter::WriteJson(const Json& /*value*/)
	{
	}

	Result<std::string> JsonWriter::Finish()
	{
		return MakeError(ErrorCode::Unsupported, "JsonWriter::Finish is an M1 contract stub");
	}

	Result<std::string> JsonWriter::Write(const Json& /*value*/, JsonStyle /*style*/)
	{
		return MakeError(ErrorCode::Unsupported, "JsonWriter::Write is an M1 contract stub");
	}

}
