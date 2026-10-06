#pragma once

#include "Engine/Core/Base.h"

namespace Engine {

	// Deactivates an entity and its subtree (Architecture §5.2). Not a reflected component: it is the entity key
	// "Active": false in files and is set through Entity::SetActive (scripts Entity:SetActive, automation entity.update),
	// so it has no registry name, fields or shortcut (Docs/Decisions/0006-m3-decisions.md, decision 7). The effective
	// state, which also considers ancestors, is Entity::IsActive; the runtime-only HierarchyDisabledTag caches it during
	// play (RuntimeComponents.h).
	struct DisabledTag
	{
	};

}
