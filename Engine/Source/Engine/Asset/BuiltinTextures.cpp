#include "EnginePCH.h"
#include "Engine/Asset/BuiltinTextures.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace Engine {

	namespace {

		// One RGBA8 texel.
		struct Texel
		{
			uint8_t R = 0;
			uint8_t G = 0;
			uint8_t B = 0;
			uint8_t A = 255;
		};

		// The sRGB decoding of every 8-bit value as linear 0..65535: round(65535 * EOTF(i / 255)) with the sRGB EOTF
		// (i / 255 / 12.92 up to 0.04045, ((i / 255 + 0.055) / 1.055)^2.4 above), evaluated once with exact decimal
		// arithmetic and rounded half to even. Strictly increasing, so the nearest entry encodes a linear value back. A table
		// instead of std::pow keeps the mips independent of the C runtime (BuiltinTextures.h). Row r holds bytes 16r to
		// 16r + 15.
		// clang-format off
		constexpr std::array<uint16_t, 256> SrgbToLinear = {
			0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219, 241, 264, 288, 313,
			340, 367, 396, 427, 458, 491, 526, 562, 599, 637, 677, 718, 761, 805, 851, 898,
			947, 997, 1048, 1101, 1156, 1212, 1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863,
			1937, 2013, 2090, 2170, 2250, 2333, 2418, 2504, 2592, 2681, 2773, 2866, 2961, 3058, 3157, 3258,
			3360, 3464, 3570, 3678, 3788, 3900, 4014, 4129, 4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124,
			5257, 5392, 5530, 5669, 5810, 5953, 6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500,
			7666, 7834, 8004, 8177, 8352, 8528, 8708, 8889, 9072, 9258, 9445, 9635, 9828, 10022, 10219, 10417,
			10619, 10822, 11028, 11235, 11446, 11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909,
			14146, 14387, 14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456, 17727, 18001,
			18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177, 21481, 21787, 22096, 22407, 22721,
			23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325, 25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094,
			28452, 28813, 29176, 29542, 29911, 30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143,
			34544, 34948, 35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449, 40891,
			41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423, 46903, 47385, 47871, 48359,
			48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884, 53401, 53921, 54445, 54971, 55500, 56032, 56567,
			57105, 57646, 58190, 58737, 59287, 59840, 60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
		};
		// clang-format on

		// The side of the solid colour textures and of the checkers, and the side of one checker square, in texels.
		constexpr uint32_t SolidSize = 4;
		constexpr uint32_t CheckerSize = 64;
		constexpr uint32_t CheckerSquare = 8;

		constexpr Texel White = { 255, 255, 255, 255 };
		constexpr Texel Black = { 0, 0, 0, 255 };
		// The tangent-space normal (0, 0, 1) encoded as n * 0.5 + 0.5 (128 is the nearest byte to 127.5).
		constexpr Texel FlatNormal = { 128, 128, 255, 255 };
		constexpr Texel LightGrey = { 192, 192, 192, 255 };
		constexpr Texel DarkGrey = { 128, 128, 128, 255 };
		constexpr Texel Magenta = { 255, 0, 255, 255 };

	}

	namespace Utils {

		// The byte whose sRGB decoding is nearest to `linear` (0..65535); a tie goes to the smaller byte.
		static uint8_t EncodeSrgb(uint32_t linear)
		{
			const auto upper = std::ranges::lower_bound(SrgbToLinear, linear);
			if (upper == SrgbToLinear.begin())
				return 0;
			if (upper == SrgbToLinear.end())
				return 255;
			const size_t index = static_cast<size_t>(upper - SrgbToLinear.begin());
			const uint32_t distanceBelow = linear - static_cast<uint32_t>(SrgbToLinear[index - 1]);
			const uint32_t distanceAbove = static_cast<uint32_t>(SrgbToLinear[index]) - linear;
			return static_cast<uint8_t>(distanceBelow <= distanceAbove ? index - 1 : index);
		}

		// The rounded mean of four channel values: of their sRGB decodings, encoded again, for the colour channels of an
		// sRGB texture; of the bytes themselves otherwise.
		static uint8_t AverageChannel(std::span<const uint8_t, 4> values, bool isSrgb)
		{
			if (isSrgb)
			{
				uint32_t sum = 0;
				for (const uint8_t value : values)
					sum += SrgbToLinear[value];
				return EncodeSrgb((sum + 2) / 4);
			}
			uint32_t sum = 0;
			for (const uint8_t value : values)
				sum += value;
			return static_cast<uint8_t>((sum + 2) / 4);
		}

		// Appends one level of `width` x `height` RGBA8 texels to `texture`.
		static void AppendLevel(TextureData& texture, uint32_t width, uint32_t height, std::span<const Texel> texels)
		{
			const uint64_t offset = texture.Pixels.size();
			texture.Pixels.reserve(texture.Pixels.size() + texels.size() * 4);
			for (const Texel& texel : texels)
			{
				texture.Pixels.push_back(static_cast<std::byte>(texel.R));
				texture.Pixels.push_back(static_cast<std::byte>(texel.G));
				texture.Pixels.push_back(static_cast<std::byte>(texel.B));
				texture.Pixels.push_back(static_cast<std::byte>(texel.A));
			}
			texture.Mips.push_back({ .Width = width, .Height = height, .Offset = offset, .Size = texture.Pixels.size() - offset });
		}

		// The next smaller level: every texel the exact 2x2 box mean of the texels it covers (an odd edge repeats its last
		// row or column). Alpha is always averaged linearly.
		static std::vector<Texel> Downsample(std::span<const Texel> texels, uint32_t width, uint32_t height, bool isSrgb)
		{
			const uint32_t smallWidth = std::max(1u, width / 2);
			const uint32_t smallHeight = std::max(1u, height / 2);
			std::vector<Texel> result(static_cast<size_t>(smallWidth) * smallHeight);
			for (uint32_t y = 0; y < smallHeight; ++y)
			{
				for (uint32_t x = 0; x < smallWidth; ++x)
				{
					const uint32_t left = std::min(2 * x, width - 1);
					const uint32_t right = std::min(2 * x + 1, width - 1);
					const uint32_t top = std::min(2 * y, height - 1);
					const uint32_t bottom = std::min(2 * y + 1, height - 1);
					const std::array<Texel, 4> corners = {
						texels[static_cast<size_t>(top) * width + left],
						texels[static_cast<size_t>(top) * width + right],
						texels[static_cast<size_t>(bottom) * width + left],
						texels[static_cast<size_t>(bottom) * width + right],
					};
					const auto average = [&corners](uint8_t Texel::* channel, bool isColour)
					{
						const std::array<uint8_t, 4> values = { corners[0].*channel, corners[1].*channel, corners[2].*channel, corners[3].*channel };
						return AverageChannel(values, isColour);
					};
					result[static_cast<size_t>(y) * smallWidth + x] = {
						average(&Texel::R, isSrgb),
						average(&Texel::G, isSrgb),
						average(&Texel::B, isSrgb),
						average(&Texel::A, false),
					};
				}
			}
			return result;
		}

		// A SolidSize x SolidSize texture of one colour, base level only.
		static TextureData CreateSolid(TextureFormat format, const Texel& colour)
		{
			TextureData texture;
			texture.Format = format;
			texture.Width = SolidSize;
			texture.Height = SolidSize;
			const std::vector<Texel> texels(static_cast<size_t>(SolidSize) * SolidSize, colour);
			AppendLevel(texture, SolidSize, SolidSize, texels);
			return texture;
		}

		// A CheckerSize x CheckerSize sRGB checkerboard of CheckerSquare-texel squares, `first` in the top-left square, with
		// its full mip chain.
		static TextureData CreateChecker(const Texel& first, const Texel& second)
		{
			TextureData texture;
			texture.Format = TextureFormat::RGBA8Srgb;
			texture.Width = CheckerSize;
			texture.Height = CheckerSize;
			std::vector<Texel> level(static_cast<size_t>(CheckerSize) * CheckerSize);
			for (uint32_t y = 0; y < CheckerSize; ++y)
			{
				for (uint32_t x = 0; x < CheckerSize; ++x)
					level[static_cast<size_t>(y) * CheckerSize + x] = ((x / CheckerSquare + y / CheckerSquare) % 2 == 0) ? first : second;
			}

			uint32_t width = CheckerSize;
			uint32_t height = CheckerSize;
			AppendLevel(texture, width, height, level);
			const uint32_t levelCount = ComputeFullMipCount(CheckerSize, CheckerSize);
			for (uint32_t mip = 1; mip < levelCount; ++mip)
			{
				level = Downsample(level, width, height, true);
				width = std::max(1u, width / 2);
				height = std::max(1u, height / 2);
				AppendLevel(texture, width, height, level);
			}
			return texture;
		}

		static TextureData CreateTexture(BuiltinTexture texture)
		{
			switch (texture)
			{
				case BuiltinTexture::White:      return CreateSolid(TextureFormat::RGBA8Srgb, White);
				case BuiltinTexture::Black:      return CreateSolid(TextureFormat::RGBA8Srgb, Black);
				case BuiltinTexture::FlatNormal: return CreateSolid(TextureFormat::RGBA8Unorm, FlatNormal);
				case BuiltinTexture::Checker:    return CreateChecker(LightGrey, DarkGrey);
				case BuiltinTexture::Missing:    return CreateChecker(Magenta, Black);
			}
			ENGINE_CORE_ASSERT(false, "Unknown BuiltinTexture {}", std::to_underlying(texture));
			return CreateChecker(Magenta, Black);
		}

	}

	std::string_view BuiltinTextureToString(BuiltinTexture texture)
	{
		switch (texture)
		{
			case BuiltinTexture::White:      return "White";
			case BuiltinTexture::Black:      return "Black";
			case BuiltinTexture::FlatNormal: return "FlatNormal";
			case BuiltinTexture::Checker:    return "Checker";
			case BuiltinTexture::Missing:    return "Missing";
		}
		return "Unknown";
	}

	TextureData GenerateBuiltinTexture(BuiltinTexture texture)
	{
		TextureData generated = Utils::CreateTexture(texture);
		ENGINE_CORE_ASSERT(ValidateTextureData(generated).has_value(), "the built-in texture '{}' fails ValidateTextureData",
			BuiltinTextureToString(texture));
		return generated;
	}

}
