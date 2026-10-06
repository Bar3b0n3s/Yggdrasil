#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Engine {

	// Strict, located reading of JSON (Architecture §4.6, §6). Every error carries the JSON pointer (RFC 6901) of the
	// offending value in its ErrorLocation, so a loader reports "/Entities/12/Components/RigidBody/Mass: expected number,
	// got string"; an error about the document root carries the pointer "" (set, not absent: a type error of the root or
	// "missing required field 'Format'" is located there). Error codes: Parse for syntax and depth (Parse() only),
	// Validation for a value of the wrong type, a missing
	// required member, an out-of-range number or a malformed UUID, UnsupportedVersion for a too-new file
	// (ReadFormatHeader). Unknown members are not errors here: FindUnknownMembers lists them and the loader decides
	// (warning by default, error with --strict, §6).
	//
	// A JsonReader is a cheap, non-owning view of one value plus its pointer. The viewed tree must outlive the reader and
	// every reader derived from it. Reading never modifies the tree; readers of the same tree may be used from several
	// threads at once.
	class JsonReader
	{
	public:
		// Parses `text` as strict RFC 8259 JSON: exactly one value (surrounding whitespace allowed), no comments, no
		// trailing commas, no NaN or Infinity literals, valid UTF-8 throughout, no duplicate key within an object, and
		// arrays and objects nested at most MaxJsonDepth deep. Errors: Parse, located at the Line and Column (1-based;
		// columns count bytes) of the first problem (for too deep input, the bracket that opens level MaxJsonDepth + 1);
		// a duplicate key is also located at the member's JSON pointer. Never throws (nlohmann's non-throwing SAX path)
		// and never recurses per nesting level while parsing.
		[[nodiscard]] static Result<Json> Parse(std::string_view text);

		// A reader of `value`, whose JSON pointer is `pointer` ("" for a document root).
		explicit JsonReader(const Json& value, std::string pointer = {});

		[[nodiscard]] const Json& GetValue() const { return *m_Value; }
		[[nodiscard]] const std::string& GetPointer() const { return m_Pointer; }

		[[nodiscard]] JsonType GetType() const;
		[[nodiscard]] bool IsNull() const;
		[[nodiscard]] bool IsObject() const;
		[[nodiscard]] bool IsArray() const;

		// Succeeds when the value has type `expected`; Float accepts every number (an Integer is a valid float).
		// Errors: Validation "expected <type>, got <type>".
		[[nodiscard]] Status ExpectType(JsonType expected) const;

		// Typed reads of this value. Errors (Validation, located here):
		//   - "expected <type>, got <type>" for the wrong JSON type;
		//   - integers: only Integer numbers (no fraction or exponent), and within the target type's range;
		//   - floats: any number, finite, and for float within +-FLT_MAX after rounding to the nearest float;
		//   - UUID: a string accepted by UUID::FromString ("expected a 16-digit hex UUID string"); "0000000000000000"
		//     reads as the invalid UUID, which callers that need a valid ID reject.
		[[nodiscard]] Result<bool> ReadBool() const;
		[[nodiscard]] Result<int32_t> ReadInt32() const;
		[[nodiscard]] Result<uint32_t> ReadUInt32() const;
		[[nodiscard]] Result<int64_t> ReadInt64() const;
		[[nodiscard]] Result<uint64_t> ReadUInt64() const;
		[[nodiscard]] Result<float> ReadFloat() const;
		[[nodiscard]] Result<double> ReadDouble() const;
		[[nodiscard]] Result<std::string> ReadString() const;
		[[nodiscard]] Result<UUID> ReadUUID() const;

		// The typed read for T in { bool, int32_t, uint32_t, int64_t, uint64_t, float, double, std::string, UUID }.
		template<typename T>
		[[nodiscard]] Result<T> Read() const;

		// True when this is an object with member `key`.
		[[nodiscard]] bool HasMember(std::string_view key) const;

		// The member `key`, with pointer "<this>/<escaped key>". Errors: Validation "expected object, got <type>", or
		// "missing required field '<key>'" located at this object.
		[[nodiscard]] Result<JsonReader> GetMember(std::string_view key) const;

		// The member `key`, or nullopt when this is not an object or has no such member (for optional fields of a value
		// already checked with ExpectType(JsonType::Object)).
		[[nodiscard]] std::optional<JsonReader> FindMember(std::string_view key) const;

		// GetMember(key), then Read<T>().
		template<typename T>
		[[nodiscard]] Result<T> ReadMember(std::string_view key) const;

		// Member names in document order. Errors: Validation when this is not an object.
		[[nodiscard]] Result<std::vector<std::string>> GetMemberNames() const;

		// The member names that are not in `knownKeys`, in document order. Errors: Validation when this is not an object.
		[[nodiscard]] Result<std::vector<std::string>> FindUnknownMembers(std::span<const std::string_view> knownKeys) const;

		// Errors: Validation when this is not an array.
		[[nodiscard]] Result<size_t> GetArraySize() const;

		// Element `index`, with pointer "<this>/<index>". Errors: Validation when this is not an array or `index` is out
		// of range.
		[[nodiscard]] Result<JsonReader> GetElement(size_t index) const;

		// Checks the header every authored file starts with (§6): "Format" must be the string `expectedFormat`
		// (Validation otherwise) and "Version" an integer from `minimumVersion` to `supportedVersion`
		// (minimumVersion <= supportedVersion, asserted). A version below the minimum, or not an integer, is Validation;
		// a newer version is UnsupportedVersion naming both versions. Returns the file's version, for migrations: a
		// format whose oldest fixtures predate version 1 passes minimumVersion 0 so that "v0 fixture upgrades to v1"
		// (Roadmap M3) reads its header here.
		[[nodiscard]] Result<uint32_t> ReadFormatHeader(std::string_view expectedFormat, uint32_t minimumVersion,
			uint32_t supportedVersion) const;

		// An error of `code` located at this value's pointer, for a loader's own checks (a number below a field's Min, an
		// unknown enum name).
		[[nodiscard]] Error MakeLocatedError(ErrorCode code, std::string message) const;

		// RFC 6901 pointer composition: `pointer` followed by "/" and `key` with '~' escaped as "~0" and '/' as "~1", or
		// by "/" and the decimal `index`.
		[[nodiscard]] static std::string AppendPointer(std::string_view pointer, std::string_view key);
		[[nodiscard]] static std::string AppendPointer(std::string_view pointer, size_t index);
	private:
		const Json* m_Value = nullptr;
		std::string m_Pointer;
	};

	template<typename T>
	Result<T> JsonReader::Read() const
	{
		if constexpr (std::is_same_v<T, bool>)
			return ReadBool();
		else if constexpr (std::is_same_v<T, int32_t>)
			return ReadInt32();
		else if constexpr (std::is_same_v<T, uint32_t>)
			return ReadUInt32();
		else if constexpr (std::is_same_v<T, int64_t>)
			return ReadInt64();
		else if constexpr (std::is_same_v<T, uint64_t>)
			return ReadUInt64();
		else if constexpr (std::is_same_v<T, float>)
			return ReadFloat();
		else if constexpr (std::is_same_v<T, double>)
			return ReadDouble();
		else if constexpr (std::is_same_v<T, std::string>)
			return ReadString();
		else if constexpr (std::is_same_v<T, UUID>)
			return ReadUUID();
		else
			static_assert(sizeof(T) == 0,
				"JsonReader::Read<T> supports bool, int32_t, uint32_t, int64_t, uint64_t, float, double, std::string and UUID");
	}

	template<typename T>
	Result<T> JsonReader::ReadMember(std::string_view key) const
	{
		Result<JsonReader> member = GetMember(key);
		if (!member)
			return std::unexpected(std::move(member).error());
		return member->Read<T>();
	}

}
