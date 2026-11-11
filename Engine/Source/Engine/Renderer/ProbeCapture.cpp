#include "EnginePCH.h"
#include "Engine/Renderer/ProbeCapture.h"

// Probe captures (ProbeCapture.h): contract stubs of M26's C26.1, implemented by stream Q-A
// (Docs/Decisions/0037-m26-contract.md decision 8). Nothing creates a capture until then.

namespace Engine {

	struct ProbeCapture::State
	{
	};

	std::array<glm::vec3, 9> ProjectCubeToIrradianceSH9(const ProbeCubeCapture& /*capture*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ProbeEquirectImage ResampleCubeToEquirect(const ProbeCubeCapture& /*capture*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Buffer WriteRadianceHdr(const ProbeEquirectImage& /*image*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ProbeCapture::ProbeCapture(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ProbeCapture::~ProbeCapture() = default;

	Result<Scope<ProbeCapture>> ProbeCapture::Create(GraphicsDevice& /*device*/, SceneRendererPipelines& /*pipelines*/, GpuResourceCache& /*cache*/,
		AssetManager& /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 probes contract (Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A)");
	}

	Result<ProbeCubeCapture> ProbeCapture::Capture(const RenderSnapshot& /*snapshot*/, const glm::vec3& /*position*/, uint32_t /*faceSize*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M26 probes contract (Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A)");
	}

}
