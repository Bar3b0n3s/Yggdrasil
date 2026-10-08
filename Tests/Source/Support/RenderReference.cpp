#include "TestsPCH.h"
#include "Support/RenderReference.h"

// Contract stubs (Docs/Decisions/0013-m8-decisions.md decision 14): stream E implements every reference.

namespace Engine {

	namespace Test {

		glm::dvec2 Hammersley(uint32_t /*index*/, uint32_t /*count*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec2(0.0);
		}

		double DistributionGgx(double /*nDotH*/, double /*alpha*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0.0;
		}

		double VisibilitySmithGgxCorrelated(double /*nDotV*/, double /*nDotL*/, double /*alpha*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0.0;
		}

		glm::dvec3 FresnelSchlick(const glm::dvec3& /*f0*/, double /*vDotH*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		glm::dvec3 ImportanceSampleGgx(const glm::dvec2& /*xi*/, double /*alpha*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0, 0.0, 1.0);
		}

		glm::dvec2 ComputeDfg(double /*nDotV*/, double /*perceptualRoughness*/, uint32_t /*sampleCount*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec2(0.0);
		}

		std::vector<glm::dvec2> ComputeDfgLut(uint32_t /*size*/, uint32_t /*sampleCount*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		double ComputeCompensatedSpecularAlbedo(double /*nDotV*/, double /*perceptualRoughness*/, uint32_t /*sampleCount*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0.0;
		}

		glm::dvec3 CubeTexelDirection(uint32_t /*face*/, uint32_t /*column*/, uint32_t /*row*/, uint32_t /*size*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		double CubeTexelSolidAngle(uint32_t /*column*/, uint32_t /*row*/, uint32_t /*size*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0.0;
		}

		glm::dvec2 DirectionToEquirectUv(const glm::dvec3& /*direction*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec2(0.0);
		}

		glm::dvec3 EquirectUvToDirection(const glm::dvec2& /*uv*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		ReferenceCube EquirectToCube(std::span<const float> /*rgb*/, uint32_t /*width*/, uint32_t /*height*/, uint32_t /*size*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		ReferenceCube PrefilterSpecular(const ReferenceCube& /*source*/, double /*perceptualRoughness*/, uint32_t /*size*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		std::array<glm::dvec3, 9> ProjectIrradianceSH9(const ReferenceCube& /*source*/)
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		glm::dvec3 EvaluateIrradianceSH9(std::span<const glm::dvec3, 9> /*coefficients*/, const glm::dvec3& /*normal*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		glm::dvec3 TonemapAgx(const glm::dvec3& /*color*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		glm::dvec3 TonemapAces(const glm::dvec3& /*color*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		glm::dvec3 TonemapPbrNeutral(const glm::dvec3& /*color*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		glm::dvec3 TonemapLinear(const glm::dvec3& /*color*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		glm::dvec3 ApplyTonemapper(RenderTonemapper /*tonemapper*/, const glm::dvec3& /*color*/)
		{
			ENGINE_CONTRACT_STUB();
			return glm::dvec3(0.0);
		}

		double LinearToSrgb(double /*value*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0.0;
		}

		double SrgbToLinear(double /*value*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0.0;
		}

		std::vector<glm::dvec3> MakeTonemapSamplePoints()
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		double HalfToDouble(uint16_t /*half*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0.0;
		}

		uint16_t DoubleToHalf(double /*value*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0;
		}

	}

}
