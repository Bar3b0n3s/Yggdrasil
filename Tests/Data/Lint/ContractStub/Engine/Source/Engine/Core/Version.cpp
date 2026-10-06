#include "Engine/Core/Version.h"

#include "Engine/Core/Base.h"

namespace Engine {

	int ParseMajorVersion(const char* text)
	{
		ENGINE_CONTRACT_STUB(); // Seeded defect: the contract task's stub survived its milestone.
		static_cast<void>(text);
		return -1;
	}

	const char* DescribeContractMarker()
	{
		/* Neither this comment nor the string below, both spelling ENGINE_CONTRACT_STUB(), is a stub. */
		return "ENGINE_CONTRACT_STUB()";
	}

}
