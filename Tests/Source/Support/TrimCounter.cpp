#include "TestsPCH.h"
#include "Support/TrimCounter.h"

#include <utility>

namespace Engine {

	namespace Test {

		TrimCounter::TrimCounter(GpuRecreationRegistry& registry)
		{
			GpuRecreationHooks hooks;
			hooks.Name = "TrimCounter";
			hooks.Release = []() {};
			uint32_t* trims = &m_Trims;
			hooks.TrimUnused = [trims]()
			{
				++*trims;
			};
			m_Registration = registry.Register(std::move(hooks));
		}

	}

}
