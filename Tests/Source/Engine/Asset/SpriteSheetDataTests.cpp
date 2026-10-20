#include "TestsPCH.h"

#include "Engine/Asset/SpriteSheetData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/BinaryWriter.h"
#include "Support/Fuzz.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <limits>

// Sprite sheets (M18, C18.1, R-2D-SHEET; Docs/Decisions/0029-m18-contract.md decisions 14 to 16): the .spritesheet
// document, its rules and its cooked payload. Skipped until stream S implements Asset/SpriteSheetData.cpp and registers the
// "SpriteSheet" types.

namespace Engine {

	namespace {

		// A sheet with every field set somewhere (canonical order and spelling): a frame with a bottom pivot, a 9-slice frame,
		// an unnamed frame, a looping clip and one that holds its last frame.
		constexpr std::string_view HeroSheet = R"({
	"Format": "SpriteSheet",
	"Version": 1,
	"Texture": "4a1c0e7d2b9f3a65",
	"Frames": [
		{
			"Name": "Idle0",
			"Rect": {
				"X": 0,
				"Y": 0,
				"Width": 8,
				"Height": 8
			},
			"Pivot": [0.5, 1],
			"Borders": {
				"Left": 0,
				"Top": 0,
				"Right": 0,
				"Bottom": 0
			}
		},
		{
			"Name": "Idle1",
			"Rect": {
				"X": 8,
				"Y": 0,
				"Width": 8,
				"Height": 8
			},
			"Pivot": [0.5, 1],
			"Borders": {
				"Left": 0,
				"Top": 0,
				"Right": 0,
				"Bottom": 0
			}
		},
		{
			"Name": "Panel",
			"Rect": {
				"X": 0,
				"Y": 8,
				"Width": 12,
				"Height": 6
			},
			"Pivot": [0.5, 0.5],
			"Borders": {
				"Left": 3,
				"Top": 2,
				"Right": 4,
				"Bottom": 1
			}
		},
		{
			"Name": "",
			"Rect": {
				"X": 12,
				"Y": 8,
				"Width": 1,
				"Height": 1
			},
			"Pivot": [0, 0],
			"Borders": {
				"Left": 0,
				"Top": 0,
				"Right": 0,
				"Bottom": 0
			}
		}
	],
	"Clips": [
		{
			"Name": "Idle",
			"Frames": [
				{
					"Frame": 0,
					"Ticks": 8
				},
				{
					"Frame": 1,
					"Ticks": 12
				}
			],
			"Loop": true
		},
		{
			"Name": "Hit",
			"Frames": [
				{
					"Frame": 1,
					"Ticks": 3
				},
				{
					"Frame": 3,
					"Ticks": 65535
				}
			],
			"Loop": false
		}
	]
}
)";

		// HeroSheet as data.
		SpriteSheetData MakeHeroSheet()
		{
			SpriteSheetData sheet;
			sheet.Texture = TypedAssetHandle<AssetType::Texture>(AssetHandle(0x4a1c0e7d2b9f3a65ull));
			sheet.Frames = {
				{ .Name = "Idle0", .Rect = { .X = 0, .Y = 0, .Width = 8, .Height = 8 }, .Pivot = { 0.5f, 1.0f }, .Borders = {} },
				{ .Name = "Idle1", .Rect = { .X = 8, .Y = 0, .Width = 8, .Height = 8 }, .Pivot = { 0.5f, 1.0f }, .Borders = {} },
				{ .Name = "Panel", .Rect = { .X = 0, .Y = 8, .Width = 12, .Height = 6 }, .Pivot = { 0.5f, 0.5f }, .Borders = { .Left = 3, .Top = 2, .Right = 4, .Bottom = 1 } },
				{ .Name = "", .Rect = { .X = 12, .Y = 8, .Width = 1, .Height = 1 }, .Pivot = { 0.0f, 0.0f }, .Borders = {} },
			};
			sheet.Clips = {
				{ .Name = "Idle", .Frames = { { .Frame = 0, .Ticks = 8 }, { .Frame = 1, .Ticks = 12 } }, .Loop = true },
				{ .Name = "Hit", .Frames = { { .Frame = 1, .Ticks = 3 }, { .Frame = 3, .Ticks = MaxSpriteFrameTicks } }, .Loop = false },
			};
			return sheet;
		}

		// The first issue's JSON pointer of a failed read, or "" when it has none.
		std::string FirstPointer(const Error& error)
		{
			if (!error.GetIssues().empty())
				return error.GetIssues().front().JsonPointer;
			return error.GetLocation().JsonPointer.value_or(std::string());
		}

		// The error code of a failed result; nullopt for one that holds a value.
		template<typename T>
		std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// HeroSheet with `change` applied, validated.
		template<typename Change>
		Status ValidateChanged(Change&& change)
		{
			SpriteSheetData sheet = MakeHeroSheet();
			change(sheet);
			return ValidateSpriteSheet(sheet);
		}

		// Fails unless HeroSheet with `change` applied is a Validation error whose first issue is located at `pointer`.
		template<typename Change>
		void CheckInvalid(std::string_view pointer, Change&& change)
		{
			CAPTURE(pointer);
			const Status status = ValidateChanged(std::forward<Change>(change));
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			CHECK(FirstPointer(status.error()) == pointer);
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("SpriteSheetData: a .spritesheet file round-trips byte-identically" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			SpriteSheetLoadReport report;
			const Result<SpriteSheetData> sheet = SpriteSheetFromText(HeroSheet, *registry, report, true);
			REQUIRE_MESSAGE(sheet.has_value(), sheet.error().ToString());
			CHECK(report.FileVersion == SpriteSheetFormatVersion);
			CHECK(report.Diagnostics.empty());
			const SpriteSheetData expected = MakeHeroSheet();
			CHECK(sheet->Texture == expected.Texture);
			CHECK(sheet->Frames == expected.Frames);
			CHECK(sheet->Clips == expected.Clips);
			CHECK(SpriteSheetToText(*sheet, *registry).value_or(std::string()) == HeroSheet);
			// The defaults: no texture, no frames, no clips, every field written.
			const Result<std::string> empty = SpriteSheetToText(SpriteSheetData{}, *registry);
			REQUIRE(empty.has_value());
			CHECK(*empty == "{\n\t\"Format\": \"SpriteSheet\",\n\t\"Version\": 1,\n\t\"Texture\": null,\n\t\"Frames\": [],\n\t\"Clips\": []\n}\n");
			// An unknown member warns, or fails a strict read (the importer's).
			std::string unknown(HeroSheet);
			unknown.insert(unknown.find("\"Texture\""), "\"Speed\": 2,\n\t");
			SpriteSheetLoadReport lenient;
			CHECK(SpriteSheetFromText(unknown, *registry, lenient).has_value());
			CHECK(lenient.Diagnostics.size() == 1);
			SpriteSheetLoadReport strict;
			CHECK_FALSE(SpriteSheetFromText(unknown, *registry, strict, true).has_value());
			// A newer file names both versions.
			std::string newer(HeroSheet);
			newer.replace(newer.find("\"Version\": 1"), std::string_view("\"Version\": 1").size(), "\"Version\": 2");
			CHECK(ErrorCodeOf(SpriteSheetFromText(newer, *registry, report)) == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("SpriteSheetData: invalid frames, borders and clips are located Validation errors" * doctest::skip(true))
		{
			REQUIRE(ValidateSpriteSheet(MakeHeroSheet()).has_value());
			CheckInvalid("/Frames/0/Rect/Width", [](SpriteSheetData& sheet)
			{
				sheet.Frames[0].Rect.Width = 0;
			});
			CheckInvalid("/Frames/1/Rect", [](SpriteSheetData& sheet)
			{
				sheet.Frames[1].Rect.X = MaxTextureDimension - 4;
			});
			CheckInvalid("/Frames/2/Borders", [](SpriteSheetData& sheet)
			{
				sheet.Frames[2].Borders.Right = 10;
			});
			CheckInvalid("/Frames/2/Borders", [](SpriteSheetData& sheet)
			{
				sheet.Frames[2].Borders.Bottom = 5;
			});
			CheckInvalid("/Frames/0/Pivot", [](SpriteSheetData& sheet)
			{
				sheet.Frames[0].Pivot.x = 1.5f;
			});
			CheckInvalid("/Frames/0/Pivot", [](SpriteSheetData& sheet)
			{
				sheet.Frames[0].Pivot.y = std::numeric_limits<float>::quiet_NaN();
			});
			CheckInvalid("/Frames/1/Name", [](SpriteSheetData& sheet)
			{
				sheet.Frames[1].Name = "Idle0";
			});
			CheckInvalid("/Frames/1/Name", [](SpriteSheetData& sheet)
			{
				sheet.Frames[1].Name = std::string(MaxSpriteSheetNameBytes + 1, 'a');
			});
			CheckInvalid("/Frames/1/Name", [](SpriteSheetData& sheet)
			{
				sheet.Frames[1].Name = "Tab\tName";
			});
			CheckInvalid("/Clips/0/Name", [](SpriteSheetData& sheet)
			{
				sheet.Clips[0].Name.clear();
			});
			CheckInvalid("/Clips/1/Name", [](SpriteSheetData& sheet)
			{
				sheet.Clips[1].Name = "Idle";
			});
			CheckInvalid("/Clips/0/Frames", [](SpriteSheetData& sheet)
			{
				sheet.Clips[0].Frames.clear();
			});
			CheckInvalid("/Clips/1/Frames/1/Frame", [](SpriteSheetData& sheet)
			{
				sheet.Clips[1].Frames[1].Frame = 4;
			});
			CheckInvalid("/Clips/0/Frames/0/Ticks", [](SpriteSheetData& sheet)
			{
				sheet.Clips[0].Frames[0].Ticks = 0;
			});
			CheckInvalid("/Clips/0/Frames/0/Ticks", [](SpriteSheetData& sheet)
			{
				sheet.Clips[0].Frames[0].Ticks = MaxSpriteFrameTicks + 1;
			});
			CheckInvalid("/Clips/0/Frames", [](SpriteSheetData& sheet)
			{
				sheet.Clips[0].Frames.resize(MaxSpriteClipFrames + 1, { .Frame = 0, .Ticks = 1 });
			});
			CheckInvalid("/Frames", [](SpriteSheetData& sheet)
			{
				sheet.Frames.resize(MaxSpriteSheetFrames + 1);
			});
			CheckInvalid("/Clips", [](SpriteSheetData& sheet)
			{
				for (uint32_t index = 0; index < MaxSpriteSheetClips; ++index)
					sheet.Clips.push_back({ .Name = std::format("Clip{}", index), .Frames = { { .Frame = 0, .Ticks = 1 } }, .Loop = true });
			});
			// A rectangle that ends exactly at the largest texture, a null texture and an unnamed frame are valid.
			CHECK(ValidateChanged([](SpriteSheetData& sheet)
			{
				sheet.Frames[3].Rect = { .X = MaxTextureDimension - 1, .Y = MaxTextureDimension - 1, .Width = 1, .Height = 1 };
				sheet.Texture = {};
			}).has_value());

			// The document reader reports the same rules, located in the file.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			std::string text(HeroSheet);
			text.replace(text.find("\"Ticks\": 12"), std::string_view("\"Ticks\": 12").size(), "\"Ticks\": 0");
			SpriteSheetLoadReport report;
			const Result<SpriteSheetData> read = SpriteSheetFromText(text, *registry, report);
			REQUIRE_FALSE(read.has_value());
			CHECK(read.error().GetCode() == ErrorCode::Validation);
			CHECK(FirstPointer(read.error()) == "/Clips/0/Frames/1/Ticks");
		}

		TEST_CASE("SpriteSheetData: the cooked payload is the documented little-endian layout" * doctest::skip(true))
		{
			// The bytes SpriteSheetData.h documents, written here independently of the serializer, so the Debug and Release
			// unit runs both prove that a sheet cooks to the same bytes in every configuration (Roadmap M18: "A SpriteSheet
			// cooks byte-identically in the Debug and Release editors").
			const SpriteSheetData sheet = MakeHeroSheet();
			BinaryWriter expected;
			expected.WriteU64(0x4a1c0e7d2b9f3a65ull);
			expected.WriteU32(4);
			expected.WriteU32(2);
			for (const SpriteFrame& frame : sheet.Frames)
			{
				expected.WriteString(frame.Name);
				expected.WriteU32(frame.Rect.X);
				expected.WriteU32(frame.Rect.Y);
				expected.WriteU32(frame.Rect.Width);
				expected.WriteU32(frame.Rect.Height);
				expected.WriteU32(std::bit_cast<uint32_t>(frame.Pivot.x));
				expected.WriteU32(std::bit_cast<uint32_t>(frame.Pivot.y));
				expected.WriteU32(frame.Borders.Left);
				expected.WriteU32(frame.Borders.Top);
				expected.WriteU32(frame.Borders.Right);
				expected.WriteU32(frame.Borders.Bottom);
			}
			for (const SpriteClip& clip : sheet.Clips)
			{
				expected.WriteString(clip.Name);
				expected.WriteU8(clip.Loop ? 1 : 0);
				expected.WriteU32(static_cast<uint32_t>(clip.Frames.size()));
				for (const SpriteClipFrame& step : clip.Frames)
				{
					expected.WriteU32(step.Frame);
					expected.WriteU32(step.Ticks);
				}
			}
			const Buffer payload = SerializeSpriteSheetPayload(sheet);
			CHECK(payload == expected.TakeBuffer());

			const Buffer cooked = CookSpriteSheet(sheet, 1);
			const Result<CookedArtifactView> view = ReadCookedArtifact(cooked, AssetType::SpriteSheet, SpriteSheetData::FormatVersion);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			CHECK(view->Header.ImporterVersion == 1);
			CHECK(std::ranges::equal(view->Payload, payload));
			CHECK(CookSpriteSheet(sheet, 1) == cooked);

			const Result<AssetRef<SpriteSheetData>> loaded = LoadCookedSpriteSheet(cooked);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->GetAssetType() == AssetType::SpriteSheet);
			CHECK((*loaded)->Texture == sheet.Texture);
			CHECK((*loaded)->Frames == sheet.Frames);
			CHECK((*loaded)->Clips == sheet.Clips);
		}

		TEST_CASE("SpriteSheetData: a damaged payload is refused, never trusted" * doctest::skip(true))
		{
			const Buffer payload = SerializeSpriteSheetPayload(MakeHeroSheet());
			// Truncated anywhere, or with a byte left over.
			for (size_t size = 0; size < payload.size(); ++size)
			{
				CAPTURE(size);
				CHECK_FALSE(DeserializeSpriteSheetPayload(std::span<const std::byte>(payload).first(size)).has_value());
			}
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(ErrorCodeOf(DeserializeSpriteSheetPayload(trailing)) == ErrorCode::Parse);
			// A frame count above the limit is refused before anything is allocated.
			Buffer counted = payload;
			const uint32_t huge = std::numeric_limits<uint32_t>::max();
			std::memcpy(counted.data() + sizeof(uint64_t), &huge, sizeof(huge));
			CHECK(ErrorCodeOf(DeserializeSpriteSheetPayload(counted)) == ErrorCode::Parse);
			// A clip step that names a frame the sheet lacks is a Validation error, as in the document.
			const Buffer valid = SerializeSpriteSheetPayload(MakeHeroSheet());
			Buffer wrongFrame = valid;
			const uint32_t missing = 9;
			std::memcpy(wrongFrame.data() + wrongFrame.size() - 2 * sizeof(uint32_t), &missing, sizeof(missing));
			CHECK(ErrorCodeOf(DeserializeSpriteSheetPayload(wrongFrame)) == ErrorCode::Validation);
			// Another type or format version.
			CHECK_FALSE(LoadCookedSpriteSheet(WriteCookedArtifact(AssetType::Texture, SpriteSheetData::FormatVersion, 1, valid)).has_value());
			CHECK(ErrorCodeOf(LoadCookedSpriteSheet(WriteCookedArtifact(AssetType::SpriteSheet, SpriteSheetData::FormatVersion + 1, 1, valid)))
				== ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("SpriteSheetData: 1,000 seeded mutations of a sprite sheet file and of its cooked form never crash" * doctest::test_suite(Test::FuzzSuite)
			* doctest::skip(true))
		{
			// The seed corpus: the sprite sheet fixtures of Tests/Data/Assets/SpriteSheets and this file's own. The text goes
			// through SpriteSheetFromText, the cooked form (the payload hash sealed again) through LoadCookedSpriteSheet; a sheet
			// either accepts is valid and writes back.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			std::vector<std::string> seeds = Test::ReadFuzzSeedTexts(Test::GetRepositoryRoot() / "Tests" / "Data" / "Assets" / "SpriteSheets", ".spritesheet");
			seeds.emplace_back(HeroSheet);
			const Buffer payload = SerializeSpriteSheetPayload(MakeHeroSheet());
			const Test::FuzzRun run = Test::RunFuzzCase(0x5B5EE7, 1000, [&registry, &seeds, &payload](Random& random, uint64_t /*index*/)
			{
				if (random.NextBool())
				{
					std::string mutated = seeds[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 1))];
					Test::MutateText(mutated, random);
					SpriteSheetLoadReport report;
					const Result<SpriteSheetData> sheet = SpriteSheetFromText(mutated, *registry, report, random.NextBool());
					if (!sheet.has_value())
						return false;
					CHECK(ValidateSpriteSheet(*sheet).has_value());
					CHECK(SpriteSheetToText(*sheet, *registry).has_value());
					return true;
				}
				std::vector<std::byte> mutated(payload.begin(), payload.end());
				Test::MutateBytes(mutated, random);
				const Result<AssetRef<SpriteSheetData>> loaded = LoadCookedSpriteSheet(
					WriteCookedArtifact(AssetType::SpriteSheet, SpriteSheetData::FormatVersion, 1, mutated));
				if (!loaded.has_value())
					return false;
				CHECK(ValidateSpriteSheet(**loaded).has_value());
				// Every accepted payload is canonical: it writes back byte for byte.
				CHECK(SerializeSpriteSheetPayload(**loaded) == mutated);
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		TEST_CASE("SpriteSheetData: frames and clips are found by name, and the texture is its only dependency" * doctest::skip(true))
		{
			const SpriteSheetData sheet = MakeHeroSheet();
			CHECK(FindSpriteFrame(sheet, "Panel") == std::optional<uint32_t>(2));
			CHECK_FALSE(FindSpriteFrame(sheet, "Missing").has_value());
			// Unnamed frames are never found by name.
			CHECK_FALSE(FindSpriteFrame(sheet, "").has_value());
			const SpriteClip* hit = FindSpriteClip(sheet, "Hit");
			REQUIRE(hit != nullptr);
			CHECK_FALSE(hit->Loop);
			CHECK(FindSpriteClip(sheet, "hit") == nullptr);
			CHECK(GetSpriteSheetDependencies(sheet) == std::vector<AssetHandle>{ AssetHandle(0x4a1c0e7d2b9f3a65ull) });
			CHECK(GetSpriteSheetDependencies(SpriteSheetData{}).empty());
		}
	}

}
