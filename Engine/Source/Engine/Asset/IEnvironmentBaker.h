#pragma once

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <vector>

// The GPU environment bake behind EnvironmentImporter (Architecture §3 rule 4, §7.4, §8.6). Asset defines the interface;
// Renderer implements it (EnvironmentBaker, M8) and the editor injects it into the EditorAssetManager
// (EditorAssetManagerSpecification::EnvironmentBaker). Without a baker (--renderer none, tools) the importer reuses a cooked
// bake from the project or engine cache, or returns Unsupported with the hint "start the editor with a GPU once to bake
// this environment" (§7.4, §13.9).

namespace Engine {

	// The source of one bake: a Radiance .hdr decoded to RGB32F (stbi_loadf, §8.6 step 1).
	struct EnvironmentBakeInput
	{
		uint32_t Width = 0; // equirectangular: Width == 2 * Height
		uint32_t Height = 0;
		std::vector<float> Texels{}; // Width * Height RGB triples, rows top first; every value finite and >= 0
		// The optional ClampLuminance import setting (§8.6 step 1, default off): texels brighter than ClampLuminanceMax are
		// scaled down to it before baking.
		bool ClampLuminance = false;
		float ClampLuminanceMax = 0.0f;
	};

	// Bakes environments. Implementations are main-thread only (they record GPU work, §4.11), so the importer that uses one
	// runs on the main thread (IAssetImporter::RequiresMainThread). Bakes are deterministic for a given device class (no
	// temporal noise, §8.3); the results are cooked, so neither the editor at load nor the Runtime ever re-bakes (§8.6).
	class IEnvironmentBaker
	{
	public:
		virtual ~IEnvironmentBaker() = default;

		// The skybox cube, the prefiltered specular cube and the SH9 irradiance of `input` (§8.6 steps 2 to 4). Errors:
		// InvalidArgument for a malformed input (sizes, non-finite texels); Gpu for a device failure (naming the resource).
		[[nodiscard]] virtual Result<EnvironmentData> Bake(const EnvironmentBakeInput& input) = 0;
	};

}
