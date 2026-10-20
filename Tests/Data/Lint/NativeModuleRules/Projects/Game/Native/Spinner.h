#pragma once

#include "Engine/Native/NativeApi.h"

namespace Spinners {

	struct Spinner
	{
		static constexpr std::string_view NativeTypeName = "Spinner";
		float DegreesPerSecond = 90.0f;
	};

}
