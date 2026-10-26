#pragma once

#include "Engine/Graphics/GpuRecreation.h"

#include <cstdint>

namespace Engine {

	namespace Test {

		// An owner of nothing that counts the TrimUnused calls of a recreation registry (M18, C18.4;
		// Docs/Decisions/0029-m18-contract.md decision 43): registered as "TrimCounter" while it lives, with a Release that
		// does nothing and a TrimUnused hook that counts, so a test sees how many trims a request ran (GraphicsDevice::
		// TrimUnused through GraphicsDevice::GetRecreation, or GpuRecreationRegistry::TrimUnused directly). Its hook refers to
		// the counter itself, so it is not copyable or movable. Main thread, like the registry; it must not outlive the
		// registry (destroy it before the device).
		class TrimCounter
		{
		public:
			explicit TrimCounter(GpuRecreationRegistry& registry);

			TrimCounter(const TrimCounter&) = delete;
			TrimCounter& operator=(const TrimCounter&) = delete;
			TrimCounter(TrimCounter&&) = delete;
			TrimCounter& operator=(TrimCounter&&) = delete;

			// How many times the registry called the counter's TrimUnused hook.
			[[nodiscard]] uint32_t GetTrims() const { return m_Trims; }
		private:
			uint32_t m_Trims = 0;
			// Declared last, so the counter unregisters before its count goes.
			GpuRecreationRegistration m_Registration{};
		};

	}

}
