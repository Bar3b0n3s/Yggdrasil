#include "TestsPCH.h"
#include "Support/DeviceLossTest.h"

#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	namespace Test {

		Result<DeviceLossReport> RunDeviceLossTest(HeadlessGpuFixture& /*gpu*/, const DeviceLossTest& /*test*/)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, "M18 shared loss-test helper contract (Docs/Decisions/0029-m18-contract.md decision 32, stream A)"));
		}

	}

}
