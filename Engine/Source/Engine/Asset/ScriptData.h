#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	// Classification is by the returned value's authenticated load-time registration, never by the filename or table shape.
	enum class ScriptKind : uint8_t
	{
		Behaviour,
		Module,
		TestSuite
	};

	// Persistent, VM-independent description of one Field constructor. Array owns exactly one immutable Element;
	// other kinds have none. Number/Integer/Vector/Color map to Float/Int32/Vec3/Color4. Enum names retain authored order.
	// DefaultValue uses reflection's JSON spelling, including null references and [x, y, z, w] quaternions.
	// Nested array elements retain their own metadata and defaults; these must not be flattened into the outer FieldMeta.
	// Omitted constructor defaults: Number f32 0, Integer 0, Bool false, String empty, Vector zero, Color white,
	// Quat identity, Entity/Asset null, Enum first authored value, Array empty. Extraction materializes these values.
	struct ScriptFieldSchema
	{
		std::string Name{}; // required on a top-level field; empty on an anonymous array element
		FieldType Type = FieldType::Float;
		VariantValue DefaultValue{};
		FieldMeta Meta{};
		std::string Tooltip{};
		std::vector<std::string> EnumValues{};
		Ref<const ScriptFieldSchema> Element{};
	};

	// One resolved require call. Paths are canonical project-relative Assets paths. From names the requiring module;
	// Path names the required module. Request retains the literal/runtime spelling for resolver and diagnostic parity.
	// Sorted by (From, Request, Path, Handle), unique. Handles are standalone Script assets, never dependency-file metas.
	struct ScriptRequire
	{
		std::string From{};
		std::string Request{};
		std::string Path{};
		AssetHandle Handle{};

		bool operator==(const ScriptRequire&) const = default;
	};

	// Maps compiled debug locations to authored source. Hash, byte count and zero-based UTF-8 LineOffsets describe the
	// exact authored bytes before any expression wrapper. Offsets begin with 0 and add the byte after every newline.
	// ChunkName is the authored diagnostic label, distinct from the private loaded-chunk identity (Sandbox).
	// Path/JsonPointer identify its diagnostic origin, never a require base.
	// Embedded line/column positions are within the decoded Luau string, not physical JSON document lines. Subtract
	// GeneratedPrefixLines once; columns are unchanged because generated text occupies separate lines. Syntax errors
	// in a generated suffix anchor at authored EOF with wrapper context. Unknown runtime positions remain 0.
	struct ScriptSourceMap
	{
		std::string ChunkName{}; // "@Assets/Scripts/Ball.luau" or a stable "=replay/<handle>/Expect/<index>" label
		std::string Path{};      // real project-relative diagnostic file (.luau or .replay); empty for fileless eval
		uint64_t SourceHash = 0; // XXH64 of the exact authored source bytes
		uint32_t SourceByteCount = 0;
		std::vector<uint32_t> LineOffsets{};
		std::string JsonPointer{};         // empty for a standalone chunk; RFC 6901, e.g. "/Expect/2/Luau" or eval's "/code"
		uint32_t GeneratedPrefixLines = 0; // 0 for unchanged chunks, 1 for the supported expression wrapper
	};

	// Immutable once published through AssetRef. This is CPU data only: no VM, Luau analysis or registry pointers.
	// Module and TestSuite have no Fields. Name is Script.Define's name or Test.Suite's name, empty for Module.
	// A Module is a valid asset but cannot be attached to ScriptComponent.Script.
	struct ScriptData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Script;
		static constexpr uint16_t FormatVersion = 1;
		// Engine-owned fingerprint version for the pinned Luau release, compiler options and deterministic builtin policy.
		// Bump together with the importer version when any of those change. Asset does not include Luau to discover it.
		static constexpr uint32_t CompilerAbiVersion = 1;

		ScriptData()
			: Asset(StaticType)
		{
		}

		ScriptKind Kind = ScriptKind::Module;
		std::string Name{};
		Buffer Bytecode{};
		std::vector<ScriptFieldSchema> Fields{}; // canonical byte-wise name order
		std::vector<ScriptRequire> Requires{};
		ScriptSourceMap SourceMap{};
	};

	// Builds only from engine-compiled bytecode and validated load-time metadata. The complete ECKD Script artifact,
	// including the schema, resolved require list and source map. Pure; identical inputs produce identical bytes.
	// SourceMap v1 ends with a u32-length UTF-8 JsonPointer and u32 GeneratedPrefixLines after LineOffsets. Validate
	// RFC 6901 spelling, prefix 0/1, authored offsets/bounds and a nonempty stable chunk name. An empty Path is allowed
	// only with an explicit non-file chunk label; diagnostic origin never authorizes file reads or require resolution.
	// Errors: Validation for invalid schema, kind, path, bounds or bytecode envelope; UnsupportedVersion for an ABI mismatch.
	[[nodiscard]] Result<Buffer> CookScript(const ScriptData& script, uint32_t importerVersion);

	// Bounds-checked CPU decode; never executes bytecode. ReadCookedArtifact validates the outer header and hash first.
	// Errors: Parse/Validation for malformed data; UnsupportedVersion for payload/compiler ABI incompatibility.
	// Hash integrity is not bytecode authentication: only trusted engine cache/pak producers may supply bytecode to a VM.
	[[nodiscard]] Result<AssetRef<ScriptData>> LoadCookedScript(std::span<const std::byte> cooked);

	// Immutable schema snapshot, keyed by standalone script handle. Pins each ScriptData and owns all schema-only
	// FieldInfo, TypeInfo and EnumInfo objects it builds. Their addresses outlive every use while this source is retained.
	// Publish a new source on reload; never mutate a source while a serializer, inspector or play session borrows it.
	class ScriptFieldSchemaSource final : public IFieldSchemaSource
	{
	public:
		// Restricts construction to Create; CreateRef still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ScriptFieldSchemaSource;
		};
		explicit ScriptFieldSchemaSource(ConstructionKey key);
		~ScriptFieldSchemaSource();
		ScriptFieldSchemaSource(const ScriptFieldSchemaSource&) = delete;
		ScriptFieldSchemaSource& operator=(const ScriptFieldSchemaSource&) = delete;

		// Validates all descriptors before constructing reflected types (external data must never reach a registry assert).
		// Every Array TypeInfo has an ElementSchema owned by this snapshot, recursively, with the same type as Element.
		// Non-Behaviour assets remain known with zero fields. Errors: Validation for null/invalid entries or invalid schema.
		[[nodiscard]] static Result<Ref<const ScriptFieldSchemaSource>> Create(std::map<AssetHandle, AssetRef<ScriptData>> scripts);

		// NotFound for an unknown handle or field, with spelling suggestions. Pointers remain valid for this source's life.
		[[nodiscard]] Result<const FieldInfo*> FindField(UUID owner, std::string_view name) const override;
		// The same canonical name order used in ScriptData::Fields; empty for an unknown handle or a non-Behaviour.
		[[nodiscard]] std::vector<std::string> GetFieldNames(UUID owner) const override;

		// The pinned persistent descriptor, including nested element metadata and the default absent from FieldInfo.
		[[nodiscard]] Result<const ScriptFieldSchema*> FindSchema(UUID owner, std::string_view name) const;
	private:
		struct State;
		Scope<State> m_State{};
	};

}
