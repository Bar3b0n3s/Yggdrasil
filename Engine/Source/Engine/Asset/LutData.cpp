#include "EnginePCH.h"
#include "Engine/Asset/LutData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The LUT's formats (LutData.h; Docs/Decisions/0037-m26-contract.md decision 15): the payload, the validation, the
// first-party `.cube` parser and the built-in LUTs. The artifact wrappers and the built-in names are the contract's.

namespace Engine {

	static_assert(sizeof(glm::vec3) == 12, "a LUT entry is three packed floats in the payload");

	namespace Utils {

		// The entries a table of `size` has.
		[[nodiscard]] static size_t GetLutEntryCount(uint32_t size)
		{
			return static_cast<size_t>(size) * size * size;
		}

		// The identity table of `size`: entry (r, g, b) = (r, g, b) / (size - 1), each an exact quotient for the sizes the
		// built-ins use (33: a power-of-two divisor), over the domain [0, 1].
		[[nodiscard]] static LutData MakeIdentityLut(uint32_t size)
		{
			LutData lut;
			lut.Size = size;
			lut.Values.reserve(GetLutEntryCount(size));
			const auto last = static_cast<float>(size - 1);
			for (uint32_t blue = 0; blue < size; ++blue)
			{
				for (uint32_t green = 0; green < size; ++green)
				{
					for (uint32_t red = 0; red < size; ++red)
						lut.Values.emplace_back(static_cast<float>(red) / last, static_cast<float>(green) / last, static_cast<float>(blue) / last);
				}
			}
			return lut;
		}

		// A Parse error located at `line` (1-based) of a `.cube` file.
		[[nodiscard]] static std::unexpected<Error> MakeCubeError(ErrorCode code, uint32_t line, std::string message)
		{
			ErrorLocation location;
			location.Line = line;
			return std::unexpected(Error(code, std::move(message)).WithLocation(std::move(location)));
		}

		[[nodiscard]] static bool IsCubeSpace(char character)
		{
			return character == ' ' || character == '\t';
		}

		// The whitespace-separated tokens of `line`.
		[[nodiscard]] static std::vector<std::string_view> SplitCubeTokens(std::string_view line)
		{
			std::vector<std::string_view> tokens;
			size_t position = 0;
			while (position < line.size())
			{
				while (position < line.size() && IsCubeSpace(line[position]))
					++position;
				const size_t start = position;
				while (position < line.size() && !IsCubeSpace(line[position]))
					++position;
				if (position > start)
					tokens.push_back(line.substr(start, position - start));
			}
			return tokens;
		}

		// Whether `token` starts a data line: a number's first character (a keyword starts with a letter).
		[[nodiscard]] static bool IsCubeNumberStart(std::string_view token)
		{
			const char first = token.front();
			return (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
		}

		// `token` as a finite float, read whole with the locale-independent from_chars; nullopt for anything else (a
		// malformed or out-of-range number, an infinity or a NaN).
		[[nodiscard]] static std::optional<float> ParseCubeFloat(std::string_view token)
		{
			float value = 0.0f;
			const std::from_chars_result parsed = std::from_chars(token.data(), token.data() + token.size(), value);
			if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size() || !std::isfinite(value))
				return std::nullopt;
			return value;
		}

		// The three floats after a keyword or on a data line, `tokens` starting at `first`.
		[[nodiscard]] static Result<glm::vec3> ParseCubeTriple(std::span<const std::string_view> tokens, uint32_t line, std::string_view what)
		{
			if (tokens.size() != 3)
				return MakeCubeError(ErrorCode::Parse, line, std::format("{} needs three values, found {}", what, tokens.size()));
			glm::vec3 values{ 0.0f };
			for (glm::length_t channel = 0; channel < 3; ++channel)
			{
				const std::optional<float> value = ParseCubeFloat(tokens[static_cast<size_t>(channel)]);
				if (!value.has_value())
				{
					return MakeCubeError(ErrorCode::Parse, line,
						std::format("'{}' in {} is not a finite number", tokens[static_cast<size_t>(channel)], what));
				}
				values[channel] = *value;
			}
			return values;
		}

		// What the keyword lines of a `.cube` file set before its first data line.
		struct CubeHeader
		{
			std::optional<uint32_t> Size{};
			std::optional<glm::vec3> DomainMin{};
			std::optional<glm::vec3> DomainMax{};
			bool HasTitle = false;
			bool HasInputRange = false; // LUT_3D_INPUT_RANGE set DomainMin and DomainMax
		};

		// Applies the keyword line `tokens` (tokens[0] the keyword) to `header`. The keywords are TITLE, LUT_3D_SIZE,
		// DOMAIN_MIN, DOMAIN_MAX and LUT_3D_INPUT_RANGE, each at most once; LUT_3D_INPUT_RANGE min max (DaVinci Resolve's form
		// of the domain) sets DOMAIN_MIN to min min min and DOMAIN_MAX to max max max, so a file holds either it or the
		// DOMAIN_ keywords; LUT_1D_SIZE is Unsupported.
		[[nodiscard]] static Status ApplyCubeKeyword(CubeHeader& header, std::span<const std::string_view> tokens, uint32_t line)
		{
			const std::string_view keyword = tokens.front();
			const std::span<const std::string_view> arguments = tokens.subspan(1);
			const auto repeated = [line, keyword]()
			{
				return MakeCubeError(ErrorCode::Parse, line, std::format("{} appears a second time", keyword));
			};
			if (keyword == "TITLE")
			{
				if (header.HasTitle)
					return repeated();
				header.HasTitle = true;
				return {};
			}
			if (keyword == "LUT_1D_SIZE")
				return MakeCubeError(ErrorCode::Unsupported, line, "LUT_1D_SIZE: 1D tables are not supported; only 3D tables (LUT_3D_SIZE) grade an image");
			if (keyword == "LUT_3D_SIZE")
			{
				if (header.Size.has_value())
					return repeated();
				if (arguments.size() != 1)
					return MakeCubeError(ErrorCode::Parse, line, std::format("LUT_3D_SIZE needs one value, found {}", arguments.size()));
				const std::string_view token = arguments.front();
				uint32_t size = 0;
				const std::from_chars_result parsed = std::from_chars(token.data(), token.data() + token.size(), size);
				if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size())
					return MakeCubeError(ErrorCode::Parse, line, std::format("LUT_3D_SIZE '{}' is not a whole number", token));
				if (size < MinLutSize || size > MaxLutSize)
				{
					return MakeCubeError(ErrorCode::Parse, line,
						std::format("LUT_3D_SIZE {} is outside the sizes a LUT may have, {} to {}", size, MinLutSize, MaxLutSize));
				}
				header.Size = size;
				return {};
			}
			const auto domainTwice = [line, keyword]()
			{
				return MakeCubeError(ErrorCode::Parse, line,
					std::format("{} sets a domain the file sets already (LUT_3D_INPUT_RANGE and DOMAIN_MIN or DOMAIN_MAX)", keyword));
			};
			if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX")
			{
				if (header.HasInputRange)
					return domainTwice();
				std::optional<glm::vec3>& domain = keyword == "DOMAIN_MIN" ? header.DomainMin : header.DomainMax;
				if (domain.has_value())
					return repeated();
				ENGINE_TRY_ASSIGN(domain, ParseCubeTriple(arguments, line, keyword));
				return {};
			}
			if (keyword == "LUT_3D_INPUT_RANGE")
			{
				if (header.HasInputRange)
					return repeated();
				if (header.DomainMin.has_value() || header.DomainMax.has_value())
					return domainTwice();
				if (arguments.size() != 2)
					return MakeCubeError(ErrorCode::Parse, line, std::format("LUT_3D_INPUT_RANGE needs two values, found {}", arguments.size()));
				const std::optional<float> low = ParseCubeFloat(arguments[0]);
				const std::optional<float> high = ParseCubeFloat(arguments[1]);
				if (!low.has_value() || !high.has_value())
				{
					return MakeCubeError(ErrorCode::Parse, line,
						std::format("'{}' in LUT_3D_INPUT_RANGE is not a finite number", !low.has_value() ? arguments[0] : arguments[1]));
				}
				header.HasInputRange = true;
				header.DomainMin = glm::vec3(*low);
				header.DomainMax = glm::vec3(*high);
				return {};
			}
			return MakeCubeError(ErrorCode::Parse, line,
				std::format("unknown keyword '{}' (a .cube file has TITLE, LUT_3D_SIZE, DOMAIN_MIN, DOMAIN_MAX, LUT_3D_INPUT_RANGE and data lines)",
					keyword));
		}

	}

	Status ValidateLutData(const LutData& lut)
	{
		if (lut.Size < MinLutSize || lut.Size > MaxLutSize)
			return MakeError(ErrorCode::Validation, "the LUT's size is {}, outside the sizes a LUT may have, {} to {}", lut.Size, MinLutSize, MaxLutSize);
		for (glm::length_t channel = 0; channel < 3; ++channel)
		{
			const float low = lut.DomainMin[channel];
			const float high = lut.DomainMax[channel];
			if (!std::isfinite(low) || !std::isfinite(high))
				return MakeError(ErrorCode::Validation, "the LUT's domain is not finite in channel {}", channel);
			if (!(low < high))
				return MakeError(ErrorCode::Validation, "the LUT's domain minimum {} is not below its maximum {} in channel {}", low, high, channel);
		}
		const size_t expected = Utils::GetLutEntryCount(lut.Size);
		if (lut.Values.size() != expected)
			return MakeError(ErrorCode::Validation, "the LUT has {} entries; a table of size {} has {}", lut.Values.size(), lut.Size, expected);
		for (size_t index = 0; index < lut.Values.size(); ++index)
		{
			const glm::vec3& value = lut.Values[index];
			if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
				return MakeError(ErrorCode::Validation, "LUT entry {} has a value that is not a finite number", index);
		}
		return {};
	}

	Buffer SerializeLutPayload(const LutData& lut)
	{
		ENGINE_CORE_ASSERT(ValidateLutData(lut).has_value(), "SerializeLutPayload needs a LUT that passes ValidateLutData");
		BinaryWriter writer;
		writer.WriteU32(lut.Size);
		for (const glm::vec3& corner : { lut.DomainMin, lut.DomainMax })
		{
			writer.WriteF32(corner.x);
			writer.WriteF32(corner.y);
			writer.WriteF32(corner.z);
		}
		writer.WriteArray(std::span<const glm::vec3>(lut.Values));
		return writer.TakeBuffer();
	}

	Result<LutData> DeserializeLutPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		LutData lut;
		ENGINE_TRY_ASSIGN(lut.Size, reader.ReadU32());
		if (lut.Size > MaxLutSize)
			return MakeError(ErrorCode::Parse, "LUT payload declares size {}, more than the {} a LUT may have", lut.Size, MaxLutSize);
		for (glm::vec3* corner : { &lut.DomainMin, &lut.DomainMax })
		{
			ENGINE_TRY_ASSIGN(corner->x, reader.ReadF32());
			ENGINE_TRY_ASSIGN(corner->y, reader.ReadF32());
			ENGINE_TRY_ASSIGN(corner->z, reader.ReadF32());
		}
		// ReadArray checks the byte count against the bytes left before it allocates.
		ENGINE_TRY_ASSIGN(lut.Values, reader.ReadArray<glm::vec3>(Utils::GetLutEntryCount(lut.Size)));
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "LUT payload has {} trailing bytes", reader.GetRemaining());
		ENGINE_TRY(ValidateLutData(lut));
		return lut;
	}

	Result<LutData> ParseCubeLut(std::string_view text)
	{
		if (text.size() > MaxCubeLutBytes)
			return MakeError(ErrorCode::InvalidArgument, "the .cube file is {} bytes, more than the {} a LUT file may have", text.size(), MaxCubeLutBytes);
		// A UTF-8 byte order mark is not part of the first line.
		constexpr std::string_view ByteOrderMark = "\xEF\xBB\xBF";
		if (text.starts_with(ByteOrderMark))
			text.remove_prefix(ByteOrderMark.size());

		Utils::CubeHeader header;
		std::vector<glm::vec3> values;
		uint32_t line = 0;
		size_t position = 0;
		while (position < text.size())
		{
			const size_t end = std::min(text.find('\n', position), text.size());
			std::string_view content = text.substr(position, end - position);
			position = end + 1;
			++line;
			if (content.ends_with('\r'))
				content.remove_suffix(1);
			const std::vector<std::string_view> tokens = Utils::SplitCubeTokens(content);
			if (tokens.empty() || tokens.front().starts_with('#'))
				continue;

			if (!Utils::IsCubeNumberStart(tokens.front()))
			{
				if (!values.empty())
					return Utils::MakeCubeError(ErrorCode::Parse, line, std::format("keyword '{}' after the first data line", tokens.front()));
				ENGINE_TRY(Utils::ApplyCubeKeyword(header, tokens, line));
				continue;
			}
			if (!header.Size.has_value())
				return Utils::MakeCubeError(ErrorCode::Parse, line, "a data line before LUT_3D_SIZE");
			const size_t expected = Utils::GetLutEntryCount(*header.Size);
			if (values.size() == expected)
			{
				return Utils::MakeCubeError(ErrorCode::Parse, line,
					std::format("more entries than the {} of a table of size {}", expected, *header.Size));
			}
			if (values.empty())
				values.reserve(expected);
			ENGINE_TRY_ASSIGN(const glm::vec3 entry, Utils::ParseCubeTriple(tokens, line, "a data line"));
			values.push_back(entry);
		}

		if (!header.Size.has_value())
			return Utils::MakeCubeError(ErrorCode::Parse, std::max(line, 1u), "no LUT_3D_SIZE: the file holds no 3D table");
		const size_t expected = Utils::GetLutEntryCount(*header.Size);
		if (values.size() != expected)
		{
			return Utils::MakeCubeError(ErrorCode::Parse, line,
				std::format("the table ends after {} entries; a table of size {} has {}", values.size(), *header.Size, expected));
		}
		LutData lut;
		lut.Size = *header.Size;
		lut.DomainMin = header.DomainMin.value_or(glm::vec3(0.0f));
		lut.DomainMax = header.DomainMax.value_or(glm::vec3(1.0f));
		lut.Values = std::move(values);
		ENGINE_TRY(ValidateLutData(lut));
		return lut;
	}

	Buffer CookLut(const LutData& lut, uint32_t importerVersion)
	{
		const Buffer payload = SerializeLutPayload(lut);
		return WriteCookedArtifact(AssetType::Lut, LutData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<LutData>> LoadCookedLut(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView artifact, ReadCookedArtifact(cooked, AssetType::Lut, LutData::FormatVersion));
		ENGINE_TRY_ASSIGN(LutData lut, DeserializeLutPayload(artifact.Payload));
		return AssetRef<LutData>(CreateRef<LutData>(std::move(lut)));
	}

	std::string_view BuiltinLutToString(BuiltinLut lut)
	{
		switch (lut)
		{
			case BuiltinLut::Identity: return "Identity";
		}
		return "Unknown";
	}

	LutData GenerateBuiltinLut(BuiltinLut lut)
	{
		switch (lut)
		{
			case BuiltinLut::Identity: return Utils::MakeIdentityLut(33);
		}
		ENGINE_CORE_ASSERT(false, "Unknown BuiltinLut {}", std::to_underlying(lut));
		return Utils::MakeIdentityLut(MinLutSize);
	}

}
