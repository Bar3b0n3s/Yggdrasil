#pragma once

#include "Engine/Core/Base.h"

namespace Engine {

	// Registry name "AudioListener" (Architecture §5.3, §10.2): where the scene is heard from. Without one the primary camera
	// listens; several active primaries raise AUDIO_MULTIPLE_PRIMARY_LISTENERS and the first in canonical order wins.
	struct AudioListenerComponent
	{
		bool Primary = true;
	};

}
