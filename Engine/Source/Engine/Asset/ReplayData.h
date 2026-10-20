#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	inline constexpr std::string_view ReplayFormatName = "Replay";
	// Version 2 adds Checkpoints. A version 1 file migrates to version 2 with no checkpoints: only its FinalStateHash is
	// compared, as before, and project.upgrade rewrites it canonically.
	inline constexpr uint32_t ReplayFormatVersion = 2;
	// The first version with the start fingerprint (Fingerprint, below; Docs/Decisions/0030-m19-contract.md decision 2,
	// A-FPRINT), reserved for M19 by the phase 1 ledger: ReplayFormatVersion, ReplayData::FormatVersion and
	// ReplayImporter::Version all become 3 with the readers, the writer and the cooked codec that carry it (stream B). A
	// version 1 or 2 file migrates to version 3 with a null Fingerprint (nothing recorded, so nothing can be named), which
	// project.upgrade writes (Tests/Data/Formats/Replay/v2.upgraded.replay); every recording writes a fingerprint.
	inline constexpr uint32_t ReplayFingerprintFormatVersion = 3;
	// The EngineVersion every recorder writes into a replay header (input.record, test.run's record, the Runtime's
	// recordings): the constant "0.1.0", never EngineVersionString or the build id (Core/BuildId.h), so neither a version
	// bump nor a commit rewrites a replay (decision 13, B-BVER). Readers accept any header text, as before.
	inline constexpr std::string_view ReplayEngineVersionString = "0.1.0";
	// The recorders (input.record, test.run's record) store the state hash of every completed-tick boundary that is a
	// positive multiple of this interval and lies before FinalTick (one second at 60 Hz; a tick inside a frame where a
	// suite's scripted clock grouped several steps is no completed boundary, ReplayRecorder::CaptureCheckpoint), so a replay proves the
	// trajectory of its run and not only the state it ends in: an autopilot that ends on a restart returns the world to
	// its idle state, which an input-free run reaches too. A reader accepts checkpoints at any ticks (strictly increasing,
	// after tick 0, before FinalTick), so the interval is the writers' choice, never a format rule.
	inline constexpr uint64_t ReplayCheckpointInterval = 60;

	// Architecture §6.7: the handle is authoritative; Path is readable and may be stale after a move.
	struct ReplaySceneReference
	{
		AssetHandle Handle{};
		std::string Path{};
	};

	struct ReplayHeader
	{
		ReplaySceneReference Scene{};
		VariantValue Parameters{}; // Null storage normalizes to an empty object; files always write an object.
		uint64_t Seed = 0;
		uint32_t FixedHz = 60;
		std::string EngineVersion{};
		// The run mode that recorded the replay ("Editor", "Release" or "Dist"), never the build configuration, so a replay's
		// bytes do not depend on it (§9.1). Informational; never a reason to select a different expected hash.
		std::string Config{};
	};

	// Asset-owned wire vocabulary, not Session's PlayInputEvent (§3). Type/State/control names use canonical file
	// spellings. Only the fields belonging to Type are serialized; readers reject inappropriate present members.
	// Type: Action, Key, MouseButton, MouseMove, MouseDelta, Scroll, GamepadButton, GamepadAxis or Text.
	// State: Down, Up or Tap; Action selects State OR Value. The source reader checks device names/ranges through
	// Platform, while Session checks project action names when adapting this value to PlayInputEvent.
	struct ReplayEvent
	{
		uint64_t Tick = 0;
		std::string Type{};
		std::string Name{};
		std::optional<std::string> State{};
		std::optional<float> Value{};
		std::string KeyName{};    // File key Key.
		std::string ButtonName{}; // File key Button.
		uint32_t Gamepad = 0;
		std::string AxisName{}; // File key Axis.
		std::optional<glm::vec2> Position{};
		std::optional<glm::vec2> Delta{};
		std::string Text{};
	};

	struct ReplayExpectation
	{
		uint64_t Tick = 0;
		std::string Luau{};
	};

	// The state hash the recording observed at the completed-tick boundary Tick (0 < Tick < FinalTick).
	struct ReplayCheckpoint
	{
		uint64_t Tick = 0;
		std::string StateHash{}; // Exactly 16 lowercase hex digits.
	};

	// The start fingerprint (version 3; Docs/Decisions/0030-m19-contract.md decision 2, A-FPRINT): what the recorded
	// session held at tick 0 after its setup, the moment ReplayRecorder::Begin binds the recording, per entity and component
	// and per asset its components reference, so a playback whose state hash differs at a checkpoint or at FinalTick names
	// what changed since the recording (ReplayPlaybackResult::Changes, Session/ReplayFingerprint.h) instead of only listing
	// the possible causes (M17 friction F13). Computed by ComputeReplayFingerprint (Session/ReplayFingerprint.h). Every hash
	// is XXH64 (seed 0) as 16 lowercase hex digits; paths are readable and never compared.
	struct ReplayComponentFingerprint
	{
		std::string Component{}; // the component's registry name ("PostProcess"); file: the key of the entity's Components
		// Of the component's value as the scene's minified canonical serialization holds it (the entity's "Components" member,
		// SceneSerializer::SaveToString, the document the state hash covers); file: that key's value.
		std::string Hash{};

		[[nodiscard]] bool operator==(const ReplayComponentFingerprint& other) const = default;
	};

	struct ReplayEntityFingerprint
	{
		UUID Entity{};      // file key Id
		std::string Path{}; // Scene::GetEntityPath at tick 0, readable only
		// Of the entity's own members as that serialization writes them (its id, name, parent, active flag, tags), without
		// its Components.
		std::string Hash{};
		std::vector<ReplayComponentFingerprint> Components{}; // in registry order, the order scene files use

		[[nodiscard]] bool operator==(const ReplayEntityFingerprint& other) const = default;
	};

	struct ReplayAssetFingerprint
	{
		AssetHandle Handle{};
		std::string Path{}; // AssetManager::GetReferencePath when it was recorded, readable only
		std::string Type{}; // its AssetType name ("Script", "Material")
		std::string Hash{}; // of its cooked artifact (AssetManager::GetCookedHash), equal in the editor's cache and a pak

		[[nodiscard]] bool operator==(const ReplayAssetFingerprint& other) const = default;
	};

	// File form, the "Fingerprint" member (after Config): {"Entities": [{"Id", "Path", "Hash", "Components": {<name>: <hash>,
	// ...}}, ...], "Assets": [{"Handle", "Path", "Type", "Hash"}, ...]}, or null for a replay migrated from version 1 or 2.
	struct ReplayFingerprint
	{
		std::vector<ReplayEntityFingerprint> Entities{}; // every entity of the scene at tick 0, sorted by id
		// Every asset an AssetRef value of those components names (a Script component's script and its asset fields, typed
		// by the session's script field schemas, a MeshRenderer's mesh and materials, ...), once, sorted by handle; cooked
		// built-ins included, while a procedural built-in (no artifact) and a handle that names no asset are left out. Only
		// direct references: a module that only a script requires, or a material's texture, is not listed.
		std::vector<ReplayAssetFingerprint> Assets{};

		[[nodiscard]] bool operator==(const ReplayFingerprint& other) const = default;
	};

	// The JSON document is flattened: Format, Version, Header's members, Fingerprint (version 3), Events, Expect,
	// Checkpoints, FinalTick, FinalStateHash. Event Tick is the zero-based step where input is applied. Expect, Checkpoint
	// and FinalTick ticks count completed ticks: 0 is setup; N is the boundary after ticks [0, N), including the frame phase
	// and pending Scene.Load.
	//
	// The state hash (PlaySession::ComputeStateHash) covers the simulated world: the scene's minified canonical
	// serialization (every entity with its id, name, parent, active flag, tags and serialized components, a Script
	// component's authored Fields included), the tick, the session's Random state, the UUIDGenerator's draw count and every body's
	// physics state (position, rotation, linear and angular velocity, sleep flag). It does not cover Luau instance state
	// (script tables such as self.State and module locals), audio voices, input state, the camera view or anything
	// rendered: scripts are observed through their effects on the world and through Expect. Two runs with equal hashes at
	// a tick have equal worlds there; they may still differ in script state that has not yet reached the world.
	struct ReplayDocument
	{
		uint32_t Version = ReplayFormatVersion;
		ReplayHeader Header{};
		std::vector<ReplayEvent> Events{};
		std::vector<ReplayExpectation> Expect{};
		std::vector<ReplayCheckpoint> Checkpoints{}; // Strictly increasing ticks; the final tick's hash is FinalStateHash.
		uint64_t FinalTick = 0;
		std::string FinalStateHash{}; // Exactly 16 lowercase hex digits.
		// Version 3 (ReplayFingerprintFormatVersion, decision 2): the start fingerprint, written after Config; nullopt (null in
		// the file) for a replay migrated from version 1 or 2. Validation: entities strictly increasing by id with valid ids,
		// component names unique per entity and in registry order, assets strictly increasing by valid handle, every hash
		// canonical.
		std::optional<ReplayFingerprint> Fingerprint{};
	};

	// Engine-compiled, trusted bytecode from ReplayImporter; no Luau type or compiler dependency in Asset/Session.
	// Script is a Module chunk with its source map and compiler ABI envelope, shared with ScriptCompiler/CookScript.
	struct ReplayBytecodeExpectation
	{
		uint64_t Tick = 0;
		ScriptData Script{};
	};

	struct ReplayData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Replay;
		static constexpr uint16_t FormatVersion = 2;

		ReplayData();

		ReplayHeader Header{};
		std::vector<ReplayEvent> Events{};
		std::vector<ReplayBytecodeExpectation> Expect{};
		std::vector<ReplayCheckpoint> Checkpoints{};
		uint64_t FinalTick = 0;
		std::string FinalStateHash{};
		// Cooked FormatVersion 3 (decision 2): the source's Fingerprint, which ReplayPlayer compares with the playback's own
		// (ComputeReplayFingerprint) when a hash differs, in the editor and in every exported configuration.
		std::optional<ReplayFingerprint> Fingerprint{};
	};

	struct ReplayLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{};
	};

	// Pure structural validation: valid scene handle, object Parameters with finite JSON values, FixedHz in the
	// FrameLoopConfig range, header strings, known event shapes, nondecreasing ticks (ties preserve array order),
	// Events.Tick < FinalTick, Expect.Tick <= FinalTick and nonempty Luau, strictly increasing Checkpoints.Tick with
	// 0 < Tick < FinalTick, canonical hashes. A trailing Tap release at FinalTick is outside the run and is not recorded.
	// Version must be ReplayFormatVersion (readers migrate older files first); newer is UnsupportedVersion. No compiler
	// runs here.
	[[nodiscard]] Status ValidateReplayDocument(const ReplayDocument& document);
	// The readers accept versions 1 and 2. Version 1 has no Checkpoints member (one is an unknown member there) and
	// migrates to version 2 with none; the document returned is always ReplayFormatVersion, and report.FileVersion keeps
	// the file's own version. Version 2 requires Checkpoints. From version 3 (stream B, decision 2) they also accept version
	// 3, which requires Fingerprint (an object or null); in versions 1 and 2 a Fingerprint member is unknown, and both
	// migrate with a null one.
	[[nodiscard]] Result<ReplayDocument> ReplayFromJson(const Json& document, ReplayLoadReport& report, bool strictUnknowns = false);
	[[nodiscard]] Result<ReplayDocument> ReplayFromText(std::string_view text, ReplayLoadReport& report, bool strictUnknowns = false);
	[[nodiscard]] Result<Json> ReplayToJson(const ReplayDocument& document);
	[[nodiscard]] Result<std::string> ReplayToText(const ReplayDocument& document);
	// Header Parameters as a replay stores them (§6.7): the object a .replay file and a cooked replay read back, every
	// float rounded to float like every authored file (JsonWriter, §6), integers exact. A recording (input.record start,
	// test.run record) starts its session from this form, so the recorded run and every replay of it see the same
	// Scene.GetLoadParameters values and the strict hash can match. Errors: Validation located below `pointer` for a
	// non-object, a float outside the float range, invalid UTF-8 or nesting deeper than MaxJsonDepth.
	[[nodiscard]] Result<Json> CanonicalizeReplayParameters(const Json& parameters, std::string_view pointer = "/Parameters");

	// Cooked FormatVersion 2 uses the standard ECKD header. Payload, little-endian: u32 source version; scene UUID
	// u64; length-prefixed UTF-8 scene path and canonical Parameters JSON; seed u64; FixedHz u32; length-prefixed
	// EngineVersion and Config; event count u32; events in array order; expectation count u32; expectations;
	// checkpoint count u32; checkpoints (Tick u64, state hash u64); FinalTick u64; state hash u64. FormatVersion 3 (stream
	// B, decision 2) appends the fingerprint: u8 present (0 or 1); when present, entity count u32 and per entity its id u64,
	// length-prefixed path, hash u64, component count u32 and per component its length-prefixed name and hash u64; then
	// asset count u32 and per asset its handle u64, length-prefixed path and type name, and hash u64. Version 1 artifacts
	// (no checkpoints) are UnsupportedVersion: the cache rebuilds them (ReplayImporter's version is part of the cache
	// key) and exports are made again. A string/buffer length is u32. Event records: Tick u64, kind u8 (the nine kinds
	// above in that order), then type-specific fields: strings, state u8 (Down=0, Up=1, Tap=2), f32 value or vec2;
	// Action has a u8 State/Value discriminant; MouseButton has a u8 HasPosition; gamepad index is u32. Expectations:
	// Tick u64, length-prefixed complete CookScript artifact. The nested artifact preserves source maps and compiler
	// ABI checks without a second bytecode envelope. Check counts, tags, lengths, finite values and trailing bytes.
	// Bytecode count/ticks must match source.Expect exactly; errors are located Results. The importer records scene
	// dependencies and compiles every expectation before calling CookReplay. The loader verifies ECKD hash/type/version.
	[[nodiscard]] Result<Buffer> CookReplay(const ReplayDocument& source, std::span<const ReplayBytecodeExpectation> expectations,
		uint32_t importerVersion);
	[[nodiscard]] Result<AssetRef<ReplayData>> LoadCookedReplay(std::span<const std::byte> cooked);

}
