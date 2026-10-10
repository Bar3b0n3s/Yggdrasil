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
	inline constexpr uint32_t ReplayFormatVersion = 1;

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
		std::string Config{}; // Informational; never a reason to select a different expected hash.
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

	// The JSON document is flattened: Format, Version, Header's members, Events, Expect, FinalTick, FinalStateHash.
	// Event Tick is the zero-based step where input is applied. Expect Tick and FinalTick count completed ticks:
	// 0 is setup; N is the boundary after ticks [0, N), including the frame phase and pending Scene.Load.
	struct ReplayDocument
	{
		uint32_t Version = ReplayFormatVersion;
		ReplayHeader Header{};
		std::vector<ReplayEvent> Events{};
		std::vector<ReplayExpectation> Expect{};
		uint64_t FinalTick = 0;
		std::string FinalStateHash{}; // Exactly 16 lowercase hex digits.
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
		static constexpr uint16_t FormatVersion = 1;

		ReplayData();

		ReplayHeader Header{};
		std::vector<ReplayEvent> Events{};
		std::vector<ReplayBytecodeExpectation> Expect{};
		uint64_t FinalTick = 0;
		std::string FinalStateHash{};
	};

	struct ReplayLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{};
	};

	// Pure structural validation: valid scene handle, object Parameters with finite JSON values, FixedHz in the
	// FrameLoopConfig range, header strings, known event shapes, nondecreasing ticks (ties preserve array order),
	// Events.Tick < FinalTick, Expect.Tick <= FinalTick and nonempty Luau, canonical hash. A trailing Tap release at
	// FinalTick is outside the run and is not recorded. Newer Version is UnsupportedVersion. No compiler runs here.
	[[nodiscard]] Status ValidateReplayDocument(const ReplayDocument& document);
	[[nodiscard]] Result<ReplayDocument> ReplayFromJson(const Json& document, ReplayLoadReport& report, bool strictUnknowns = false);
	[[nodiscard]] Result<ReplayDocument> ReplayFromText(std::string_view text, ReplayLoadReport& report, bool strictUnknowns = false);
	[[nodiscard]] Result<Json> ReplayToJson(const ReplayDocument& document);
	[[nodiscard]] Result<std::string> ReplayToText(const ReplayDocument& document);

	// Cooked FormatVersion 1 uses the standard ECKD header. Payload, little-endian: u32 source version; scene UUID
	// u64; length-prefixed UTF-8 scene path and canonical Parameters JSON; seed u64; FixedHz u32; length-prefixed
	// EngineVersion and Config; event count u32; events in array order; expectation count u32; expectations;
	// FinalTick u64; state hash u64. A string/buffer length is u32. Event records: Tick u64, kind u8 (the nine kinds
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
