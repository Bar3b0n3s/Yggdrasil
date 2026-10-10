#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	struct ReplayData;

	// Strict source Replay document -> cooked ReplayData. Asset owns the wire vocabulary and codec; this importer
	// never includes Session or its input-event types. The scene is resolved by the header's handle, not its readable
	// path, and is an asset dependency. No scene is instantiated, and no expectation is evaluated while importing.
	class ReplayImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Replay";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Replay; }
		// ".replay".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }
		[[nodiscard]] bool RequiresMainThread() const override { return false; }

		// Reads SourceBytes through ReplayFromText with strictUnknowns=true, validates event/expectation tick bounds,
		// finite values and the replay header, then compiles each Expect with ScriptCompiler's ExpressionOrChunk mode.
		// A complete expression gets the documented wrapper; a complete chunk (including return) stays unchanged.
		// ChunkName is "=replay/<16-digit-handle>/Expect/<index>"; SourceMap.Path is the actual .replay and JsonPointer
		// is "/Expect/<index>/Luau". Authored offsets and GeneratedPrefixLines persist in the nested Script artifact.
		// No fake .luau file or require base is created. Locations refer to the decoded authored Luau string, not JSON
		// document lines. Ties preserve authored array order; their indices and diagnostic origins remain distinct.
		// Uses the import's immutable lookup snapshot for scene-handle resolution (including a stale readable path).
		// Produces one artifact with metadata.Handle through CookReplay; no settings or dependency-file ownership.
		// Errors: Parse/Validation/UnsupportedVersion from strict load, NotFound for an unknown scene handle,
		// Validation for a non-Scene handle, located CompileFailed for an Expect, and codec errors. Atomic on failure.
		// Independent calls are thread-safe after scripting initialization; no VFS writes, ECS or live VM access.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;
	};

}
