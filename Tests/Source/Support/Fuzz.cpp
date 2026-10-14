#include "TestsPCH.h"
#include "Support/Fuzz.h"

namespace Engine {

	namespace Test {

		FuzzRange GetFuzzRange(uint64_t acceptance, uint64_t /*minimum*/)
		{
			ENGINE_CONTRACT_STUB();
			return FuzzRange{ .Begin = 0, .End = acceptance, .Total = acceptance };
		}

		uint64_t GetFuzzIterationSeed(uint64_t /*caseSeed*/, uint64_t /*index*/)
		{
			ENGINE_CONTRACT_STUB();
			return 0;
		}

		void ReportFuzzRun(std::string_view /*caseName*/, uint64_t /*caseSeed*/, const FuzzRange& /*range*/, const FuzzCounts& /*counts*/)
		{
			ENGINE_CONTRACT_STUB();
		}

	}

}
