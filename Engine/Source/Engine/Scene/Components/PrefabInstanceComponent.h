#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	// Registry enum "PrefabOverrideKind": what a prefab override changes (Architecture §5.5).
	enum class PrefabOverrideKind : uint8_t
	{
		Field,           // one field of one component of a prefab entity; Value is the field's value
		AddComponent,    // a component added to a prefab entity; Value is the whole component object
		RemoveComponent, // a prefab component removed from a prefab entity; Value is null
		EntityKey        // an entity key of a prefab entity ("Name", "Active" or "Tags"); Value is the key's value
	};

	// Registry struct "PrefabEntityKeys": the entity keys a prefab override can change (PrefabOverrideKind::EntityKey),
	// with the §6.2 spelling of each. It exists so the override resolver has registered schemas for them (Name and Tags
	// mirror NameComponent and TagsComponent; Active is the DisabledTag state, which is not a reflected component). Never
	// stored on an entity.
	struct PrefabEntityKeys
	{
		std::string Name = "Entity";
		bool Active = true;
		std::vector<std::string> Tags;
	};

	// Registry struct "PrefabOverride" (Architecture §5.5, §6.2): one recorded difference between an instance and its prefab,
	// keyed by the prefab-local ID of the entity it applies to (PrefabEntityID, never the derived instance ID, so it
	// survives prefab edits). Value is a Variant resolved from Kind, Component and Field (PrefabOverrideValueResolver in
	// CoreRegistration.cpp, ResolveContext rules): the field's schema for Field, the component's whole schema
	// (GetSelfField()) for AddComponent, the PrefabEntityKeys field named by Field for EntityKey. Root Name, Transform and
	// Parent are implicit overrides and are never recorded; the root's Active and Tags are recorded like any member's.
	// Field order is the file's key order.
	struct PrefabOverride
	{
		UUID PrefabEntityID;
		PrefabOverrideKind Kind = PrefabOverrideKind::Field;
		std::string Component; // registry name for Field, AddComponent and RemoveComponent; empty for EntityKey
		std::string Field;     // field name for Field; "Name", "Active" or "Tags" for EntityKey; empty otherwise
		VariantValue Value;
	};

	// Registry name "Prefab" (Architecture §5.3, §5.5): marks the root of a prefab instance. Hidden; on the instance root
	// only. Prefab is the source prefab asset (a scene still loads when it is missing, diagnostic PREFAB_MISSING_ASSET);
	// Overrides are kept sorted by (PrefabEntityID, Component, Kind, Field) so the file is canonical.
	struct PrefabInstanceComponent
	{
		TypedAssetHandle<AssetType::Prefab> Prefab;
		std::vector<PrefabOverride> Overrides;
	};

}
