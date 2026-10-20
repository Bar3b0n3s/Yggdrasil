#include "EnginePCH.h"
#include "Engine/Asset/SpriteSheetData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <utility>

namespace Engine {

	namespace Utils {

		// The oldest readable .spritesheet version (there has been no other yet).
		static constexpr uint32_t MinimumSpriteSheetVersion = 1;
		// The fewest payload bytes a frame takes (an empty Name: its uint32 length, then ten uint32 and f32 fields) and a clip
		// takes (an empty Name, the Loop byte and the step count); a step is two uint32. DeserializeSpriteSheetPayload checks
		// every count against these before it allocates.
		static constexpr uint64_t MinFramePayloadBytes = 4 + 10 * 4;
		static constexpr uint64_t MinClipPayloadBytes = 4 + 1 + 4;
		static constexpr uint64_t StepPayloadBytes = 2 * 4;

		// An Error at <context's pointer><relative> (`relative` empty or starting with '/').
		static void AddSheetIssue(ValidationContext& context, std::string_view relative, std::string message, std::string hint = {})
		{
			ValidationIssue issue;
			issue.Severity = DiagnosticSeverity::Error;
			issue.JsonPointer = context.GetPointer() + std::string(relative);
			issue.Message = std::move(message);
			issue.Hint = std::move(hint);
			context.AddIssue(std::move(issue));
		}

		// Whether `text` holds a C0 control character or DEL (the rule of every sheet name; VfsPath's too).
		[[nodiscard]] static bool HasControlCharacter(std::string_view text)
		{
			return std::ranges::any_of(text, [](char character)
			{
				const auto value = static_cast<unsigned char>(character);
				return value < 0x20 || value == 0x7f;
			});
		}

		// A frame's or a clip's Name (`what`: "frame 3", "clip 0"): at most MaxSpriteSheetNameBytes bytes of valid UTF-8
		// without control characters; not empty when `required` (a clip's).
		static void CheckSheetName(std::string_view name, bool required, std::string_view relative, std::string_view what, ValidationContext& context)
		{
			if (name.empty())
			{
				if (required)
					AddSheetIssue(context, relative, std::format("{} has no Name", what), "a clip is played by its name: give it one, unique among the clips");
				return;
			}
			if (name.size() > MaxSpriteSheetNameBytes)
			{
				AddSheetIssue(context, relative, std::format("{}'s Name is {} bytes long, more than the {} a name may have", what, name.size(), MaxSpriteSheetNameBytes));
				return;
			}
			if (!IsValidUtf8(name))
				AddSheetIssue(context, relative, std::format("{}'s Name is not valid UTF-8", what));
			else if (HasControlCharacter(name))
				AddSheetIssue(context, relative, std::format("{}'s Name holds a control character", what), "names are printable text: remove tabs, line breaks and other control characters");
		}

		// One frame's own rules, located below `base` ("" in the "SpriteFrame" hook, "/Frames/<i>" for a whole sheet): its
		// Name, a Rect of at least 1 x 1 texels ending at most at MaxTextureDimension, a finite Pivot in [0, 1] and Borders
		// inside the Rect. Its Name's uniqueness is the sheet's rule (ValidateSheet).
		static void ValidateFrame(const SpriteFrame& frame, std::string_view base, std::string_view what, ValidationContext& context)
		{
			const std::string prefix(base);
			CheckSheetName(frame.Name, false, prefix + "/Name", what, context);

			const SpriteRect& rect = frame.Rect;
			if (rect.Width == 0)
				AddSheetIssue(context, prefix + "/Rect/Width", std::format("{} is 0 texels wide", what), "a frame is at least 1 x 1 texels");
			if (rect.Height == 0)
				AddSheetIssue(context, prefix + "/Rect/Height", std::format("{} is 0 texels high", what), "a frame is at least 1 x 1 texels");
			const uint64_t right = static_cast<uint64_t>(rect.X) + rect.Width;
			const uint64_t bottom = static_cast<uint64_t>(rect.Y) + rect.Height;
			if (right > MaxTextureDimension || bottom > MaxTextureDimension)
			{
				AddSheetIssue(context, prefix + "/Rect",
					std::format("{} ends at texel ({}, {}), outside the largest texture ({} x {})", what, right, bottom, MaxTextureDimension, MaxTextureDimension),
					"X + Width and Y + Height are at most the texture's size");
			}

			const bool finite = std::isfinite(frame.Pivot.x) && std::isfinite(frame.Pivot.y);
			if (!finite || frame.Pivot.x < 0.0f || frame.Pivot.x > 1.0f || frame.Pivot.y < 0.0f || frame.Pivot.y > 1.0f)
			{
				AddSheetIssue(context, prefix + "/Pivot", std::format("{}'s Pivot ({}, {}) is not a point of the frame", what, frame.Pivot.x, frame.Pivot.y),
					"a Pivot is a fraction of the frame's size from its top-left corner: each component from 0 to 1, (0.5, 0.5) its centre");
			}

			const SpriteBorders& borders = frame.Borders;
			const uint64_t horizontal = static_cast<uint64_t>(borders.Left) + borders.Right;
			const uint64_t vertical = static_cast<uint64_t>(borders.Top) + borders.Bottom;
			if (horizontal > rect.Width || vertical > rect.Height)
			{
				AddSheetIssue(context, prefix + "/Borders",
					std::format("{}'s Borders (Left {}, Top {}, Right {}, Bottom {}) do not fit its {} x {} Rect", what, borders.Left, borders.Top, borders.Right,
						borders.Bottom, rect.Width, rect.Height),
					"Left + Right must be at most Width and Top + Bottom at most Height");
			}
		}

		// One clip's rules below `base` ("" in the "SpriteClip" hook, "/Clips/<i>" for a whole sheet): a valid Name, at most
		// MaxSpriteClipFrames steps and each step's Ticks 1 to MaxSpriteFrameTicks; with `frameCount` (a whole sheet) also a
		// Name that is not empty, at least one step and each step's Frame an index of the sheet's frames. The hook leaves those
		// to the sheet, so the registry's default clip (no name, no step) reads and validates on its own, as a sheet holding
		// it does not. Its Name's uniqueness is the sheet's rule.
		static void ValidateClip(const SpriteClip& clip, std::string_view base, std::string_view what, std::optional<size_t> frameCount,
			ValidationContext& context)
		{
			const std::string prefix(base);
			const bool wholeSheet = frameCount.has_value();
			CheckSheetName(clip.Name, wholeSheet, prefix + "/Name", what, context);
			if (clip.Frames.empty())
			{
				if (wholeSheet)
					AddSheetIssue(context, prefix + "/Frames", std::format("{} has no frame", what), "a clip shows at least one frame");
			}
			else if (clip.Frames.size() > MaxSpriteClipFrames)
				AddSheetIssue(context, prefix + "/Frames", std::format("{} has {} steps, more than the {} a clip may have", what, clip.Frames.size(), MaxSpriteClipFrames));

			const size_t steps = std::min<size_t>(clip.Frames.size(), MaxSpriteClipFrames);
			for (size_t index = 0; index < steps; ++index)
			{
				const SpriteClipFrame& step = clip.Frames[index];
				const std::string stepPointer = std::format("{}/Frames/{}", prefix, index);
				if (frameCount.has_value() && step.Frame >= *frameCount)
				{
					AddSheetIssue(context, stepPointer + "/Frame", std::format("step {} of {} shows frame {}, but the sheet has {} frames", index, what, step.Frame, *frameCount),
						"a step's Frame is an index into the sheet's Frames, from 0");
				}
				if (step.Ticks == 0 || step.Ticks > MaxSpriteFrameTicks)
				{
					AddSheetIssue(context, stepPointer + "/Ticks",
						std::format("step {} of {} lasts {} ticks; a step lasts 1 to {} fixed-step ticks", index, what, step.Ticks, MaxSpriteFrameTicks));
				}
			}
		}

		// Every rule of ValidateSpriteSheet, located below the context's pointer: the counts, each frame's and each clip's
		// rules (up to the count limits) and the unique names (a duplicate is reported at its later occurrence).
		static void ValidateSheet(const SpriteSheetData& sheet, ValidationContext& context)
		{
			if (sheet.Frames.size() > MaxSpriteSheetFrames)
				AddSheetIssue(context, "/Frames", std::format("the sheet has {} frames, more than the {} a sheet may have", sheet.Frames.size(), MaxSpriteSheetFrames));
			std::set<std::string_view> frameNames;
			const size_t frames = std::min<size_t>(sheet.Frames.size(), MaxSpriteSheetFrames);
			for (size_t index = 0; index < frames; ++index)
			{
				const SpriteFrame& frame = sheet.Frames[index];
				const std::string base = std::format("/Frames/{}", index);
				const std::string what = std::format("frame {}", index);
				ValidateFrame(frame, base, what, context);
				if (!frame.Name.empty() && !frameNames.insert(frame.Name).second)
				{
					AddSheetIssue(context, base + "/Name", std::format("{}'s Name '{}' is the name of an earlier frame", what, frame.Name),
						"frame names are unique: rename the frame, or leave it unnamed (\"\")");
				}
			}

			if (sheet.Clips.size() > MaxSpriteSheetClips)
				AddSheetIssue(context, "/Clips", std::format("the sheet has {} clips, more than the {} a sheet may have", sheet.Clips.size(), MaxSpriteSheetClips));
			std::set<std::string_view> clipNames;
			const size_t clips = std::min<size_t>(sheet.Clips.size(), MaxSpriteSheetClips);
			for (size_t index = 0; index < clips; ++index)
			{
				const SpriteClip& clip = sheet.Clips[index];
				const std::string base = std::format("/Clips/{}", index);
				const std::string what = std::format("clip {}", index);
				ValidateClip(clip, base, what, sheet.Frames.size(), context);
				if (!clip.Name.empty() && !clipNames.insert(clip.Name).second)
					AddSheetIssue(context, base + "/Name", std::format("{}'s Name '{}' is the name of an earlier clip", what, clip.Name), "clip names are unique: rename the clip");
			}
		}

		// The "SpriteFrame" Validate hook: one frame's own rules, so a bad frame is reported where it is read.
		static void ValidateSpriteFrameHook(const SpriteFrame& frame, ValidationContext& context)
		{
			ValidateFrame(frame, {}, "the frame", context);
		}

		// The "SpriteClip" Validate hook: one clip's own rules (a name and a step are required, and its frame indices checked,
		// by the sheet's).
		static void ValidateSpriteClipHook(const SpriteClip& clip, ValidationContext& context)
		{
			ValidateClip(clip, {}, "the clip", std::nullopt, context);
		}

		// The "SpriteSheet" Validate hook: ValidateSpriteSheet on the whole sheet. The registry runs it only when the frames and
		// clips read without an issue, so it adds the rules across them (counts, unique names, the clips' frame indices).
		static void ValidateSpriteSheetHook(const SpriteSheetData& sheet, ValidationContext& context)
		{
			ValidateSheet(sheet, context);
		}

		// The "SpriteFrame" Generate hook: a random Rect is cut to end inside the largest texture (its X and Y are below it by
		// their ranges, so at least one texel stays), and random Borders are cut to fit it. Random names are printable ASCII.
		static void GenerateSpriteFrame(SpriteFrame& frame, Random& /*random*/)
		{
			SpriteRect& rect = frame.Rect;
			rect.Width = std::min(rect.Width, MaxTextureDimension - std::min(rect.X, MaxTextureDimension - 1));
			rect.Height = std::min(rect.Height, MaxTextureDimension - std::min(rect.Y, MaxTextureDimension - 1));
			SpriteBorders& borders = frame.Borders;
			borders.Left = std::min(borders.Left, rect.Width);
			borders.Right = std::min(borders.Right, rect.Width - borders.Left);
			borders.Top = std::min(borders.Top, rect.Height);
			borders.Bottom = std::min(borders.Bottom, rect.Height - borders.Top);
		}

		// The "SpriteClip" Generate hook: a clip gets a name and at least one step, which a sheet requires of it.
		static void GenerateSpriteClip(SpriteClip& clip, Random& /*random*/)
		{
			if (clip.Name.empty())
				clip.Name = "Clip";
			if (clip.Frames.empty())
				clip.Frames.push_back(SpriteClipFrame{});
		}

		// `name` made unique among `taken` by appending '#' until it is (random names are at most a dozen bytes, so the result
		// stays far below MaxSpriteSheetNameBytes).
		[[nodiscard]] static std::string MakeUniqueName(std::string name, const std::set<std::string>& taken)
		{
			while (taken.contains(name))
				name += '#';
			return name;
		}

		// The "SpriteSheet" Generate hook: unique frame and clip names, a frame for the clips to show, and every step's Frame an
		// index of the frames.
		static void GenerateSpriteSheet(SpriteSheetData& sheet, Random& /*random*/)
		{
			std::set<std::string> frameNames;
			for (SpriteFrame& frame : sheet.Frames)
			{
				if (frame.Name.empty())
					continue;
				frame.Name = MakeUniqueName(std::move(frame.Name), frameNames);
				frameNames.insert(frame.Name);
			}
			std::set<std::string> clipNames;
			for (SpriteClip& clip : sheet.Clips)
			{
				clip.Name = MakeUniqueName(std::move(clip.Name), clipNames);
				clipNames.insert(clip.Name);
			}
			if (!sheet.Clips.empty() && sheet.Frames.empty())
				sheet.Frames.push_back(SpriteFrame{});
			for (SpriteClip& clip : sheet.Clips)
				for (SpriteClipFrame& step : clip.Frames)
					step.Frame %= static_cast<uint32_t>(sheet.Frames.size());
		}

		// The registered "SpriteSheet" struct. A registry without it is a programmer error (asserted); the error return keeps
		// builds without asserts safe.
		[[nodiscard]] static Result<const StructInfo*> FindSpriteSheetType(const TypeRegistry& registry)
		{
			ENGINE_CORE_ASSERT(registry.IsFrozen(), "Sprite sheets need a frozen type registry");
			const StructInfo* type = registry.FindStruct<SpriteSheetData>();
			ENGINE_CORE_ASSERT(type != nullptr, "Sprite sheets need a registry on which RegisterSpriteSheetTypes ran");
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no SpriteSheet struct (RegisterSpriteSheetTypes was not called)");
			return type;
		}

		[[nodiscard]] static Result<SpriteSheetData> ReadSpriteSheetDocument(const Json& document, const TypeRegistry& registry, SpriteSheetLoadReport& report,
			bool strictUnknowns)
		{
			ENGINE_TRY_ASSIGN(const StructInfo* type, FindSpriteSheetType(registry));
			const JsonReader root(document);
			ENGINE_TRY_ASSIGN(report.FileVersion, root.ReadFormatHeader(SpriteSheetFormatName, MinimumSpriteSheetVersion, SpriteSheetFormatVersion));

			// Everything below the header is the SpriteSheet object; the header check above guarantees an object.
			Json fields = document;
			fields.erase("Format");
			fields.erase("Version");

			SpriteSheetData sheet;
			ReadContext context;
			context.Strict = strictUnknowns;
			context.Diagnostics = &report.Diagnostics;
			ENGINE_TRY(type->FromJson(&sheet, JsonReader(fields), context));
			ENGINE_TRY(ValidateSpriteSheet(sheet));
			return sheet;
		}

		// A payload count `count` (`what`: "frames") that must be at most `limit` and leave room for `minimumBytes` bytes per
		// item in what follows. Errors: Parse.
		[[nodiscard]] static Status CheckPayloadCount(const BinaryReader& reader, uint64_t count, uint64_t limit, uint64_t minimumBytes, std::string_view what)
		{
			if (count > limit)
				return MakeError(ErrorCode::Parse, "sprite sheet payload: {} {} at offset {}, more than the {} a sheet may have", count, what, reader.GetPosition(), limit);
			if (count * minimumBytes > reader.GetRemaining())
			{
				return MakeError(ErrorCode::Parse, "sprite sheet payload: {} {} need at least {} bytes, but {} follow at offset {}", count, what, count * minimumBytes,
					reader.GetRemaining(), reader.GetPosition());
			}
			return {};
		}

		[[nodiscard]] static Result<SpriteFrame> ReadPayloadFrame(BinaryReader& reader)
		{
			SpriteFrame frame;
			ENGINE_TRY_ASSIGN(frame.Name, reader.ReadString());
			ENGINE_TRY_ASSIGN(frame.Rect.X, reader.ReadU32());
			ENGINE_TRY_ASSIGN(frame.Rect.Y, reader.ReadU32());
			ENGINE_TRY_ASSIGN(frame.Rect.Width, reader.ReadU32());
			ENGINE_TRY_ASSIGN(frame.Rect.Height, reader.ReadU32());
			ENGINE_TRY_ASSIGN(frame.Pivot.x, reader.ReadF32());
			ENGINE_TRY_ASSIGN(frame.Pivot.y, reader.ReadF32());
			ENGINE_TRY_ASSIGN(frame.Borders.Left, reader.ReadU32());
			ENGINE_TRY_ASSIGN(frame.Borders.Top, reader.ReadU32());
			ENGINE_TRY_ASSIGN(frame.Borders.Right, reader.ReadU32());
			ENGINE_TRY_ASSIGN(frame.Borders.Bottom, reader.ReadU32());
			return frame;
		}

		[[nodiscard]] static Result<SpriteClip> ReadPayloadClip(BinaryReader& reader)
		{
			SpriteClip clip;
			ENGINE_TRY_ASSIGN(clip.Name, reader.ReadString());
			const size_t loopOffset = reader.GetPosition();
			ENGINE_TRY_ASSIGN(const uint8_t loop, reader.ReadU8());
			if (loop > 1)
				return MakeError(ErrorCode::Parse, "sprite sheet payload: the Loop byte at offset {} is {} instead of 0 or 1", loopOffset, loop);
			clip.Loop = loop == 1;
			ENGINE_TRY_ASSIGN(const uint32_t stepCount, reader.ReadU32());
			ENGINE_TRY(CheckPayloadCount(reader, stepCount, MaxSpriteClipFrames, StepPayloadBytes, "clip steps"));
			clip.Frames.reserve(stepCount);
			for (uint32_t index = 0; index < stepCount; ++index)
			{
				SpriteClipFrame step;
				ENGINE_TRY_ASSIGN(step.Frame, reader.ReadU32());
				ENGINE_TRY_ASSIGN(step.Ticks, reader.ReadU32());
				clip.Frames.push_back(step);
			}
			return clip;
		}

	}

	void RegisterSpriteSheetTypes(TypeRegistry& registry)
	{
		// The per-field ranges; the rules across fields (a Rect inside the largest texture, Borders inside the Rect, names, the
		// clips' steps and frame indices, the counts) are ValidateSpriteSheet's, run as Validate hooks per frame, per clip and on
		// the whole sheet.
		const double maxTexel = static_cast<double>(MaxTextureDimension);
		registry.Struct<SpriteRect>("SpriteRect", "A frame's rectangle in its texture, in whole texels: x right and y down from the texture's top-left corner.")
			.Field("X", &SpriteRect::X, "The texel column of the frame's left edge.", { .Min = 0.0, .Max = maxTexel - 1.0, .Unit = "texels" })
			.Field("Y", &SpriteRect::Y, "The texel row of the frame's top edge (row 0 is the texture's top row).", { .Min = 0.0, .Max = maxTexel - 1.0, .Unit = "texels" })
			.Field("Width", &SpriteRect::Width, std::format("The frame's width; X + Width is at most {}.", MaxTextureDimension), { .Min = 1.0, .Max = maxTexel, .Unit = "texels" })
			.Field("Height", &SpriteRect::Height, std::format("The frame's height; Y + Height is at most {}.", MaxTextureDimension),
				{ .Min = 1.0, .Max = maxTexel, .Unit = "texels" });

		registry.Struct<SpriteBorders>("SpriteBorders",
					"A frame's 9-slice insets in texels: the corners keep their size and the edges and the centre stretch when the frame is drawn larger. "
					"All zero: the frame is not sliced.")
			.Field("Left", &SpriteBorders::Left, "The left inset; Left + Right is at most the frame's Width.", { .Min = 0.0, .Max = maxTexel, .Unit = "texels" })
			.Field("Top", &SpriteBorders::Top, "The top inset; Top + Bottom is at most the frame's Height.", { .Min = 0.0, .Max = maxTexel, .Unit = "texels" })
			.Field("Right", &SpriteBorders::Right, "The right inset.", { .Min = 0.0, .Max = maxTexel, .Unit = "texels" })
			.Field("Bottom", &SpriteBorders::Bottom, "The bottom inset.", { .Min = 0.0, .Max = maxTexel, .Unit = "texels" });

		registry.Struct<SpriteFrame>("SpriteFrame", "One frame of a sprite sheet: a rectangle of its texture with a pivot and 9-slice borders. Clips and sprites refer to it by its index.")
			.Field("Name", &SpriteFrame::Name,
				std::format("An optional label of at most {} bytes, without control characters, unique among the frames when not empty.", MaxSpriteSheetNameBytes))
			.Field("Rect", &SpriteFrame::Rect, "The frame's texel rectangle.")
			.Field("Pivot", &SpriteFrame::Pivot,
				"The point of the frame placed at its sprite's position, as a fraction of the frame's size from its top-left corner: (0.5, 0.5) is its centre, "
				"(0.5, 1) the middle of its bottom edge.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Borders", &SpriteFrame::Borders, "The frame's 9-slice insets.")
			.Validate(&Utils::ValidateSpriteFrameHook)
			.Generate(&Utils::GenerateSpriteFrame);

		registry.Struct<SpriteClipFrame>("SpriteClipFrame", "One step of a sprite clip: a frame shown for a number of fixed-step ticks.")
			.Field("Frame", &SpriteClipFrame::Frame, "The index of the frame in the sheet's Frames, from 0.", { .Min = 0.0, .Max = static_cast<double>(MaxSpriteSheetFrames - 1) })
			.Field("Ticks", &SpriteClipFrame::Ticks, "How many fixed-step ticks the frame shows.", { .Min = 1.0, .Max = static_cast<double>(MaxSpriteFrameTicks), .Unit = "ticks" });

		registry.Struct<SpriteClip>("SpriteClip", "A named animation over a sprite sheet's frames, driven by fixed-step ticks.")
			.Field("Name", &SpriteClip::Name,
				std::format("The clip's name, which scripts play it by: 1 to {} bytes without control characters, unique among the clips.", MaxSpriteSheetNameBytes))
			.Field("Frames", &SpriteClip::Frames, std::format("1 to {} steps, played in order.", MaxSpriteClipFrames))
			.Field("Loop", &SpriteClip::Loop, "Whether the clip starts again after its last step; otherwise it holds its last frame.")
			.Validate(&Utils::ValidateSpriteClipHook)
			.Generate(&Utils::GenerateSpriteClip);

		registry.Struct<SpriteSheetData>("SpriteSheet",
					"A sprite sheet (.spritesheet): one texture, the frames cut from it with their pivots and 9-slice borders, and named clips of frames with tick "
					"durations.")
			.Field("Texture", &SpriteSheetData::Texture, "The texture the frames are cut from; null draws the frames as opaque white.")
			.Field("Frames", &SpriteSheetData::Frames, std::format("At most {} frames; a frame's identity is its index.", MaxSpriteSheetFrames))
			.Field("Clips", &SpriteSheetData::Clips, std::format("At most {} clips, in authored order.", MaxSpriteSheetClips))
			.Validate(&Utils::ValidateSpriteSheetHook)
			.Generate(&Utils::GenerateSpriteSheet);
	}

	Status ValidateSpriteSheet(const SpriteSheetData& sheet)
	{
		ValidationContext context;
		Utils::ValidateSheet(sheet, context);
		return context.ToStatus("the sprite sheet");
	}

	std::optional<uint32_t> FindSpriteFrame(const SpriteSheetData& sheet, std::string_view name)
	{
		if (name.empty())
			return std::nullopt;
		const auto found = std::ranges::find(sheet.Frames, name, &SpriteFrame::Name);
		if (found == sheet.Frames.end())
			return std::nullopt;
		return static_cast<uint32_t>(found - sheet.Frames.begin());
	}

	const SpriteClip* FindSpriteClip(const SpriteSheetData& sheet, std::string_view name)
	{
		const auto found = std::ranges::find(sheet.Clips, name, &SpriteClip::Name);
		return found == sheet.Clips.end() ? nullptr : &*found;
	}

	std::vector<AssetHandle> GetSpriteSheetDependencies(const SpriteSheetData& sheet)
	{
		std::vector<AssetHandle> handles;
		if (sheet.Texture.GetHandle().IsValid())
			handles.push_back(sheet.Texture.GetHandle());
		return handles;
	}

	Result<Json> SpriteSheetToJson(const SpriteSheetData& sheet, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const StructInfo* type, Utils::FindSpriteSheetType(registry));
		ENGINE_TRY_ASSIGN(Json fields, type->ToJson(&sheet));

		// The header first, then the fields in registry order (§6).
		Json document = Json::object();
		document["Format"] = std::string(SpriteSheetFormatName);
		document["Version"] = SpriteSheetFormatVersion;
		for (auto field = fields.begin(); field != fields.end(); ++field)
			document[field.key()] = std::move(*field);
		return document;
	}

	Result<std::string> SpriteSheetToText(const SpriteSheetData& sheet, const TypeRegistry& registry, JsonStyle style)
	{
		ENGINE_TRY_ASSIGN(const Json document, SpriteSheetToJson(sheet, registry));
		return JsonWriter::Write(document, style);
	}

	Result<SpriteSheetData> SpriteSheetFromJson(const Json& document, const TypeRegistry& registry, SpriteSheetLoadReport& report, bool strictUnknowns)
	{
		report = SpriteSheetLoadReport{};
		return Utils::ReadSpriteSheetDocument(document, registry, report, strictUnknowns);
	}

	Result<SpriteSheetData> SpriteSheetFromText(std::string_view text, const TypeRegistry& registry, SpriteSheetLoadReport& report, bool strictUnknowns)
	{
		report = SpriteSheetLoadReport{};
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		return SpriteSheetFromJson(document, registry, report, strictUnknowns);
	}

	Buffer SerializeSpriteSheetPayload(const SpriteSheetData& sheet)
	{
		ENGINE_CORE_ASSERT(ValidateSpriteSheet(sheet).has_value(), "Only a valid sprite sheet is cooked");
		BinaryWriter writer;
		writer.WriteU64(sheet.Texture.GetHandle().GetValue());
		writer.WriteU32(static_cast<uint32_t>(sheet.Frames.size()));
		writer.WriteU32(static_cast<uint32_t>(sheet.Clips.size()));
		for (const SpriteFrame& frame : sheet.Frames)
		{
			writer.WriteString(frame.Name);
			writer.WriteU32(frame.Rect.X);
			writer.WriteU32(frame.Rect.Y);
			writer.WriteU32(frame.Rect.Width);
			writer.WriteU32(frame.Rect.Height);
			writer.WriteF32(frame.Pivot.x);
			writer.WriteF32(frame.Pivot.y);
			writer.WriteU32(frame.Borders.Left);
			writer.WriteU32(frame.Borders.Top);
			writer.WriteU32(frame.Borders.Right);
			writer.WriteU32(frame.Borders.Bottom);
		}
		for (const SpriteClip& clip : sheet.Clips)
		{
			writer.WriteString(clip.Name);
			writer.WriteU8(clip.Loop ? 1 : 0);
			writer.WriteU32(static_cast<uint32_t>(clip.Frames.size()));
			for (const SpriteClipFrame& step : clip.Frames)
			{
				writer.WriteU32(step.Frame);
				writer.WriteU32(step.Ticks);
			}
		}
		return writer.TakeBuffer();
	}

	Result<SpriteSheetData> DeserializeSpriteSheetPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		SpriteSheetData sheet;
		ENGINE_TRY_ASSIGN(const uint64_t texture, reader.ReadU64());
		sheet.Texture = TypedAssetHandle<AssetType::Texture>(AssetHandle(texture));
		ENGINE_TRY_ASSIGN(const uint32_t frameCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t clipCount, reader.ReadU32());
		ENGINE_TRY(Utils::CheckPayloadCount(reader, frameCount, MaxSpriteSheetFrames, Utils::MinFramePayloadBytes, "frames"));
		ENGINE_TRY(Utils::CheckPayloadCount(reader, clipCount, MaxSpriteSheetClips, Utils::MinClipPayloadBytes, "clips"));
		if (static_cast<uint64_t>(frameCount) * Utils::MinFramePayloadBytes + static_cast<uint64_t>(clipCount) * Utils::MinClipPayloadBytes > reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "sprite sheet payload: {} frames and {} clips need more than the {} bytes that follow at offset {}", frameCount, clipCount,
				reader.GetRemaining(), reader.GetPosition());
		}

		sheet.Frames.reserve(frameCount);
		for (uint32_t index = 0; index < frameCount; ++index)
		{
			ENGINE_TRY_ASSIGN(SpriteFrame frame, Utils::ReadPayloadFrame(reader));
			sheet.Frames.push_back(std::move(frame));
		}
		sheet.Clips.reserve(clipCount);
		for (uint32_t index = 0; index < clipCount; ++index)
		{
			ENGINE_TRY_ASSIGN(SpriteClip clip, Utils::ReadPayloadClip(reader));
			sheet.Clips.push_back(std::move(clip));
		}
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "sprite sheet payload has {} trailing bytes after offset {}", reader.GetRemaining(), reader.GetPosition());
		ENGINE_TRY(ValidateSpriteSheet(sheet));
		return sheet;
	}

	Buffer CookSpriteSheet(const SpriteSheetData& sheet, uint32_t importerVersion)
	{
		const Buffer payload = SerializeSpriteSheetPayload(sheet);
		return WriteCookedArtifact(AssetType::SpriteSheet, SpriteSheetData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<SpriteSheetData>> LoadCookedSpriteSheet(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::SpriteSheet, SpriteSheetData::FormatVersion));
		ENGINE_TRY_ASSIGN(SpriteSheetData sheet, DeserializeSpriteSheetPayload(view.Payload));
		return AssetRef<SpriteSheetData>(CreateRef<SpriteSheetData>(std::move(sheet)));
	}

}
