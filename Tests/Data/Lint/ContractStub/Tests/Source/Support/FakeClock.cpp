#include "TestsPCH.h"

#include "Support/FakeClock.h"

namespace Engine {

	namespace Test {

		double FakeClock::Now() const
		{
			ENGINE_CONTRACT_STUB(); // Seeded defect: Tests support stubs are held to the same rule.
			return 0.0;
		}

	}

}
