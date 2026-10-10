#include "TestsPCH.h"
#include "Engine/Scripting/ScriptProxy.h"

#include "Engine/Core/Base.h"

#include <doctest/doctest.h>

#include <type_traits>

namespace Engine {

	static_assert(std::is_trivially_copyable_v<ScriptEntityIdentity>);
	static_assert(std::is_trivially_copyable_v<ScriptProxyIdentity>);
	static_assert(std::is_same_v<decltype(ScriptEntityIdentity::ID), UUID>);
	static_assert(std::is_same_v<decltype(ScriptEntityIdentity::SceneGeneration), uint64_t>);
	static_assert(std::is_same_v<decltype(ScriptProxyIdentity::Entity), ScriptEntityIdentity>);

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptProxy: stale generations never resolve in a replacement scene" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Retain identities across scene reload/stop and recreate the same UUID; IsValid is false and every "
				"entity/component access fails without touching the replacement scene");
		}

		TEST_CASE("ScriptProxy: dead and marked entities fail without assertions" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Exercise zero IDs, zero generations, destroyed/marked entities, forged indices and wrong userdata tags through "
				"public bindings; only Entity:IsValid accepts stale identities as a false result");
		}

		TEST_CASE("ScriptProxy: each access re-resolves the component after structural changes" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Hold a proxy while creating entities and growing/removing component storage, remove/readd its type, and require "
				"absent access errors followed by fresh resolution of the readded type without a retained pointer");
		}

		TEST_CASE("ScriptProxy: shortcut eligibility follows reflected flags and entity precedence" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Table-drive ScriptVisible/Hidden/EntityLevel/NoShortcut flags: Name stays a string, ID/Relationship stay hidden, "
				"Script uses GetComponent/GetScript, absent eligible components are nil, and collisions fail startup");
		}

		TEST_CASE("ScriptProxy: reflected writes validate atomically and notify systems" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Exercise FieldMeta bounds/nonzero scale, enum values, type validators and resolved script schemas; reject "
				"invalid values atomically, verify scene revision and physics/audio notifications only after a valid reflected "
				"patch");
		}

		TEST_CASE("ScriptProxy: field visibility and run modes govern reads writes and coverage" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Read/write every eligible reflected field in each mode, exclude Hidden/non-Scriptable fields, reject read-only "
				"writes and EditorOnly fields in Runtime, and require independent successful read/write counters");
		}

		TEST_CASE("ScriptProxy: read-only evaluation rejects proxy writes before mutation" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Read fields in edit evaluation, attempt writes through direct proxies and aliases, and verify unchanged "
				"scene/revisions/system state plus unchanged write counters even if a caller bypasses the usual dispatch path");
		}

		TEST_CASE("ScriptProxy: Transform fast paths preserve reflection and interpolation semantics" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Test local/world transform writes and Teleport, render pose reads, read-only WorldScale, DetMath rotations, "
				"reflected validation, coverage and the host MarkTeleported path");
		}

		TEST_CASE("ScriptProxy: entity equality compares UUID without granting access" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Equal UUID userdata compares equal per section 11.5, but a stale generation still fails IsValid and cannot "
				"read/write any replacement-scene entity or component");
		}
	}

}
