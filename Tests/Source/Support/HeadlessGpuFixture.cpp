#include "TestsPCH.h"
#include "Support/HeadlessGpuFixture.h"

#include "Engine/Core/FatalError.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Support/TestOptions.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the fixture:
// the device, the shaders:// mount, the services and the destruction checks. Until then no device is ever available, so
// every GPU test reports itself skipped (or fails under --require-gpu).

namespace Engine {

	namespace Test {

		HeadlessGpuFixture::HeadlessGpuFixture(const HeadlessGpuOptions& /*options*/)
		{
			ENGINE_CONTRACT_STUB();
			m_UnavailableReason = "the headless GPU fixture is not implemented yet (M5 contract)";
		}

		HeadlessGpuFixture::~HeadlessGpuFixture() = default;

		bool HeadlessGpuFixture::IsAvailable() const
		{
			ENGINE_CONTRACT_STUB();
			return false;
		}

		const std::string& HeadlessGpuFixture::GetUnavailableReason() const
		{
			return m_UnavailableReason;
		}

		GraphicsDevice& HeadlessGpuFixture::GetDevice()
		{
			ENGINE_CONTRACT_STUB();
			FatalError(FatalErrorKind::Assert, "HeadlessGpuFixture::GetDevice without a device");
		}

		ShaderLibrary& HeadlessGpuFixture::GetShaders()
		{
			ENGINE_CONTRACT_STUB();
			FatalError(FatalErrorKind::Assert, "HeadlessGpuFixture::GetShaders without a device");
		}

		PipelineFactory& HeadlessGpuFixture::GetPipelines()
		{
			ENGINE_CONTRACT_STUB();
			FatalError(FatalErrorKind::Assert, "HeadlessGpuFixture::GetPipelines without a device");
		}

		VirtualFileSystem& HeadlessGpuFixture::GetVfs()
		{
			ENGINE_CONTRACT_STUB();
			FatalError(FatalErrorKind::Assert, "HeadlessGpuFixture::GetVfs without a device");
		}

		void HeadlessGpuFixture::Reset()
		{
			ENGINE_CONTRACT_STUB();
		}

		std::vector<std::string> GetGpuApplicationArguments()
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		std::vector<std::string> GetGpuTestsChildArguments()
		{
			ENGINE_CONTRACT_STUB();
			return {};
		}

		void ReportGpuUnavailable(std::string_view reason)
		{
			if (GetTestOptions().RequireGpu)
				FAIL_CHECK("GPU test needs a device (--require-gpu): " << std::string(reason));
			else
				MESSAGE("GPU test skipped: " << std::string(reason));
		}

	}

}
