#pragma once

// The contract-stub marker. A comment that names ENGINE_CONTRACT_STUB() is not a stub.
#define ENGINE_CONTRACT_STUB() static_cast<void>(0)

namespace Engine {

	inline int PlaceholderAnswer()
	{
		ENGINE_CONTRACT_STUB(); // Seeded defect: Base.h may define the marker, not use it.
		return 0;
	}

}
