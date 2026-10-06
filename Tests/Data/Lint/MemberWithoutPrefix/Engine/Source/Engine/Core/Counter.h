#pragma once

#include <cstdint>

namespace Engine {

	class Counter
	{
	public:
		void Increment() { ++Count; }
		uint32_t GetCount() const { return Count; }
	private:
		uint32_t Count = 0; // Seeded defect: a private data member without the m_ prefix.
	};

}
