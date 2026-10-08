#include "EnginePCH.h"
#include "Engine/Renderer/BlueNoise.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Random.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

// Ulichney's void-and-cluster method (1993) on a toroidal size x size grid. The energy of a texel is the sum, over the
// texels of the current binary pattern, of a Gaussian of their toroidal distance; a "cluster" is the pattern texel of the
// highest energy and a "void" the empty texel of the lowest. Energies are integers (each Gaussian value scaled by
// EnergyScale and rounded once), so adding and removing a texel are exact, the result never depends on summation order and
// ties are exact; with Core/DetMath's Exp the whole output is identical on every compiler and configuration.
//   1. Initial pattern: about a tenth of the texels set at seeded random positions, then repeatedly the tightest cluster
//      moved into the largest void until the move would put it back where it was.
//   2. Ranks below the pattern's count: from a copy of the pattern, the tightest cluster removed again and again, each
//      ranked one lower than the previous.
//   3. Ranks from the pattern's count up: from the initial pattern, the largest void filled again and again, each ranked one
//      higher. (Ulichney's third phase, the tightest cluster of the inverted pattern once more than half is set, picks the
//      same texel: with a Gaussian on a torus the energy of the empty texels is a constant minus the energy of the set ones.)

namespace Engine {

	namespace Utils {

		// The Gaussian of BlueNoiseSigma scaled to integers: a texel's energy is at most the kernel's sum, about
		// 2 pi sigma^2 = 14 times the scale, far inside int64.
		constexpr double BlueNoiseEnergyScale = 1099511627776.0; // 2^40

		// The toroidal Gaussian exp(-d^2 / (2 sigma^2)) of every offset, scaled to integers, in VoidAndCluster's layout: size
		// rows of 2 * size values, row dy and column x holding the Gaussian of the toroidal offset (x mod size, dy).
		static std::vector<int64_t> MakeBlueNoiseKernel(uint32_t size)
		{
			std::vector<int64_t> kernel(static_cast<size_t>(size) * size * 2, 0);
			const double divisor = 2.0 * BlueNoiseSigma * BlueNoiseSigma;
			for (uint32_t dy = 0; dy < size; ++dy)
			{
				const uint32_t wrappedY = std::min(dy, size - dy);
				for (uint32_t column = 0; column < 2 * size; ++column)
				{
					const uint32_t dx = column % size;
					const uint32_t wrappedX = std::min(dx, size - dx);
					const double distanceSquared = static_cast<double>(wrappedX * wrappedX + wrappedY * wrappedY);
					const double scaled = DetMath::Exp(-distanceSquared / divisor) * BlueNoiseEnergyScale;
					kernel[static_cast<size_t>(dy) * size * 2 + column] = static_cast<int64_t>(std::floor(scaled + 0.5));
				}
			}
			return kernel;
		}

	}

	namespace {

		// The grid, its binary pattern and the energy of every texel for that pattern.
		class VoidAndCluster
		{
		public:
			VoidAndCluster(uint32_t size, std::vector<int64_t> kernel)
				: m_Size(size), m_Kernel(std::move(kernel)), m_Pattern(static_cast<size_t>(size) * size, 0), m_Energy(m_Pattern.size(), 0)
			{
			}

			[[nodiscard]] size_t GetTexelCount() const { return m_Pattern.size(); }
			[[nodiscard]] bool IsSet(size_t texel) const { return m_Pattern[texel] != 0; }

			void Set(size_t texel) { Update(texel, 1); }
			void Clear(size_t texel) { Update(texel, -1); }

			// The set texel of the highest energy (the lowest index among equals).
			[[nodiscard]] size_t FindTightestCluster() const { return Find(true); }
			// The empty texel of the lowest energy (the lowest index among equals).
			[[nodiscard]] size_t FindLargestVoid() const { return Find(false); }
		private:
			// Adds (`sign` 1) or removes (`sign` -1) the Gaussian centred on `texel` to every energy.
			void Update(size_t texel, int64_t sign)
			{
				ENGINE_CORE_ASSERT(m_Pattern[texel] == (sign > 0 ? 0 : 1), "VoidAndCluster: texel {} is already {}", texel, sign > 0 ? "set" : "empty");
				m_Pattern[texel] = sign > 0 ? 1 : 0;
				const size_t size = m_Size;
				const size_t centerX = texel % size;
				const size_t centerY = texel / size;
				int64_t* energy = m_Energy.data();
				for (size_t y = 0; y < size; ++y)
				{
					// The kernel row of the toroidal vertical offset, read from column size - centerX on: m_Kernel holds each
					// row twice over, so the toroidal horizontal offset of column x is column x of that window.
					const int64_t* row = m_Kernel.data() + ((y + size - centerY) % size) * 2 * size + (size - centerX);
					int64_t* energyRow = energy + y * size;
					for (size_t x = 0; x < size; ++x)
						energyRow[x] += sign * row[x];
				}
			}

			[[nodiscard]] size_t Find(bool set) const
			{
				size_t best = std::numeric_limits<size_t>::max();
				for (size_t texel = 0; texel < m_Pattern.size(); ++texel)
				{
					if ((m_Pattern[texel] != 0) != set)
						continue;
					if (best == std::numeric_limits<size_t>::max() || (set ? m_Energy[texel] > m_Energy[best] : m_Energy[texel] < m_Energy[best]))
						best = texel;
				}
				ENGINE_CORE_ASSERT(best != std::numeric_limits<size_t>::max(), "VoidAndCluster: no {} texel", set ? "set" : "empty");
				return best;
			}
		private:
			uint32_t m_Size = 0;
			std::vector<int64_t> m_Kernel{}; // Utils::MakeBlueNoiseKernel
			std::vector<uint8_t> m_Pattern{};
			std::vector<int64_t> m_Energy{};
		};

	}

	std::vector<uint8_t> GenerateBlueNoise(uint32_t size, uint64_t seed)
	{
		ENGINE_CORE_ASSERT(size >= 4 && size <= 256 && std::has_single_bit(size), "GenerateBlueNoise needs a power of two from 4 to 256, got {}", size);
		VoidAndCluster grid(size, Utils::MakeBlueNoiseKernel(size));
		const size_t texelCount = grid.GetTexelCount();

		// 1. The initial pattern: a tenth of the texels at distinct seeded positions, then the clusters moved into the voids.
		const size_t initialCount = std::max<size_t>(texelCount / 10, 1);
		Random random(seed);
		for (size_t placed = 0; placed < initialCount;)
		{
			const size_t texel = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(texelCount) - 1));
			if (grid.IsSet(texel))
				continue;
			grid.Set(texel);
			++placed;
		}
		// A move never raises the pattern's total energy, and the pattern settles after a few hundred moves; the bound ends the
		// loop even if equal energies let it cycle.
		for (size_t move = 0; move < texelCount * 4; ++move)
		{
			const size_t cluster = grid.FindTightestCluster();
			grid.Clear(cluster);
			const size_t emptiest = grid.FindLargestVoid();
			grid.Set(emptiest);
			if (emptiest == cluster)
				break;
		}

		std::vector<uint32_t> ranks(texelCount, 0);
		// 2. The ranks below the pattern's count, from a copy of the initial pattern.
		{
			VoidAndCluster removal = grid;
			for (size_t rank = initialCount; rank > 0; --rank)
			{
				const size_t cluster = removal.FindTightestCluster();
				removal.Clear(cluster);
				ranks[cluster] = static_cast<uint32_t>(rank - 1);
			}
		}
		// 3. The ranks from the pattern's count up.
		for (size_t rank = initialCount; rank < texelCount; ++rank)
		{
			const size_t emptiest = grid.FindLargestVoid();
			grid.Set(emptiest);
			ranks[emptiest] = static_cast<uint32_t>(rank);
		}

		std::vector<uint8_t> noise(texelCount, 0);
		for (size_t texel = 0; texel < texelCount; ++texel)
			noise[texel] = static_cast<uint8_t>(static_cast<uint64_t>(ranks[texel]) * 256 / texelCount);
		return noise;
	}

	Result<Buffer> GenerateBlueNoiseTexture()
	{
		const std::vector<uint8_t> noise = GenerateBlueNoise();
		TextureData texture;
		texture.Format = TextureFormat::R8Unorm;
		texture.Width = BlueNoiseSize;
		texture.Height = BlueNoiseSize;
		texture.Mips = { { .Width = BlueNoiseSize, .Height = BlueNoiseSize, .Offset = 0, .Size = noise.size() } };
		texture.Pixels.resize(noise.size());
		for (size_t index = 0; index < noise.size(); ++index)
			texture.Pixels[index] = static_cast<std::byte>(noise[index]);
		ENGINE_TRY(ValidateTextureData(texture));
		return CookTexture(texture, BlueNoiseGeneratorVersion);
	}

}
