#include "TestsPCH.h"

#include "Engine/Renderer/UiPass.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// The Ui pass (M22, C22.1, R-UI-RENDER; Docs/Decisions/0033-m22-contract.md decision 7): planning a UI draw list (sheet frames,
// 9-slices, text, runs, annotations) and recording it in the SceneOverlay stage after game Text, before the upscale, clipped
// by the QuadBatcher's and the glyph runs' scissors. Texts and annotation labels lay out through TextMeasurement.h.

namespace Engine {

	namespace {

		constexpr AssetHandle SheetTexture{ 0x7E57 };
		constexpr AssetHandle Sheet{ 0x5EE7 };
		constexpr AssetHandle TestFont{ 0xF047 };

		// The texel (x, y) of the test texture: distinct bytes everywhere, opaque.
		glm::u8vec4 MakeTexel(uint32_t x, uint32_t y)
		{
			return glm::u8vec4(static_cast<uint8_t>(40 + 50 * x), static_cast<uint8_t>(30 + 60 * y), static_cast<uint8_t>(200 - 20 * (x + y)), 255);
		}

		// A `width` x `height` sRGB colour texture of MakeTexel, one level.
		Ref<TextureData> MakeTexture(uint32_t width, uint32_t height)
		{
			auto texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = width;
			texture->Height = height;
			texture->Mips = { TextureMip{ .Width = width, .Height = height, .Offset = 0, .Size = static_cast<uint64_t>(width) * height * 4 } };
			for (uint32_t y = 0; y < height; ++y)
			{
				for (uint32_t x = 0; x < width; ++x)
				{
					const glm::u8vec4 texel = MakeTexel(x, y);
					for (int channel = 0; channel < 4; ++channel)
						texture->Pixels.push_back(std::byte{ texel[channel] });
				}
			}
			REQUIRE(ValidateTextureData(*texture).has_value());
			return texture;
		}

		// A sheet of the 16 x 8 test texture: frame 0 its right half's 8 x 8 texels, frame 1 its left half with 2-texel borders.
		Ref<SpriteSheetData> MakeSheet()
		{
			auto sheet = CreateRef<SpriteSheetData>();
			sheet->Texture = TypedAssetHandle<AssetType::Texture>(SheetTexture);
			sheet->Frames = {
				SpriteFrame{ .Rect = SpriteRect{ .X = 8, .Y = 0, .Width = 8, .Height = 8 } },
				SpriteFrame{ .Rect = SpriteRect{ .X = 0, .Y = 0, .Width = 8, .Height = 8 }, .Borders = SpriteBorders{ .Left = 2, .Top = 2, .Right = 2, .Bottom = 2 } },
			};
			return sheet;
		}

		// TextLayoutTests' font: ascent 0.8, descent -0.2, line gap 0.1; 'A' advances 0.5, 'B' 0.6, ' ' 0.25; "AB" kerns -0.1.
		Ref<FontData> MakeFont()
		{
			auto font = CreateRef<FontData>();
			font->Ascent = 0.8f;
			font->Descent = -0.2f;
			font->LineGap = 0.1f;
			font->AtlasWidth = 4;
			font->AtlasHeight = 4;
			font->AtlasPixels.assign(16, std::byte{ 0 });
			font->Glyphs = {
				FontGlyph{ .Codepoint = ' ', .Advance = 0.25f },
				FontGlyph{ .Codepoint = 'A', .Advance = 0.5f, .PlaneMin = glm::vec2(0.0f), .PlaneMax = glm::vec2(0.5f, 0.7f), .AtlasMin = glm::vec2(0.0f), .AtlasMax = glm::vec2(0.5f) },
				FontGlyph{ .Codepoint = 'B', .Advance = 0.6f, .PlaneMin = glm::vec2(0.05f, 0.0f), .PlaneMax = glm::vec2(0.55f, 0.7f), .AtlasMin = glm::vec2(0.5f), .AtlasMax = glm::vec2(1.0f) },
			};
			font->Kerning = { FontKerningPair{ .First = 'A', .Second = 'B', .Advance = -0.1f } };
			REQUIRE(ValidateFontData(*font).has_value());
			return font;
		}

		// An asset manager with the test texture, the sheet, the test font and the same font as the Default font.
		void PublishUiAssets(Test::InMemoryAssetManager& assets)
		{
			assets.Publish(SheetTexture, MakeTexture(16, 8));
			assets.Publish(Sheet, MakeSheet());
			assets.Publish(TestFont, MakeFont());
			assets.Publish(BuiltinAssetHandles::DefaultFont, MakeFont());
		}

		UiDrawItem MakeImage(glm::vec2 min, glm::vec2 max, AssetHandle sheet = {}, uint32_t frame = 0)
		{
			return UiDrawItem{ .Kind = UiDrawKind::Image, .Min = min, .Max = max, .Sheet = sheet, .Frame = frame };
		}

		UiDrawItem MakeText(glm::vec2 min, glm::vec2 max, std::string text, glm::vec4 color = glm::vec4(1.0f))
		{
			return UiDrawItem{ .Kind = UiDrawKind::Text, .Min = min, .Max = max, .Color = color, .Text = std::move(text), .Font = TestFont, .FontSize = 10.0f };
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("UiPass: a plain image is one white quad of its colour over its rectangle")
		{
			Test::InMemoryAssetManager assets;
			UiDrawList list;
			UiDrawItem item = MakeImage(glm::vec2(2.0f, 3.0f), glm::vec2(10.0f, 7.0f));
			item.Color = glm::vec4(0.25f, 0.5f, 1.0f, 0.5f);
			list.Items.push_back(item);
			const UiPassPlan plan = PlanUiPass(list, assets);
			REQUIRE(plan.Quads.size() == 1);
			CHECK(plan.Quads[0]
				== UiPassQuad{ .Min = glm::vec2(2.0f, 3.0f), .Max = glm::vec2(10.0f, 7.0f), .Color = glm::vec4(0.25f, 0.5f, 1.0f, 0.5f), .Texture = AssetHandle{}, .Filter = UiDrawFilter::Point });
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0] == UiPassRun{ .Kind = UiPassRunKind::Quads, .First = 0, .Count = 1 });
			CHECK(plan.DrawnItems == 1);
			CHECK(plan.SkippedItems == 0);
		}

		TEST_CASE("UiPass: a Simple image samples its frame's texel rectangle and a Sliced one its nine patches")
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			UiDrawList list;
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(8.0f), Sheet, 0));
			UiDrawItem sliced = MakeImage(glm::vec2(4.0f, 2.0f), glm::vec2(24.0f, 18.0f), Sheet, 1);
			sliced.ImageType = UiDrawImageType::Sliced;
			sliced.Filter = UiDrawFilter::Linear;
			list.Items.push_back(sliced);
			const UiPassPlan plan = PlanUiPass(list, assets);
			REQUIRE(plan.Quads.size() == 10);
			// Frame 0 is texels [8, 16) x [0, 8) of 16 x 8: texel edges, no half-texel inset.
			CHECK(plan.Quads[0].TexCoordMin == glm::vec2(0.5f, 0.0f));
			CHECK(plan.Quads[0].TexCoordMax == glm::vec2(1.0f, 1.0f));
			CHECK(plan.Quads[0].Texture == SheetTexture);
			const NineSliceExpansion expected = ExpandNineSlice(
				NineSliceSource{ .TextureSize = glm::uvec2(16, 8), .RectMin = glm::uvec2(0), .RectSize = glm::uvec2(8), .Borders = glm::uvec4(2) }, glm::vec2(20.0f, 16.0f), 1.0f);
			REQUIRE(expected.Count == 9);
			for (uint32_t index = 0; index < expected.Count; ++index)
			{
				const UiPassQuad& quad = plan.Quads[1 + index];
				CAPTURE(index);
				CHECK(quad.Min == expected.Patches[index].Min + glm::vec2(4.0f, 2.0f));
				CHECK(quad.Max == expected.Patches[index].Max + glm::vec2(4.0f, 2.0f));
				CHECK(quad.TexCoordMin == expected.Patches[index].TexCoordMin);
				CHECK(quad.TexCoordMax == expected.Patches[index].TexCoordMax);
				CHECK(quad.Filter == UiDrawFilter::Linear);
			}
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0].Count == 10);
			CHECK(plan.DrawnItems == 2);
		}

		// The W22.1 review (Docs/Decisions/0033-m22-contract.md): a sheet has no placeholder, so the pass reports an unusable
		// one itself through the AssetManager's shared failure policy (§7.2), as GetOrPlaceholder reports a missing mesh or
		// font: ASSET_MISSING for an unknown handle and ASSET_TYPE_MISMATCH for an asset of another type, each recorded for
		// its handle and logged once however many images and frames use it. A frame the sheet lacks is project.validate's
		// UI_IMAGE_FRAME_OUT_OF_RANGE, with no asset diagnostic.
		TEST_CASE("UiPass: an image whose sheet is missing or lacks its frame draws nothing and is counted")
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			constexpr AssetHandle Missing{ 0xDEAD };
			UiDrawList list;
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(8.0f), Missing, 0));
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(8.0f), Sheet, 9));
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(8.0f)));
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(8.0f), Missing, 1));
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(8.0f), SheetTexture, 0)); // a texture, not a sheet
			const auto countDiagnostics = [&assets](std::string_view code, AssetHandle handle)
			{
				return std::ranges::count_if(assets.GetDiagnostics(), [code, handle](const AssetDiagnostic& diagnostic)
				{
					return diagnostic.Code == code && diagnostic.Asset == handle;
				});
			};
			Test::ExpectLog missing(LogLevel::Error, std::string(AssetMissingCode));
			Test::ExpectLog mismatch(LogLevel::Error, std::string(AssetTypeMismatchCode));
			for (int frame = 0; frame < 2; ++frame)
			{
				CAPTURE(frame);
				const UiPassPlan plan = PlanUiPass(list, assets);
				CHECK(plan.SkippedItems == 4);
				CHECK(plan.DrawnItems == 1);
				CHECK(plan.Quads.size() == 1);
				CHECK(countDiagnostics(AssetMissingCode, Missing) == 1);
				CHECK(countDiagnostics(AssetTypeMismatchCode, SheetTexture) == 1);
				CHECK(assets.GetDiagnostics().size() == 2);
			}
			CHECK(missing.GetMatchCount() == 1);
			CHECK(mismatch.GetMatchCount() == 1);
			// Reference diagnostics report a use, not the state of an asset: they never block play or export.
			CHECK_FALSE(assets.HasErrorDiagnostics());
		}

		TEST_CASE("UiPass: an item clipped away or without area draws nothing, and a sheet without a texture samples white")
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			constexpr AssetHandle Untextured{ 0x5EE8 };
			auto sheet = CreateRef<SpriteSheetData>();
			sheet->Frames = { SpriteFrame{ .Rect = SpriteRect{ .X = 2, .Y = 0, .Width = 2, .Height = 4 } } };
			assets.Publish(Untextured, sheet);
			UiDrawList list;
			UiDrawItem clippedAway = MakeImage(glm::vec2(0.0f), glm::vec2(8.0f), Sheet, 0);
			clippedAway.Clipped = true;
			clippedAway.Clip = UiClipRect{ .MinX = 4, .MinY = 0, .MaxX = 4, .MaxY = 8 };
			list.Items.push_back(clippedAway);
			list.Items.push_back(MakeImage(glm::vec2(8.0f), glm::vec2(8.0f, 12.0f)));                                  // no width
			list.Items.push_back(MakeImage(glm::vec2(8.0f), glm::vec2(4.0f)));                                         // inverted
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(std::numeric_limits<float>::infinity(), 4.0f))); // unbounded
			UiDrawItem sliced = MakeImage(glm::vec2(0.0f), glm::vec2(8.0f), Sheet, 1);
			sliced.ImageType = UiDrawImageType::Sliced;
			sliced.BorderScale = 0.0f; // no border size
			list.Items.push_back(sliced);
			// An unclipped item's clip rectangle is ignored, whatever it holds.
			UiDrawItem untextured = MakeImage(glm::vec2(1.0f), glm::vec2(3.0f, 5.0f), Untextured, 0);
			untextured.Clip = UiClipRect{ .MinX = 7, .MinY = 7, .MaxX = 7, .MaxY = 7 };
			list.Items.push_back(untextured);
			const UiPassPlan plan = PlanUiPass(list, assets);
			CHECK(plan.SkippedItems == 0);
			CHECK(plan.DrawnItems == 1);
			REQUIRE(plan.Quads.size() == 1);
			CHECK_FALSE(plan.Quads[0].Texture.IsValid());
			CHECK(plan.Quads[0].Min == glm::vec2(1.0f));
			CHECK(plan.Quads[0].Max == glm::vec2(3.0f, 5.0f));
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0] == UiPassRun{ .Kind = UiPassRunKind::Quads, .First = 0, .Count = 1 });
		}

		TEST_CASE("UiPass: an outline is four one-pixel edges inside its rectangle, and a thin one leaves out the edges without area")
		{
			Test::InMemoryAssetManager assets;
			UiDrawList list;
			list.Annotations.push_back(UiAnnotation{ .Min = glm::vec2(10.0f, 20.0f), .Max = glm::vec2(14.0f, 24.0f), .Color = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), .Outline = true });
			// One pixel high: its top row alone; two pixels wide and three high: its two rows and two one-pixel columns between them.
			list.Annotations.push_back(UiAnnotation{ .Min = glm::vec2(0.0f), .Max = glm::vec2(5.0f, 1.0f), .Outline = true });
			list.Annotations.push_back(UiAnnotation{ .Min = glm::vec2(0.0f), .Max = glm::vec2(2.0f, 3.0f), .Outline = true });
			// Without Outline and Label, an annotation draws nothing.
			list.Annotations.push_back(UiAnnotation{ .Min = glm::vec2(0.0f), .Max = glm::vec2(9.0f) });
			const UiPassPlan plan = PlanUiPass(list, assets);
			REQUIRE(plan.Quads.size() == 9);
			CHECK(plan.Quads[0].Min == glm::vec2(10.0f, 20.0f));
			CHECK(plan.Quads[0].Max == glm::vec2(14.0f, 21.0f));
			CHECK(plan.Quads[1].Min == glm::vec2(10.0f, 23.0f));
			CHECK(plan.Quads[1].Max == glm::vec2(14.0f, 24.0f));
			CHECK(plan.Quads[2].Min == glm::vec2(10.0f, 21.0f));
			CHECK(plan.Quads[2].Max == glm::vec2(11.0f, 23.0f));
			CHECK(plan.Quads[3].Min == glm::vec2(13.0f, 21.0f));
			CHECK(plan.Quads[3].Max == glm::vec2(14.0f, 23.0f));
			for (size_t index = 0; index < 4; ++index)
			{
				CHECK(plan.Quads[index].Color == glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
				CHECK_FALSE(plan.Quads[index].Texture.IsValid());
			}
			CHECK(plan.Quads[4].Min == glm::vec2(0.0f));
			CHECK(plan.Quads[4].Max == glm::vec2(5.0f, 1.0f));
			// The 2 x 3 rectangle: its rows, then its columns of one pixel each between them.
			CHECK(plan.Quads[5].Max == glm::vec2(2.0f, 1.0f));
			CHECK(plan.Quads[6].Min == glm::vec2(0.0f, 2.0f));
			CHECK(plan.Quads[7].Min == glm::vec2(0.0f, 1.0f));
			CHECK(plan.Quads[7].Max == glm::vec2(1.0f, 2.0f));
			CHECK(plan.Quads[8].Min == glm::vec2(1.0f, 1.0f));
			CHECK(plan.Quads[8].Max == glm::vec2(2.0f, 2.0f));
			// Annotations are not items, and all of them are one unclipped run.
			CHECK(plan.DrawnItems == 0);
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0] == UiPassRun{ .Kind = UiPassRunKind::Quads, .First = 0, .Count = 9 });
		}

		TEST_CASE("UiPass: a text lays out in its rectangle by its alignments and wraps at its width")
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			UiDrawList list;
			// "AB A" at 10 pixels per em wraps after "AB" (10 pixels) in a 12-pixel width; End alignment puts "AB" at x 2 and "A"
			// at x 7; the two-line block, 21 pixels high, is centred in 30 pixels: 4.5 from the top.
			UiDrawItem text = MakeText(glm::vec2(100.0f, 50.0f), glm::vec2(112.0f, 80.0f), "AB A");
			text.HorizontalAlignment = UiDrawAlignment::End;
			text.VerticalAlignment = UiDrawAlignment::Center;
			list.Items.push_back(text);
			const UiPassPlan plan = PlanUiPass(list, assets);
			REQUIRE(plan.Glyphs.size() == 3);
			REQUIRE(plan.Runs.size() == 1);
			CHECK(plan.Runs[0].Kind == UiPassRunKind::Glyphs);
			CHECK(plan.Runs[0].Font == TestFont);
			// 'A' of the first line: x 100 + 2; its box from 7 pixels above the baseline (0.7 em) at 8 (0.8 em) from the block's top.
			CHECK(plan.Glyphs[0].Min.x == doctest::Approx(102.0f));
			CHECK(plan.Glyphs[0].Min.y == doctest::Approx(55.5f));
			CHECK(plan.Glyphs[0].Max.y == doctest::Approx(62.5f));
			// 'A' of the second line, 11 pixels lower (the 1.1 em line pitch), ending at the right edge.
			CHECK(plan.Glyphs[2].Min.x == doctest::Approx(107.0f));
			CHECK(plan.Glyphs[2].Min.y == doctest::Approx(66.5f));
			CHECK(plan.Glyphs[2].AtlasMax == glm::vec2(0.5f));
		}

		TEST_CASE("UiPass: runs split where the kind, the clip, the font or the colour changes, in draw order")
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			UiDrawList list;
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(4.0f)));
			list.Items.push_back(MakeImage(glm::vec2(4.0f), glm::vec2(8.0f)));
			UiDrawItem clipped = MakeImage(glm::vec2(8.0f), glm::vec2(12.0f));
			clipped.Clipped = true;
			clipped.Clip = UiClipRect{ .MinX = 0, .MinY = 0, .MaxX = 10, .MaxY = 10 };
			list.Items.push_back(clipped);
			list.Items.push_back(MakeText(glm::vec2(0.0f), glm::vec2(40.0f, 20.0f), "A"));
			list.Items.push_back(MakeText(glm::vec2(0.0f, 20.0f), glm::vec2(40.0f), "B"));
			list.Items.push_back(MakeText(glm::vec2(0.0f, 40.0f), glm::vec2(40.0f, 60.0f), "A", glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)));
			list.Items.push_back(MakeImage(glm::vec2(12.0f), glm::vec2(16.0f)));
			const UiPassPlan plan = PlanUiPass(list, assets);
			REQUIRE(plan.Runs.size() == 5);
			CHECK(plan.Runs[0] == UiPassRun{ .Kind = UiPassRunKind::Quads, .First = 0, .Count = 2 });
			CHECK(plan.Runs[1] == UiPassRun{ .Kind = UiPassRunKind::Quads, .First = 2, .Count = 1, .Clipped = true, .Clip = clipped.Clip });
			CHECK(plan.Runs[2] == UiPassRun{ .Kind = UiPassRunKind::Glyphs, .First = 0, .Count = 2, .Font = TestFont });
			CHECK(plan.Runs[3] == UiPassRun{ .Kind = UiPassRunKind::Glyphs, .First = 2, .Count = 1, .Font = TestFont, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) });
			CHECK(plan.Runs[4] == UiPassRun{ .Kind = UiPassRunKind::Quads, .First = 3, .Count = 1 });
			CHECK(plan.DrawnItems == 7);
		}

		TEST_CASE("UiPass: annotations draw after the items as one-pixel outlines and labels")
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			UiDrawList list;
			list.Items.push_back(MakeImage(glm::vec2(0.0f), glm::vec2(4.0f)));
			list.Annotations.push_back(UiAnnotation{ .Min = glm::vec2(2.0f), .Max = glm::vec2(6.0f, 5.0f), .Color = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), .Outline = true });
			list.Annotations.push_back(UiAnnotation{ .Min = glm::vec2(2.0f, 20.0f), .Max = glm::vec2(6.0f, 25.0f), .Label = "AB" });
			const UiPassPlan plan = PlanUiPass(list, assets);
			// The item's quad, then the outline's top and bottom rows and its left and right columns between them.
			REQUIRE(plan.Quads.size() == 5);
			CHECK(plan.Quads[1].Min == glm::vec2(2.0f, 2.0f));
			CHECK(plan.Quads[1].Max == glm::vec2(6.0f, 3.0f));
			CHECK(plan.Quads[2].Min == glm::vec2(2.0f, 4.0f));
			CHECK(plan.Quads[2].Max == glm::vec2(6.0f, 5.0f));
			CHECK(plan.Quads[3].Min == glm::vec2(2.0f, 3.0f));
			CHECK(plan.Quads[3].Max == glm::vec2(3.0f, 4.0f));
			CHECK(plan.Quads[4].Min == glm::vec2(5.0f, 3.0f));
			CHECK(plan.Quads[4].Max == glm::vec2(6.0f, 4.0f));
			CHECK(plan.Quads[4].Color == glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
			// The label in the Default font, its block's bottom-left corner at the rectangle's top-left corner, unclipped.
			REQUIRE(plan.Runs.size() == 2);
			CHECK(plan.Runs[1].Kind == UiPassRunKind::Glyphs);
			CHECK_FALSE(plan.Runs[1].Clipped);
			CHECK_FALSE(plan.Runs[1].Font.IsValid());
			REQUIRE(plan.Glyphs.size() == 2);
			CHECK(plan.Glyphs[0].Min.x == doctest::Approx(2.0f));
			CHECK(plan.Glyphs[0].Max.y <= 20.0f);
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 42; stream F): the font chain.
		TEST_CASE("UiPass: a text's font chain follows its fallbacks depth first, each font once, and leaves out a missing one"
			* doctest::skip(true))
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			// A falls back to B, a missing font and C; B to D and back to A. The chain of A: A, B, D, C.
			constexpr AssetHandle FontA{ 0xA0 }, FontB{ 0xB0 }, FontC{ 0xC0 }, FontD{ 0xD0 }, Missing{ 0xDEAD };
			const auto publish = [&assets](AssetHandle handle, std::vector<AssetHandle> fallbacks)
			{
				Ref<FontData> font = MakeFont();
				font->Fallbacks = std::move(fallbacks);
				assets.Publish(handle, font);
			};
			Test::ExpectLog missing(LogLevel::Error, std::string(AssetMissingCode));
			publish(FontA, { FontB, Missing, FontC });
			publish(FontB, { FontD, FontA });
			publish(FontC, {});
			publish(FontD, {});
			CHECK(BuildUiFontChain(FontA, assets) == std::vector<AssetHandle>{ FontA, FontB, FontD, FontC });
			CHECK(BuildUiFontChain(FontC, assets) == std::vector<AssetHandle>{ FontC });
			// A null font is the Default font's chain; a chain stops at MaxUiFontChainLength fonts.
			CHECK(BuildUiFontChain({}, assets) == std::vector<AssetHandle>{ BuiltinAssetHandles::DefaultFont });
			std::vector<AssetHandle> many;
			for (uint64_t index = 1; index <= 10; ++index)
			{
				many.push_back(AssetHandle(0x100 + index));
				publish(many.back(), {});
			}
			publish(FontA, many);
			CHECK(BuildUiFontChain(FontA, assets).size() == MaxUiFontChainLength);
			// The missing fallback was reported once, however many chains met it.
			CHECK(std::ranges::count_if(assets.GetDiagnostics(), [](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == AssetHandle(0xDEAD) && diagnostic.Code == AssetMissingCode;
			}) == 1);
			CHECK(missing.GetMatchCount() == 1);
		}

		TEST_CASE("UiPass: a codepoint the text's font lacks falls back to the first font of its chain that has it" * doctest::skip(true))
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			// "Latin" has A and the space; "Extra" has B. A text in Latin falling back to Extra draws "AB A" from both.
			constexpr AssetHandle Latin{ 0x1A7 }, Extra{ 0xE77 };
			Ref<FontData> latin = MakeFont();
			std::erase_if(latin->Glyphs, [](const FontGlyph& glyph)
			{
				return glyph.Codepoint == 'B';
			});
			latin->Kerning.clear();
			latin->Fallbacks = { Extra };
			Ref<FontData> extra = MakeFont();
			std::erase_if(extra->Glyphs, [](const FontGlyph& glyph)
			{
				return glyph.Codepoint != 'B';
			});
			extra->Kerning.clear();
			assets.Publish(Latin, latin);
			assets.Publish(Extra, extra);
			UiDrawList list;
			UiDrawItem text = MakeText(glm::vec2(0.0f), glm::vec2(100.0f, 20.0f), "AB A");
			text.Font = Latin;
			list.Items.push_back(text);
			const UiPassPlan plan = PlanUiPass(list, assets);
			REQUIRE(plan.Glyphs.size() == 3);
			// Runs split where the glyph's font changes: A (Latin), B (Extra), A (Latin); B starts at A's unkerned 5 pixels.
			REQUIRE(plan.Runs.size() == 3);
			CHECK(plan.Runs[0].Font == Latin);
			CHECK(plan.Runs[1].Font == Extra);
			CHECK(plan.Runs[2].Font == Latin);
			CHECK(plan.Glyphs[1].Min.x == doctest::Approx(5.5f));
		}

		// C22.3 (decision 48; streams T and S): the text input's caret.
		TEST_CASE("UiPass: a focused text input's caret stands before its byte and keeps inside the rectangle" * doctest::skip(true))
		{
			Test::InMemoryAssetManager assets;
			PublishUiAssets(assets);
			UiDrawList list;
			// "AB A" at 10 pixels per em in (100, 50)-(140, 70): the caret before byte 1 is at x 104 (A's 5 less the kerning's
			// 1), one pixel wide (floor(10 / 16 + 0.5)), the line's 10 pixels high.
			UiDrawItem field = MakeText(glm::vec2(100.0f, 50.0f), glm::vec2(140.0f, 70.0f), "AB A");
			field.Wrap = false;
			field.HasCaret = true;
			field.CaretByte = 1;
			list.Items.push_back(field);
			UiPassPlan plan = PlanUiPass(list, assets);
			REQUIRE(plan.Quads.size() == 1);
			CHECK(plan.Quads[0].Min == glm::vec2(104.0f, 50.0f));
			CHECK(plan.Quads[0].Max == glm::vec2(105.0f, 60.0f));
			CHECK_FALSE(plan.Quads[0].Texture.IsValid());
			REQUIRE(plan.Runs.size() == 2);
			CHECK(plan.Runs[0].Kind == UiPassRunKind::Glyphs);
			CHECK(plan.Runs[1].Kind == UiPassRunKind::Quads);

			// A text wider than the rectangle (40 pixels) with the caret at its end moves left by the least that puts the caret
			// inside: the caret's right edge lands on the rectangle's right edge, 140, and the first glyph left of 100.
			list.Items[0].Text = "ABABABAB";
			list.Items[0].CaretByte = 8;
			plan = PlanUiPass(list, assets);
			REQUIRE(plan.Quads.size() == 1);
			CHECK(plan.Quads[0].Max.x == 140.0f);
			CHECK(plan.Glyphs.front().Min.x < 100.0f);
			// The caret at the start moves the block back so its left is the rectangle's left.
			list.Items[0].CaretByte = 0;
			plan = PlanUiPass(list, assets);
			CHECK(plan.Quads[0].Min.x == 100.0f);
			CHECK(plan.Glyphs.front().Min.x == doctest::Approx(100.0f));

			// Right-aligned, "ABABABABAB" (50 pixels) starts 10 pixels left of the rectangle: the caret at its start moves the
			// block right by the least that puts the caret inside, so the text starts at the rectangle's left (the review of
			// C22.3).
			list.Items[0].Text = "ABABABABAB";
			list.Items[0].HorizontalAlignment = UiDrawAlignment::End;
			plan = PlanUiPass(list, assets);
			REQUIRE(plan.Quads.size() == 1);
			CHECK(plan.Quads[0].Min.x == 100.0f);
			CHECK(plan.Glyphs.front().Min.x == doctest::Approx(100.0f));

			// An empty focused field draws only its caret, aligned as one line of LineHeight (10 pixels) in the 20-pixel
			// rectangle: in the middle, then at the bottom, where its first character's line will stand.
			list.Items[0].Text.clear();
			list.Items[0].CaretByte = 0;
			list.Items[0].HorizontalAlignment = UiDrawAlignment::Start;
			list.Items[0].VerticalAlignment = UiDrawAlignment::Center;
			plan = PlanUiPass(list, assets);
			CHECK(plan.Glyphs.empty());
			REQUIRE(plan.Quads.size() == 1);
			CHECK(plan.Quads[0].Min == glm::vec2(100.0f, 55.0f));
			CHECK(plan.Quads[0].Max == glm::vec2(101.0f, 65.0f));
			list.Items[0].VerticalAlignment = UiDrawAlignment::End;
			plan = PlanUiPass(list, assets);
			REQUIRE(plan.Quads.size() == 1);
			CHECK(plan.Quads[0].Min == glm::vec2(100.0f, 60.0f));
			CHECK(plan.Quads[0].Max == glm::vec2(101.0f, 70.0f));
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("UiPass: a 1:1 UI image reads back its source texels pixel-exact")
		{
			// Roadmap M22's acceptance: frame 0 (8 x 8 texels) drawn over [6, 14) x [6, 14) with the point sampler gives each
			// pixel its texel's exact bytes, under both API caps (the gpu suite runs every case under 1.4 and 1.3).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 32, 32);
			PublishUiAssets(setup.Assets());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			snapshot.Ui.Items.push_back(MakeImage(glm::vec2(6.0f), glm::vec2(14.0f), Sheet, 0));
			const Image image = setup.Render(snapshot);
			for (uint32_t y = 6; y < 14; ++y)
			{
				for (uint32_t x = 6; x < 14; ++x)
				{
					const glm::u8vec4 expected = MakeTexel(8 + x - 6, y - 6);
					for (size_t channel = 0; channel < 3; ++channel)
						CHECK(Test::SceneRendererIntegrationFixture::Channel(image, x, y, channel) == expected[static_cast<glm::length_t>(channel)]);
				}
			}
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* ui = Test::SceneRendererIntegrationFixture::FindPass(stats, "Ui");
			REQUIRE(ui != nullptr);
			CHECK(ui->DrawCalls >= 1);
		}

		TEST_CASE("UiPass: a clipped element leaves the pixels outside its clip untouched")
		{
			// The scissor oracle: the same render without the item gives every pixel outside the clip; inside it the texels.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 32, 32);
			PublishUiAssets(setup.Assets());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			const Image without = setup.Render(snapshot);
			UiDrawItem item = MakeImage(glm::vec2(6.0f), glm::vec2(14.0f), Sheet, 0);
			item.Clipped = true;
			item.Clip = UiClipRect{ .MinX = 9, .MinY = -4, .MaxX = 40, .MaxY = 10 };
			snapshot.Ui.Items.push_back(item);
			snapshot.FrameIndex += 1;
			const Image with = setup.Render(snapshot);
			for (uint32_t y = 0; y < 32; ++y)
			{
				for (uint32_t x = 0; x < 32; ++x)
				{
					const bool inside = x >= 9 && x < 14 && y >= 6 && y < 10;
					CAPTURE(x);
					CAPTURE(y);
					if (inside)
						CHECK(Test::SceneRendererIntegrationFixture::Channel(with, x, y, 0) == MakeTexel(8 + x - 6, y - 6).r);
					else
						CHECK(Test::SceneRendererIntegrationFixture::Channel(with, x, y, 0) == Test::SceneRendererIntegrationFixture::Channel(without, x, y, 0));
				}
			}
		}

		TEST_CASE("UiPass: a view without a camera draws its UI, and the data views draw none")
		{
			// The UI is in scene pixels, so a game view without a primary camera (a menu scene) draws it, as it draws its screen
			// texts; the data views show no UI, as they show no Text, and record no "Ui" row.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 32, 32);
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			snapshot.HasCamera = false;
			UiDrawItem red = MakeImage(glm::vec2(4.0f), glm::vec2(12.0f));
			red.Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			snapshot.Ui.Items.push_back(red);
			const Image menu = setup.Render(snapshot);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(menu, 8, 8, 0) == 255);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(menu, 8, 8, 1) == 0);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(menu, 20, 20, 0) != 255);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "Ui") != nullptr);

			// The AO view without GTAO is white everywhere: a black UI image leaves it so.
			RenderSnapshot ao = setup.Snapshot();
			ao.DebugView = RenderDebugView::AO;
			ao.FrameIndex += 1;
			UiDrawItem black = MakeImage(glm::vec2(0.0f), glm::vec2(32.0f));
			black.Color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			ao.Ui.Items.push_back(black);
			const Image image = setup.Render(ao);
			for (size_t pixel = 0; pixel < image.Pixels.size(); pixel += 4)
				CHECK(image.Pixels[pixel] == std::byte{ 255 });
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "Ui") == nullptr);
		}

		TEST_CASE("UiPass: at scale 3 the UI is upscaled with the scene image, its row between Text and Upscale")
		{
			// A 16 x 16 scene in a 48 x 48 view: the frame drawn 1:1 at scene pixels [2, 10) shows as 3 x 3 blocks of its texels.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 48, 48);
			PublishUiAssets(setup.Assets());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			snapshot.Output = RenderOutputSettings{ .SceneWidth = 16, .SceneHeight = 16, .PixelArt = true };
			snapshot.Ui.Items.push_back(MakeImage(glm::vec2(2.0f), glm::vec2(10.0f), Sheet, 0));
			const Image image = setup.Render(snapshot);
			for (uint32_t y = 6; y < 30; ++y)
			{
				for (uint32_t x = 6; x < 30; ++x)
					CHECK(Test::SceneRendererIntegrationFixture::Channel(image, x, y, 0) == MakeTexel(8 + (x - 6) / 3, (y - 6) / 3).r);
			}
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const auto& passes = stats.Passes;
			const auto ui = std::find_if(passes.begin(), passes.end(), [](const auto& pass)
			{
				return pass.Name == "Ui";
			});
			const auto upscale = std::find_if(passes.begin(), passes.end(), [](const auto& pass)
			{
				return pass.Name == "Upscale";
			});
			REQUIRE(ui != passes.end());
			REQUIRE(upscale != passes.end());
			CHECK(ui < upscale);
		}

		TEST_CASE("UiPass: a device loss recreates nothing of the pass's own and the UI draws the same image afterwards")
		{
			// The recreation protocol (C18.3): the pass owns no GPU object; its QuadBatcher and TextRenderer are
			// SceneRendererPipelines', its binding caches the SceneRenderer's, so those owners' recovery covers it.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 32, 32);
			PublishUiAssets(setup.Assets());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			UiDrawItem sliced = MakeImage(glm::vec2(2.0f), glm::vec2(30.0f, 20.0f), Sheet, 1);
			sliced.ImageType = UiDrawImageType::Sliced;
			snapshot.Ui.Items.push_back(sliced);
			Test::DeviceLossTest test;
			test.Owners = { "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&gpu, &setup, &snapshot]() -> Result<std::vector<std::byte>>
			{
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, gpu.GetDevice().CreateCommandList());
				list->open();
				const Status rendered = setup.Renderer().Render(*list, snapshot);
				list->close();
				setup.Renderer().OnSubmitted(snapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(*list));
				++snapshot.FrameIndex;
				ENGINE_TRY(rendered);
				nvrhi::ITexture* image = setup.Renderer().GetFinalTexture();
				if (image == nullptr)
					return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
				Readback readback(gpu.GetDevice());
				ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*image));
				return pixels.Pixels;
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// The UI was drawn: the sliced frame's top-left corner patch, [2, 4) x [2, 4) at one pixel per texel, shows texel
			// (1, 1) of frame 1 at pixel (3, 3).
			const size_t texel = (static_cast<size_t>(3) * 32 + 3) * 4;
			REQUIRE(report->Before.size() > texel);
			CHECK(std::to_integer<int>(report->Before[texel]) == MakeTexel(1, 1).r);
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 51; stream W): World canvases in the world.
		TEST_CASE("UiPass: a World canvas is depth-tested against the scene and drawn under the Screen canvases" * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 32, 32);
			PublishUiAssets(setup.Assets());
			// The fixture's cube at z = -3 in front of a red World canvas of 64 x 64 canvas pixels covering [-4, 4] x [-4, 4]
			// at z = -6 (beyond the view's edges), and a green Screen image over the image's centre [14, 18).
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Ui.WorldCanvases.push_back(UiWorldCanvas{ .Canvas = UUID(0xB0A2D),
				.Size = glm::uvec2(64),
				.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.125f, -0.125f, 1.0f))
					* glm::translate(glm::mat4(1.0f), glm::vec3(-32.0f, -32.0f, 0.0f)) });
			UiDrawItem board = MakeImage(glm::vec2(0.0f), glm::vec2(64.0f));
			board.Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			board.WorldCanvas = 0;
			snapshot.Ui.Items.push_back(board);
			UiDrawItem overlay = MakeImage(glm::vec2(14.0f), glm::vec2(18.0f));
			overlay.Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			snapshot.Ui.Items.push_back(overlay);
			const Image image = setup.Render(snapshot);
			// The corner shows the canvas; the cube hides it beside the overlay; the overlay is on top of both.
			CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 1, 1, 0) == 255);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 1, 1, 1) == 0);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 12, 16, 0) != 255);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 16, 16, 1) == 255);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 16, 16, 0) == 0);
			// A view without a camera draws no World canvas, only the overlay.
			snapshot.HasCamera = false;
			const Image cameraless = setup.Render(snapshot);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(cameraless, 1, 1, 0) == 0);
			CHECK(Test::SceneRendererIntegrationFixture::Channel(cameraless, 16, 16, 1) == 255);
		}

		TEST_CASE("UiPass: a device loss recreates the world canvases' pipeline and a World canvas draws the same image afterwards"
			* doctest::skip(true))
		{
			// The recreation protocol (C18.3): the depth-tested pipeline World canvases draw with is SceneRendererPipelines'
			// (the QuadBatcher's LdrDepth target, decision 57's ledger amendment, granted on main in 1707cf47), so its owner's
			// recovery covers it.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, 32, 32);
			PublishUiAssets(setup.Assets());
			RenderSnapshot snapshot = setup.Snapshot();
			snapshot.Meshes.clear();
			snapshot.Ui.WorldCanvases.push_back(UiWorldCanvas{ .Canvas = UUID(0xB0A2D),
				.Size = glm::uvec2(64),
				.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.125f, -0.125f, 1.0f))
					* glm::translate(glm::mat4(1.0f), glm::vec3(-32.0f, -32.0f, 0.0f)) });
			UiDrawItem board = MakeImage(glm::vec2(0.0f), glm::vec2(64.0f), Sheet, 0);
			board.WorldCanvas = 0;
			snapshot.Ui.Items.push_back(board);
			Test::DeviceLossTest test;
			test.Owners = { "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&gpu, &setup, &snapshot]() -> Result<std::vector<std::byte>>
			{
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, gpu.GetDevice().CreateCommandList());
				list->open();
				const Status rendered = setup.Renderer().Render(*list, snapshot);
				list->close();
				setup.Renderer().OnSubmitted(snapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(*list));
				++snapshot.FrameIndex;
				ENGINE_TRY(rendered);
				nvrhi::ITexture* image = setup.Renderer().GetFinalTexture();
				if (image == nullptr)
					return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
				Readback readback(gpu.GetDevice());
				ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*image));
				return pixels.Pixels;
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// The canvas was drawn: its sheet's frame covers the whole view, so no pixel keeps the black clear colour.
			REQUIRE(report->Before.size() >= 4);
			CHECK(std::to_integer<int>(report->Before[0]) + std::to_integer<int>(report->Before[1]) + std::to_integer<int>(report->Before[2]) > 0);
		}
	}

}
