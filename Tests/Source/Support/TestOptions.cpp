#include "TestsPCH.h"
#include "Support/TestOptions.h"

// M1 contract stub (Roadmap rule 3): stream E implements option parsing and the run's option storage.

namespace Engine {

	namespace Test {

		Result<TestOptions> ParseTestOptions(int /*argc*/, const char* const* /*argv*/)
		{
			return MakeError(ErrorCode::Unsupported, "Test::ParseTestOptions is an M1 contract stub");
		}

		void SetTestOptions(TestOptions /*options*/)
		{
		}

		const TestOptions& GetTestOptions()
		{
			static const TestOptions DefaultOptions;
			return DefaultOptions;
		}

	}

}
