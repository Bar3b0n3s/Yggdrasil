#include "TestsPCH.h"

#include "Engine/Renderer/BrdfLut.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

// The DFG LUT (Architecture §8.5, §8.6 step 5, §15.3 "DFG LUT vs CPU reference"). Skeletons of the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 7); stream A implements BrdfLut and removes the skips.

namespace Engine {

	namespace {

		// The (DFG1, DFG2) pairs of an RG16_FLOAT readback, rows top first.
		std::vector<glm::dvec2> DecodeRg16(const Image& image)
		{
			std::vector<glm::dvec2> texels;
			for (uint32_t row = 0; row < image.Height; ++row)
			{
				const std::span<const std::byte> bytes = image.GetRow(row);
				for (uint32_t column = 0; column < image.Width; ++column)
				{
					std::array<uint16_t, 2> halves{};
					std::memcpy(halves.data(), bytes.data() + static_cast<size_t>(column) * sizeof(halves), sizeof(halves));
					texels.emplace_back(Test::HalfToDouble(halves[0]), Test::HalfToDouble(halves[1]));
				}
			}
			return texels;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("BrdfLut: DFG matches the CPU reference" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<BrdfLut>> lut = BrdfLut::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(lut.has_value(), lut.error().ToString());
				CHECK((*lut)->GetPipelineCount() == BrdfLut::PipelineCount);
				nvrhi::ITexture* texture = (*lut)->GetTexture();
				REQUIRE(texture != nullptr);
				CHECK(texture->getDesc().width == BrdfLut::Size);
				CHECK(texture->getDesc().format == BrdfLut::Format);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				const std::vector<glm::dvec2> gpuTexels = DecodeRg16(*image);
				const std::vector<glm::dvec2> cpuTexels = Test::ComputeDfgLut(BrdfLut::Size, BrdfLut::LutSampleCount);
				REQUIRE(gpuTexels.size() == cpuTexels.size());
				// The same sample sequence on both sides: the difference is float versus double arithmetic and the binary16
				// rounding of the stored value.
				double worst = 0.0;
				for (size_t index = 0; index < gpuTexels.size(); ++index)
					worst = std::max(worst, std::max(std::abs(gpuTexels[index].x - cpuTexels[index].x), std::abs(gpuTexels[index].y - cpuTexels[index].y)));
				CHECK(worst < 2e-3);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("BrdfLut: the LUT is created once and readable after later submissions" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// A keepInitialState texture in ShaderResource, never a permanent state: Readback copies out of it (§8.2).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<BrdfLut>> lut = BrdfLut::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(lut.has_value(), lut.error().ToString());
				Readback readback(device);
				const Result<Image> first = readback.ReadTexture(*(*lut)->GetTexture());
				const Result<Image> second = readback.ReadTexture(*(*lut)->GetTexture());
				REQUIRE(first.has_value());
				REQUIRE(second.has_value());
				CHECK(first->Pixels == second->Pixels);
			}
			device.RunGarbageCollection();
		}
	}

}
