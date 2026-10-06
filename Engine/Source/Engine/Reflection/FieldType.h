#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <string_view>

namespace Engine {

	// The kind of a reflected value (Architecture §5.4). Every consumer of the registry (serializer, inspector drawers,
	// script proxies, automation schemas, random generators, docs) switches over it. The C++ storage of each kind:
	//   Bool bool; Int32 int32_t; UInt32 uint32_t; Float float; Vec2 glm::vec2; Vec3 glm::vec3; Vec4 glm::vec4;
	//   Quat glm::quat; Color3 glm::vec3 (linear); Color4 glm::vec4 (linear); Bool3 glm::bvec3; String std::string;
	//   EntityRef UUID (EntityRef); AssetRef TypedAssetHandle<Type> (Asset/TypedAssetHandle.h); Enum a registered enum;
	//   Array std::vector<T>; Struct a registered struct; Map std::map<std::string, T>; Variant VariantValue.
	// The JSON spelling of each kind is fixed by §6: vectors, quaternions ([x, y, z, w]) and colours as number arrays,
	// Bool3 as [b, b, b], UUIDs as 16-digit hex strings with the invalid UUID written as null, enums by name, arrays as
	// arrays, structs as objects in field order, maps as objects with byte-wise sorted keys, variants verbatim.
	// The values are not persisted; append new kinds at the end anyway, so switch statements stay reviewable.
	enum class FieldType : uint8_t
	{
		Bool,
		Int32,
		UInt32,
		Float,
		Vec2,
		Vec3,
		Vec4,
		Quat,
		Color3,
		Color4,
		Bool3,
		String,
		EntityRef,
		AssetRef,
		Enum,
		Array,
		Struct,
		Map,
		Variant
	};

	// The enumerator name ("Color3"); "Unknown" for a value outside the enumeration. Pure and thread-safe.
	[[nodiscard]] constexpr std::string_view FieldTypeToString(FieldType type)
	{
		switch (type)
		{
			case FieldType::Bool:      return "Bool";
			case FieldType::Int32:     return "Int32";
			case FieldType::UInt32:    return "UInt32";
			case FieldType::Float:     return "Float";
			case FieldType::Vec2:      return "Vec2";
			case FieldType::Vec3:      return "Vec3";
			case FieldType::Vec4:      return "Vec4";
			case FieldType::Quat:      return "Quat";
			case FieldType::Color3:    return "Color3";
			case FieldType::Color4:    return "Color4";
			case FieldType::Bool3:     return "Bool3";
			case FieldType::String:    return "String";
			case FieldType::EntityRef: return "EntityRef";
			case FieldType::AssetRef:  return "AssetRef";
			case FieldType::Enum:      return "Enum";
			case FieldType::Array:     return "Array";
			case FieldType::Struct:    return "Struct";
			case FieldType::Map:       return "Map";
			case FieldType::Variant:   return "Variant";
		}
		return "Unknown";
	}

	// True for Array and Map, whose values hold elements of another type (TypeInfo::GetElement).
	[[nodiscard]] constexpr bool IsContainerFieldType(FieldType type)
	{
		return type == FieldType::Array || type == FieldType::Map;
	}

	// True for the kinds whose values are read and written as one Value by TypeOps::Read/Write: every kind except Array,
	// Map and Struct (Variant included: its value is one JSON value).
	[[nodiscard]] constexpr bool IsScalarFieldType(FieldType type)
	{
		return type != FieldType::Array && type != FieldType::Map && type != FieldType::Struct;
	}

	// How far the length of a Quat value may be from 1. Every read and write path of a Quat field rejects a quaternion
	// outside it (ADR 0006 decision 22); code that receives a rotation asserts or normalizes against the same measure.
	inline constexpr double UnitQuaternionTolerance = 1e-3;

	// True for the kinds whose numeric components FieldMeta::Min, Max, Step and MinMagnitude constrain: Int32, UInt32,
	// Float, Vec2, Vec3, Vec4, Color3 and Color4 (per component). Quat is normalized instead, Bool3 has no range.
	[[nodiscard]] constexpr bool IsNumericFieldType(FieldType type)
	{
		switch (type)
		{
			case FieldType::Int32:
			case FieldType::UInt32:
			case FieldType::Float:
			case FieldType::Vec2:
			case FieldType::Vec3:
			case FieldType::Vec4:
			case FieldType::Color3:
			case FieldType::Color4:
				return true;
			case FieldType::Bool:
			case FieldType::Quat:
			case FieldType::Bool3:
			case FieldType::String:
			case FieldType::EntityRef:
			case FieldType::AssetRef:
			case FieldType::Enum:
			case FieldType::Array:
			case FieldType::Struct:
			case FieldType::Map:
			case FieldType::Variant:
				return false;
		}
		return false;
	}

	// The run modes in which a reflected field, a script API member or a callback exists (Architecture §11.4, §15.6):
	// the editor (headless lockstep included), exported Release and exported Dist builds. Registrations use only All (the
	// default) and EditorOnly; the per-mode coverage gates evaluate the individual bits.
	enum class RunModes : uint8_t
	{
		None = 0,
		Editor = 1 << 0,
		Release = 1 << 1,
		Dist = 1 << 2,
		EditorOnly = Editor,
		All = Editor | Release | Dist
	};

	template<>
	inline constexpr bool EnableFlagOperators<RunModes> = true;

	// A reference to an entity (Architecture §4.7, §5.3): the entity's UUID, never a pointer or an entt::entity. Members of
	// type UUID reflect as FieldType::EntityRef; the invalid UUID is "no entity" and is written as JSON null. Prefab
	// instantiation remaps EntityRef values that point inside the prefab (§5.5).
	using EntityRef = UUID;

}
