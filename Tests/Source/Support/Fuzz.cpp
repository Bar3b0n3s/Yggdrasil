#include "TestsPCH.h"
#include "Support/Fuzz.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <algorithm>
#include <cmath>

namespace Engine {

	namespace Test {

		namespace Utils {

			constexpr uint64_t SplitMixIncrement = 0x9E3779B97F4A7C15ull;

			// SplitMix64's output function (Steele, Lea and Flood) for an already advanced state.
			static uint64_t MixSplitMix64(uint64_t state)
			{
				uint64_t z = state;
				z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
				z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
				return z ^ (z >> 31);
			}

			// A uniform index in [0, count) for count > 0.
			static size_t PickIndex(Random& random, size_t count)
			{
				return static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(count) - 1));
			}

			// A uniform position in [0, count], an insertion point.
			static size_t PickPosition(Random& random, size_t count)
			{
				return static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(count)));
			}

			// A field value that readers check badly: zero, all bits set, the sign bit alone, the largest positive value, a
			// small count, or random bits.
			static uint64_t PickExtremeValue(Random& random, size_t width)
			{
				const uint64_t mask = width >= 8 ? ~0ull : (1ull << (width * 8)) - 1;
				const uint64_t sign = 1ull << (width * 8 - 1);
				switch (random.RangeInt(0, 5))
				{
					case 0:  return 0;
					case 1:  return mask;
					case 2:  return sign;
					case 3:  return sign - 1;
					case 4:  return static_cast<uint64_t>(random.RangeInt(1, 16));
					default: return random.NextU64() & mask;
				}
			}

			static void MutateBytesOnce(std::vector<std::byte>& bytes, Random& random)
			{
				const int64_t kind = bytes.empty() ? 3 : random.RangeInt(0, 6);
				switch (kind)
				{
					case 0: // flip one to eight bits
					{
						const int64_t flips = random.RangeInt(1, 8);
						for (int64_t flip = 0; flip < flips; ++flip)
							bytes[PickIndex(random, bytes.size())] ^= static_cast<std::byte>(1u << random.RangeInt(0, 7));
						break;
					}
					case 1: // truncate
						bytes.resize(PickIndex(random, bytes.size()));
						break;
					case 2: // an extreme little-endian value over a field
					{
						static constexpr std::array<size_t, 4> Widths = { 1, 2, 4, 8 };
						const size_t width = Widths[PickIndex(random, Widths.size())];
						if (bytes.size() < width)
						{
							bytes[PickIndex(random, bytes.size())] = static_cast<std::byte>(random.NextU32() & 0xFFu);
							break;
						}
						const size_t offset = PickIndex(random, bytes.size() - width + 1);
						const uint64_t value = PickExtremeValue(random, width);
						for (size_t index = 0; index < width; ++index)
							bytes[offset + index] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
						break;
					}
					case 3: // insert one to sixteen random bytes
					{
						const size_t offset = PickPosition(random, bytes.size());
						const size_t count = static_cast<size_t>(random.RangeInt(1, 16));
						std::vector<std::byte> inserted(count);
						for (std::byte& value : inserted)
							value = static_cast<std::byte>(random.NextU32() & 0xFFu);
						bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(offset), inserted.begin(), inserted.end());
						break;
					}
					case 4: // remove a chunk of up to 64 bytes
					{
						const size_t offset = PickIndex(random, bytes.size());
						const size_t count = static_cast<size_t>(random.RangeInt(1, static_cast<int64_t>(std::min<size_t>(64, bytes.size() - offset))));
						bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
						break;
					}
					case 5: // duplicate a chunk of up to 64 bytes somewhere else
					{
						const size_t offset = PickIndex(random, bytes.size());
						const size_t count = static_cast<size_t>(random.RangeInt(1, static_cast<int64_t>(std::min<size_t>(64, bytes.size() - offset))));
						const std::vector<std::byte> chunk(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
							bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
						const size_t destination = PickPosition(random, bytes.size());
						bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(destination), chunk.begin(), chunk.end());
						break;
					}
					default: // copy a chunk of up to 64 bytes over another place
					{
						const size_t source = PickIndex(random, bytes.size());
						const size_t destination = PickIndex(random, bytes.size());
						const size_t count = std::min<size_t>(static_cast<size_t>(random.RangeInt(1, 64)),
							std::min(bytes.size() - source, bytes.size() - destination));
						std::memmove(bytes.data() + destination, bytes.data() + source, count);
						break;
					}
				}
			}

			static bool IsDigit(char character)
			{
				return character >= '0' && character <= '9';
			}

			static void MutateTextOnce(std::string& text, Random& random)
			{
				static constexpr std::array<std::string_view, 14> Numbers = { "-1", "0", "-0", "0.5", "1e308", "-1e308", "1e999", "4294967296",
					"18446744073709551616", "9007199254740993", "1e-320", "NaN", "Infinity", "2147483648" };
				static constexpr std::array<std::string_view, 12> Values = { "null", "true", "false", "0", "[]", "{}", "\"\"", "\"\\u0000\"",
					"\"\\ud800\"", "\"../../outside\"", "\"project://\"", "-1" };
				static constexpr std::string_view Structural = "{}[]:,\"\\";

				const int64_t kind = text.empty() ? 1 : random.RangeInt(0, 5);
				switch (kind)
				{
					case 0: // an extreme number over a number
					{
						const size_t start = PickIndex(random, text.size());
						size_t begin = start;
						while (begin < text.size() && !IsDigit(text[begin]))
							++begin;
						if (begin == text.size())
						{
							text.insert(start, Numbers[PickIndex(random, Numbers.size())]);
							break;
						}
						size_t end = begin;
						while (end < text.size() && (IsDigit(text[end]) || text[end] == '.' || text[end] == 'e' || text[end] == 'E'))
							++end;
						if (begin > 0 && text[begin - 1] == '-')
							--begin;
						text.replace(begin, end - begin, Numbers[PickIndex(random, Numbers.size())]);
						break;
					}
					case 1: // insert a structural character
						text.insert(PickPosition(random, text.size()), 1, Structural[PickIndex(random, Structural.size())]);
						break;
					case 2: // remove a structural character
					{
						const size_t start = PickIndex(random, text.size());
						const size_t found = text.find_first_of(Structural, start);
						const size_t position = found != std::string::npos ? found : text.find_first_of(Structural);
						if (position != std::string::npos)
							text.erase(position, 1);
						else
							text.erase(start, 1);
						break;
					}
					case 3: // duplicate or remove a line
					{
						const size_t start = PickIndex(random, text.size());
						const size_t previous = start == 0 ? std::string::npos : text.rfind('\n', start - 1);
						const size_t lineBegin = previous == std::string::npos ? 0 : previous + 1;
						const size_t newline = text.find('\n', start);
						const size_t lineEnd = newline == std::string::npos ? text.size() : newline + 1;
						if (random.NextBool())
							text.insert(lineBegin, text.substr(lineBegin, lineEnd - lineBegin));
						else
							text.erase(lineBegin, lineEnd - lineBegin);
						break;
					}
					case 4: // a quoted string replaced by another value
					{
						const size_t start = PickIndex(random, text.size());
						size_t open = text.find('"', start);
						if (open == std::string::npos)
							open = text.find('"');
						const size_t close = open == std::string::npos ? std::string::npos : text.find('"', open + 1);
						if (close == std::string::npos)
						{
							text.insert(start, Values[PickIndex(random, Values.size())]);
							break;
						}
						text.replace(open, close - open + 1, Values[PickIndex(random, Values.size())]);
						break;
					}
					default: // the binary mutations on the text's bytes
					{
						std::vector<std::byte> bytes(text.size());
						std::memcpy(bytes.data(), text.data(), text.size());
						MutateBytesOnce(bytes, random);
						text.assign(bytes.size(), '\0');
						std::memcpy(text.data(), bytes.data(), bytes.size());
						break;
					}
				}
			}

		}

		FuzzRange GetFuzzRange(uint64_t acceptance, uint64_t minimum)
		{
			const TestOptions& options = GetTestOptions();
			FuzzRange range;
			if (options.FuzzIterations > 0)
			{
				range.Total = options.FuzzIterations;
			}
			else
			{
				const double scaled = std::round(static_cast<double>(acceptance) * options.FuzzScale);
				range.Total = std::max(minimum, static_cast<uint64_t>(scaled));
			}

			if (options.FuzzIndex.has_value())
			{
				const uint64_t index = *options.FuzzIndex;
				REQUIRE_MESSAGE(index < range.Total, "--fuzz-index=", index, " is not below the case's ", range.Total, " iterations");
				range.Begin = index;
				range.End = index + 1;
				return range;
			}

			const uint64_t shardCount = std::max<uint64_t>(1, options.FuzzShardCount);
			const uint64_t shard = std::min<uint64_t>(options.FuzzShardIndex, shardCount - 1);
			const uint64_t base = range.Total / shardCount;
			const uint64_t longer = range.Total % shardCount;
			range.Begin = shard * base + std::min(shard, longer);
			range.End = range.Begin + base + (shard < longer ? 1 : 0);
			return range;
		}

		uint64_t GetFuzzIterationSeed(uint64_t caseSeed, uint64_t index)
		{
			// Unsigned arithmetic wraps, as SplitMix64's state does.
			return Utils::MixSplitMix64(caseSeed + (index + 1) * Utils::SplitMixIncrement);
		}

		void ReportFuzzRun(std::string_view caseName, uint64_t caseSeed, const FuzzRange& range, const FuzzCounts& counts)
		{
			if (caseName.empty() || caseName.find('\'') != std::string_view::npos)
			{
				FAIL_CHECK("a fuzz case name must be non-empty and hold no apostrophe (the hardening stage quotes it): '", std::string(caseName), "'");
				return;
			}
			ENGINE_CORE_INFO("Fuzz '{}': iterations {}..{} of {} (seed 0x{:016x}): {} accepted, {} rejected", caseName, range.Begin, range.End,
				range.Total, caseSeed, counts.Accepted, counts.Rejected);
		}

		bool IsFuzzSample(const FuzzRange& range, uint64_t iterations)
		{
			return !GetTestOptions().FuzzIndex.has_value() && range.End - range.Begin >= iterations;
		}

		std::string GetRunningFuzzCaseName()
		{
			const std::optional<RunningTestCase> running = GetRunningTestCase();
			return running.has_value() ? running->Name : std::string();
		}

		std::string GetFuzzReplayCommand(uint64_t total, uint64_t index)
		{
			std::string filter;
			for (const char character : GetRunningFuzzCaseName())
			{
				if (character == ',')
					filter += '\\';
				filter += character;
			}
			return std::format("Tests \"--test-case={}\" --fuzz-iterations={} --fuzz-index={}", filter, total, index);
		}

		std::vector<std::vector<std::byte>> ReadFuzzSeeds(const std::filesystem::path& root, std::string_view extension)
		{
			std::vector<std::filesystem::path> files;
			std::error_code error;
			std::filesystem::recursive_directory_iterator iterator(root, error);
			REQUIRE_MESSAGE(!error, "cannot list the seed corpus '", PathToUtf8(root), "': ", error.message());
			for (const std::filesystem::recursive_directory_iterator end; iterator != end; iterator.increment(error))
			{
				REQUIRE_MESSAGE(!error, "cannot list the seed corpus '", PathToUtf8(root), "': ", error.message());
				std::error_code status;
				if (iterator->is_regular_file(status) && !status && PathToUtf8(iterator->path().extension()) == extension)
					files.push_back(iterator->path());
			}
			std::sort(files.begin(), files.end());
			REQUIRE_MESSAGE(!files.empty(), "the seed corpus '", PathToUtf8(root), "' holds no ", std::string(extension), " file");
			std::vector<std::vector<std::byte>> seeds;
			for (const std::filesystem::path& file : files)
			{
				Result<Buffer> bytes = FileSystem::ReadFile(file);
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				seeds.push_back(std::move(*bytes));
			}
			return seeds;
		}

		std::vector<std::string> ReadFuzzSeedTexts(const std::filesystem::path& root, std::string_view extension)
		{
			std::vector<std::string> texts;
			for (const std::vector<std::byte>& seed : ReadFuzzSeeds(root, extension))
				texts.emplace_back(AsStringView(seed));
			return texts;
		}

		void MutateBytes(std::vector<std::byte>& bytes, Random& random)
		{
			const int64_t rounds = random.RangeInt(1, 3);
			for (int64_t round = 0; round < rounds; ++round)
				Utils::MutateBytesOnce(bytes, random);
		}

		void MutateText(std::string& text, Random& random)
		{
			const int64_t rounds = random.RangeInt(1, 3);
			for (int64_t round = 0; round < rounds; ++round)
				Utils::MutateTextOnce(text, random);
		}

	}

}
