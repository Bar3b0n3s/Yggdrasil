#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

namespace Engine {

	// The identity of an asset (Architecture §7.1): a UUID, stored in the asset's .meta sidecar and therefore stable across
	// moves and renames. 0 is "no asset". Sub-asset handles are Hash64(sourceHandle, subAssetKey) and built-in assets use
	// the reserved range 0x0000000000000001-0x00000000000003ff (§4.8). Files write a handle as a 16-digit hex string, and
	// the null handle as JSON null (§6). M3 defines only the alias that component fields need; the M6 Asset module adds
	// the metadata, registry and manager around it (Docs/Decisions/0006-m3-decisions.md, decision 2).
	using AssetHandle = UUID;

}
