#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Sprite sheets (M18, C18.1, R-2D-SHEET; Docs/Decisions/0029-m18-contract.md decisions 14 to 18): the .spritesheet file
// (AssetType::SpriteSheet, format version 1), the reflected SpriteSheetData struct behind it, its cooked payload and the
// loaded asset. A sheet is data only: one texture, the frames cut from it (texel rectangles with a pivot and 9-slice
// borders) and named clips (frame lists with tick durations). Produced by SpriteSheetImporter (.spritesheet files) and
// asset.create; drawn by M21's sprites, tilemaps and SpriteAnimator (which plays clips), M22's UI images (frames with their
// 9-slice borders, through the QuadBatcher's ExpandNineSlice) and M26's particles.
//
// Conventions, which every consumer follows:
//   - A frame's identity is its index in Frames (clips, M21's sprites and tilemaps refer to frames by index); a frame's
//     Name is optional and only labels it. A clip's identity is its Name (scripts play clips by name).
//   - Texel space: x right, y down, origin at the top-left corner of the texture's first row, in whole texels. A frame's
//     Rect is [X, X + Width) x [Y, Y + Height) there; its Borders are its 9-slice insets in texels (all zero: not sliced).
//   - A frame's Pivot is the point of the frame that sits at its sprite's position, as a fraction of the frame's size from
//     its top-left corner in the same orientation: (0.5, 0.5) is its centre (the default), (0.5, 1) the middle of its
//     bottom edge. A pixel-art sprite keeps its texels on the pixel grid with a pivot that is a multiple of 1 / Width and
//     1 / Height.
//   - A clip frame shows Frame for Ticks fixed-step ticks (sprite animation is driven by ticks, never the wall clock); a
//     clip with Loop starts again after its last frame, one without holds its last frame.
//   - A null Texture is valid: its frames sample an opaque white texel (QuadDraw's null texture), so frames give sizes and
//     pivots to plain coloured quads.
// The texture's size is not part of the sheet: the importer never reads the texture, so a sheet imports and loads without
// it and a texture edit does not reimport the sheet. A frame that extends past its texture samples the texture's clamped
// edge texels (the QuadBatcher's samplers clamp to edge); asset.create and asset.setProperties refuse such a frame when the
// texture is registered and loads (EditorCore/Automation/AssetMethods.h).

namespace Engine {

	class TypeRegistry;

	// The .spritesheet document header (§6.9).
	inline constexpr std::string_view SpriteSheetFormatName = "SpriteSheet";
	inline constexpr uint32_t SpriteSheetFormatVersion = 1;

	// The limits ValidateSpriteSheet enforces, so a sheet external input controls cannot exhaust memory.
	inline constexpr uint32_t MaxSpriteSheetFrames = 65536; // a frame index fits in 16 bits (M21's tilemaps)
	inline constexpr uint32_t MaxSpriteSheetClips = 1024;
	inline constexpr uint32_t MaxSpriteClipFrames = 1024;  // per clip
	inline constexpr uint32_t MaxSpriteFrameTicks = 65535; // per clip frame: about 18 minutes at 60 Hz
	inline constexpr size_t MaxSpriteSheetNameBytes = 128; // a frame's or a clip's Name, in UTF-8 bytes

	// Registry struct "SpriteRect": a frame's texel rectangle. X + Width and Y + Height are at most MaxTextureDimension
	// (16384, the largest texture TextureImporter accepts); Width and Height are at least 1.
	struct SpriteRect
	{
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Width = 1;
		uint32_t Height = 1;

		bool operator==(const SpriteRect&) const = default;
	};

	// Registry struct "SpriteBorders": a frame's 9-slice insets in texels (NineSliceSource::Borders, Renderer/QuadBatcher.h);
	// Left + Right <= Rect.Width and Top + Bottom <= Rect.Height. All zero: the frame is not sliced.
	struct SpriteBorders
	{
		uint32_t Left = 0;
		uint32_t Top = 0;
		uint32_t Right = 0;
		uint32_t Bottom = 0;

		bool operator==(const SpriteBorders&) const = default;
	};

	// Registry struct "SpriteFrame": one frame of the sheet's texture.
	struct SpriteFrame
	{
		std::string Name{}; // optional; 0 to MaxSpriteSheetNameBytes bytes; unique among the frames when not empty
		SpriteRect Rect{};
		glm::vec2 Pivot{ 0.5f, 0.5f }; // each component finite, 0 to 1 (the file comment's convention)
		SpriteBorders Borders{};

		bool operator==(const SpriteFrame&) const = default;
	};

	// Registry struct "SpriteClipFrame": one step of a clip.
	struct SpriteClipFrame
	{
		uint32_t Frame = 0; // an index into the sheet's Frames
		uint32_t Ticks = 1; // 1 to MaxSpriteFrameTicks fixed-step ticks

		bool operator==(const SpriteClipFrame&) const = default;
	};

	// Registry struct "SpriteClip": a named animation over the sheet's frames.
	struct SpriteClip
	{
		std::string Name{};                    // 1 to MaxSpriteSheetNameBytes bytes; unique among the clips
		std::vector<SpriteClipFrame> Frames{}; // 1 to MaxSpriteClipFrames steps, played in order
		bool Loop = true;                      // start again after the last step; false: hold the last frame

		bool operator==(const SpriteClip&) const = default;
	};

	// A sprite sheet (AssetType::SpriteSheet), registry struct "SpriteSheet". Every field is serialized under its member
	// name, in member order, after "Format": "SpriteSheet" and "Version": 1. Plain data; immutable once loaded
	// (AssetRef<SpriteSheetData>). There is no placeholder sheet (GetPlaceholderHandle): a consumer whose sheet is missing
	// or failed reports it and draws nothing.
	struct SpriteSheetData : Asset
	{
		static constexpr AssetType StaticType = AssetType::SpriteSheet;
		// The cooked payload layout below (CookedHeader::FormatVersion).
		static constexpr uint16_t FormatVersion = 1;

		SpriteSheetData()
			: Asset(StaticType)
		{
		}

		TypedAssetHandle<AssetType::Texture> Texture{}; // null: frames sample opaque white
		std::vector<SpriteFrame> Frames{};              // at most MaxSpriteSheetFrames
		std::vector<SpriteClip> Clips{};                // at most MaxSpriteSheetClips, in authored order
	};

	// Registers the structs "SpriteRect", "SpriteBorders", "SpriteFrame", "SpriteClipFrame", "SpriteClip" and "SpriteSheet"
	// (SpriteSheetData) with the field ranges above and the rules across fields (ValidateSpriteSheet: borders inside the
	// rectangle, unique names, clip frames that exist) as Validate hooks, and Generate hooks that keep the registry suite's
	// random objects valid. RegisterAssetTypes calls it, so sheets read and write in the editor and in exported games alike.
	void RegisterSpriteSheetTypes(TypeRegistry& registry);

	// Checks every rule of the file comment and the field comments: at most MaxSpriteSheetFrames frames and
	// MaxSpriteSheetClips clips; each frame's Rect of at least 1 x 1 texels ending at most at MaxTextureDimension, its Pivot
	// finite and in [0, 1], its Borders inside its Rect, its Name valid UTF-8 without control characters of at most
	// MaxSpriteSheetNameBytes bytes, unique among the frames when not empty (byte-wise); each clip's Name likewise but not
	// empty and unique among the clips, 1 to MaxSpriteClipFrames steps, every step's Frame below the frame count and its Ticks
	// 1 to MaxSpriteFrameTicks. The texture's size is not checked (the file comment). Pure. Errors: Validation with one issue
	// per violation, each located by its JSON pointer into the .spritesheet document ("/Frames/3/Borders", "/Clips/0/Name",
	// "/Clips/1/Frames/2/Frame").
	[[nodiscard]] Status ValidateSpriteSheet(const SpriteSheetData& sheet);

	// The index of the frame named `name` (not empty), or nullopt. Linear in the frame count.
	[[nodiscard]] std::optional<uint32_t> FindSpriteFrame(const SpriteSheetData& sheet, std::string_view name);

	// The clip named `name`, or nullptr. Linear in the clip count.
	[[nodiscard]] const SpriteClip* FindSpriteClip(const SpriteSheetData& sheet, std::string_view name);

	// The assets `sheet` references, without nulls, sorted and unique: its texture. Its importer's Import.Dependencies
	// (ADR 0028's phase-1 ledger: every importer fills them, so M19's pruned export keeps the texture) and its edges in the
	// AssetDependencyGraph.
	[[nodiscard]] std::vector<AssetHandle> GetSpriteSheetDependencies(const SpriteSheetData& sheet);

	// What a sheet load reports besides its result: the file's version and its warnings (unknown members) in document order.
	struct SpriteSheetLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{};
	};

	// The canonical .spritesheet document ("Format", "Version", then the registry fields in member order, every field
	// written, §6). Errors: Validation for a value that cannot be written (a non-finite pivot), located. Requires a frozen
	// registry on which RegisterSpriteSheetTypes ran (asserted; InvalidState without asserts).
	[[nodiscard]] Result<Json> SpriteSheetToJson(const SpriteSheetData& sheet, const TypeRegistry& registry);

	// The canonical text of SpriteSheetToJson: Pretty for .spritesheet files.
	[[nodiscard]] Result<std::string> SpriteSheetToText(const SpriteSheetData& sheet, const TypeRegistry& registry,
		JsonStyle style = JsonStyle::Pretty);

	// Reads a .spritesheet document strictly through the registry (StructInfo::FromJson of "SpriteSheet": missing keys keep
	// their defaults, unknown keys warn into `report`, or fail with `strictUnknowns`, every value validated), then
	// ValidateSpriteSheet. Errors: Validation (located, every bad field an issue) for a wrong "Format", a version below 1, a
	// wrong JSON type, an out-of-range value or a ValidateSpriteSheet violation; UnsupportedVersion naming both versions for
	// a newer file.
	[[nodiscard]] Result<SpriteSheetData> SpriteSheetFromJson(const Json& document, const TypeRegistry& registry, SpriteSheetLoadReport& report,
		bool strictUnknowns = false);

	// JsonReader::Parse, then SpriteSheetFromJson. Errors: Parse (line and column), and as SpriteSheetFromJson.
	[[nodiscard]] Result<SpriteSheetData> SpriteSheetFromText(std::string_view text, const TypeRegistry& registry, SpriteSheetLoadReport& report,
		bool strictUnknowns = false);

	// The cooked sprite sheet payload (FormatVersion 1), little-endian, strings as uint32 byte count then UTF-8 bytes:
	//     uint64 Texture (the handle's value; 0 for none); uint32 FrameCount; uint32 ClipCount;
	//     FrameCount x { string Name; uint32 X; uint32 Y; uint32 Width; uint32 Height; f32 PivotX; f32 PivotY;
	//                    uint32 Left; uint32 Top; uint32 Right; uint32 Bottom; }
	//     ClipCount x { string Name; uint8 Loop (0 or 1); uint32 StepCount; StepCount x { uint32 Frame; uint32 Ticks; } }
	// Frames and clips in the sheet's order. Asserts ValidateSpriteSheet. Pure; identical sheets give identical bytes in
	// every configuration (no padding, no pointers, floats as their bits).
	[[nodiscard]] Buffer SerializeSpriteSheetPayload(const SpriteSheetData& sheet);

	// Reads a payload written by SerializeSpriteSheetPayload, checking every count against its limit and against the bytes
	// left before allocating, then ValidateSpriteSheet. Never asserts on data (fuzzed: "SpriteSheetData: 1,000 seeded
	// mutations of a sprite sheet file and of its cooked form never crash"). Errors: Parse for truncation, a count above its
	// limit, a Loop byte other than 0 or 1, or trailing bytes; Validation from ValidateSpriteSheet.
	[[nodiscard]] Result<SpriteSheetData> DeserializeSpriteSheetPayload(std::span<const std::byte> payload);

	// The complete cooked artifact (CookedHeader + payload, §6.8) of `sheet`. Asserts ValidateSpriteSheet.
	[[nodiscard]] Buffer CookSpriteSheet(const SpriteSheetData& sheet, uint32_t importerVersion);

	// The sheet of a cooked artifact (both asset managers' loader for AssetType::SpriteSheet, RegisterBuiltinLoaders).
	// Errors: as ReadCookedArtifact and DeserializeSpriteSheetPayload.
	[[nodiscard]] Result<AssetRef<SpriteSheetData>> LoadCookedSpriteSheet(std::span<const std::byte> cooked);

}
