#pragma once

#include "Engine/AssetPipeline/EngineAssetBaker.h"
#include "Engine/Core/Base.h"

#include <span>

// The generators of the editor's Generated built-ins (Architecture §7.5, §8.4; AssetPipeline/EngineAssetBaker.h): the
// Renderer's "BlueNoise" (Renderer/BlueNoise.h, the tonemap's dither texture). AssetPipeline cannot name the Renderer's code
// (the inverted-interface rule of §3 rule 4 keeps the editor's asset pipeline free of it), so EditorCore assembles the list
// and every editor path that bakes passes it: EditorContext's asset manager (first-use bakes), Editor --bake-engine-assets
// and the exporter's Engine.pak cooking. One list, so the three can never disagree. Frozen by the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 8), which implemented and wired it.

namespace Engine {

	// Sorted by Id, each once (EngineBakeSpecification::Generators' precondition); valid for the whole process.
	[[nodiscard]] std::span<const EngineAssetGenerator> GetEngineAssetGenerators();

}
