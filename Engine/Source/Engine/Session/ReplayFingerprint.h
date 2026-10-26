#pragma once

#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <string>
#include <vector>

// The replay's start fingerprint (Asset/ReplayData.h ReplayFingerprint; Docs/Decisions/0030-m19-contract.md decision 2,
// A-FPRINT): computed by both recorders at tick 0 (ReplayRecorder::Begin) and by ReplayPlayer::Begin for the playback, and
// compared when a playback's state hash differs from the recording's, so the failure names what changed since the
// recording. Session owns it because it reads a scene and its assets, which Asset may not include (§3).

namespace Engine {

	class AssetManager;
	class Scene;
	class ScriptFieldSchemaSource;

	// At most this many changes are named (ReplayPlaybackResult::Changes); the last line then says how many more differ.
	inline constexpr size_t MaxReplayFingerprintChanges = 16;

	// The fingerprint of `scene` as it is now: every entity in id order with its readable path, the hash of its own members
	// and of each serialized component, all from the scene's minified canonical serialization (SceneSerializer::SaveToString,
	// the document PlaySession::ComputeStateHash covers, so the two agree on what a component holds), and every asset an
	// AssetRef value of those components names, once, in handle order, with its type, readable path and
	// AssetManager::GetCookedHash (procedural built-ins and handles that name no asset left out, as ReplayFingerprint
	// says). A Script component's Fields are untyped values, where an asset field and an entity field are both 16-hex
	// strings, so their asset values are read with `schemas`, the session's script field schemas
	// (PlaySession::GetScriptSchemas): a field the script's schema types AssetRef, or an Array of AssetRef, names assets;
	// any other field, a field the schema does not know and every field when `schemas` is null (a host without scripts)
	// names none. `assets` null (a host without assets) lists no asset, nor does a manager without cooked artifacts
	// (GetCookedHash's default, Unsupported). Deterministic: equal scenes, schemas and cooked assets give equal
	// fingerprints in every configuration and host (the editor, every exported Runtime). Errors: the serializer's;
	// GetCookedHash's for a referenced asset that exists but cannot be hashed (a failed cook or read), with the asset's
	// path as context.
	[[nodiscard]] Result<ReplayFingerprint> ComputeReplayFingerprint(const Scene& scene, const ScriptFieldSchemaSource* schemas, AssetManager* assets);

	// What differs between `recorded` (the replay's) and `observed` (the playback's), one readable line per difference, in
	// four groups in this order:
	//   1. entities only one of them holds, by id: "entity <path> (<id>) was added" or "... was removed";
	//   2. entities both hold whose own members differ (a name, parent, active flag or tags), by id: "entity <path> (<id>)
	//      changed";
	//   3. components of entities both hold, by entity id and then in registry order: "component <Component> of <path>
	//      (<id>) changed", "... was added" or "... was removed";
	//   4. assets, by handle: "<Type> <path> (<handle>) changed", "... is now referenced" or "... is no longer referenced"
	//      ("Script Assets/Scripts/Board.luau (c0ffee0000000001) changed").
	// Ids and handles are 16 lowercase hex digits; a path is the recorded one where both hold the item, else the one that
	// holds it. At most `limit` lines (at least 1): when more differ, the last line is "... and <n> more" in place of the
	// limit-th difference, n counting every difference not listed. Empty when the two are equal.
	[[nodiscard]] std::vector<std::string> DescribeReplayFingerprintChanges(const ReplayFingerprint& recorded, const ReplayFingerprint& observed,
		size_t limit = MaxReplayFingerprintChanges);

}
