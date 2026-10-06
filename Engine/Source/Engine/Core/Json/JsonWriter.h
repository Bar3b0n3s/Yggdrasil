#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	enum class JsonStyle : uint8_t
	{
		Pretty,  // authored files (§6): tab-indented, LF, final newline
		Minified // cooked scene, prefab, material and project payloads (§6.8): no whitespace, no final newline
	};

	// The canonical JSON writer (Architecture §6). Every authored file is written through it, which is what makes
	// "load then save is byte-identical" hold. The caller decides the member order (registry order for reflected types,
	// "Format" and "Version" first); the writer decides everything else:
	//   - Pretty layout: one tab per nesting level. An object puts each member on its own line as "<key>": <value>, with
	//     "," at the end of every member line but the last; "}" closes on its own line. An array whose elements are all
	//     scalars (null, boolean, number, string) stays on one line, "[1, 2, 3]" with ", " separators; an array with at
	//     least one object or array element puts each element on its own line like an object. Empty containers are
	//     "{}" and "[]". The document ends with exactly one "\n". Minified drops every space and line break.
	//   - Floats: the shortest text that round-trips the float value (std::to_chars on float, so 0.1f is "0.1", 1.0f is
	//     "1", 1e8f is "1e+08"); -0 is written as "0"; NaN and infinities are rejected.
	//   - Integers: decimal, exact over the full int64 and uint64 ranges. UUIDs: 16-digit lowercase hex strings (§4.8).
	//   - Strings and keys: UTF-8 written as is, except '"' -> \", '\' -> \\, the control characters \b \f \n \r \t,
	//     and every other code point below U+0020 as \u00xx (lowercase hex). Invalid UTF-8 is rejected.
	//   - Map fields (std::map<std::string, T>) are written with keys in std::map order, which is byte-wise (§5.4).
	// Data problems (a non-finite float, invalid UTF-8, a duplicate key in one object) do not stop the writer; Finish
	// returns a Validation error located at the JSON pointer of the first one. Structural misuse (a value without a key in
	// an object, a key outside an object, unbalanced Begin/End, Finish with open containers, any call after Finish) is a
	// programmer error (asserted). Not thread-safe (one owner).
	class JsonWriter
	{
	public:
		explicit JsonWriter(JsonStyle style = JsonStyle::Pretty);
		~JsonWriter();

		JsonWriter(const JsonWriter&) = delete;
		JsonWriter& operator=(const JsonWriter&) = delete;

		void BeginObject();
		void EndObject();
		void BeginArray();
		void EndArray();

		// The key of the next value; only directly inside an object.
		void WriteKey(std::string_view key);

		void WriteNull();
		void WriteBool(bool value);
		void WriteInt(int64_t value);
		void WriteUInt(uint64_t value);
		void WriteFloat(float value);
		void WriteString(std::string_view value);
		void WriteUUID(UUID value);

		// Writes a whole tree, object members in the tree's order. Float numbers are written as float (the shortest form
		// of the value rounded to float; outside the float range is a data error), integers exactly. The tree must nest
		// at most MaxJsonDepth arrays and objects deep (asserted; JsonReader::Parse never produces deeper trees).
		void WriteJson(const Json& value);

		// Writes `map` as an object with keys in std::map (byte-wise) order; `writeValue(writer, value)` writes each value.
		template<typename T, typename WriteValue>
		void WriteMap(const std::map<std::string, T>& map, WriteValue&& writeValue)
		{
			BeginObject();
			for (const auto& [key, value] : map)
			{
				WriteKey(key);
				writeValue(*this, value);
			}
			EndObject();
		}

		// The finished document. Exactly one root value must have been written and every container closed. Errors:
		// Validation for the first data problem (see the class comment).
		[[nodiscard]] Result<std::string> Finish();

		// Writes `value` as one document.
		[[nodiscard]] static Result<std::string> Write(const Json& value, JsonStyle style = JsonStyle::Pretty);
	private:
		struct State; // the document under construction and the open containers (JsonWriter.cpp)
	private:
		Scope<State> m_State;
	};

}
