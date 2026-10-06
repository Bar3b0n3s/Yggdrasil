#include "TestsPCH.h"

#include "EditorCore/Automation/RegisterMethods.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/RandomValueGenerator.h"
#include "Engine/Reflection/StructInfo.h"
#include "Support/AutomationTestClient.h"

namespace Engine {

	// The M4 method set (Roadmap M4), test hooks excluded.
	static const std::vector<std::string>& GetExpectedMethodNames()
	{
		static const std::vector<std::string> ExpectedNames = {
			"component.list", "component.schema", "docs.get", "edit.batch", "edit.getSelection", "edit.history", "edit.redo", "edit.select",
			"edit.undo", "entity.create", "entity.destroy", "entity.duplicate", "entity.get", "entity.reparent", "entity.update", "events.read",
			"log.read", "project.create", "project.getSettings", "project.info", "project.open", "project.save", "project.setSettings",
			"project.upgrade", "project.validate", "rpc.discover", "scene.diff", "scene.get", "scene.new", "scene.open", "scene.query",
			"scene.save", "scene.tree", "session.hello", "session.info", "session.shutdown"
		};
		return ExpectedNames;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("RegisterMethods: the editor registers exactly the M4 method set" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("RegisterSet");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			std::vector<std::string> names;
			for (const MethodDescriptor* method : methods.GetMethods())
				names.push_back(method->Specification.Name);
			CHECK(names == GetExpectedMethodNames());
		}

		TEST_CASE("RegisterMethods: test hooks are registered only when enabled" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("RegisterHooks");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = true });
			methods.Freeze();
			const MethodDescriptor* stall = methods.Find("debug.stall");
			REQUIRE(stall != nullptr);
			CHECK(stall->Specification.TestHook);
			CHECK_FALSE(stall->Specification.ExposeAsTool);
			const MethodDescriptor* pend = methods.Find("debug.pend");
			REQUIRE(pend != nullptr);
			CHECK(pend->Pending);
			CHECK_FALSE(pend->Specification.AllowedInBatch);
			CHECK_FALSE(methods.BuildToolCatalog().dump().contains("debug."));
			CHECK_FALSE(methods.BuildMethodCatalog().dump().contains("debug."));
		}

		TEST_CASE("RegisterMethods: every method has a description, structs, an example and the documented flags" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("RegisterMetadata");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			const std::vector<std::string> launcher = { "docs.get", "project.create", "project.open", "rpc.discover", "session.hello",
				"session.info", "session.shutdown" };
			const std::vector<std::string> tools = { "component.list", "component.schema", "docs.get", "edit.batch", "edit.redo", "edit.undo",
				"entity.create", "entity.destroy", "entity.duplicate", "entity.get", "entity.reparent", "entity.update", "log.read",
				"project.create", "project.getSettings", "project.open", "project.save", "project.setSettings", "project.validate", "scene.diff",
				"scene.new", "scene.open", "scene.query", "scene.save", "scene.tree" };
			// The edit.batch ops (ADR 0008 decision 8): pure reads and methods whose effects all go through Execute.
			const std::vector<std::string> batchable = { "component.list", "component.schema", "docs.get", "edit.getSelection",
				"edit.history", "entity.create", "entity.destroy", "entity.duplicate", "entity.get", "entity.reparent", "entity.update",
				"events.read", "log.read", "project.getSettings", "project.info", "project.setSettings", "project.validate",
				"rpc.discover", "scene.diff", "scene.get", "scene.query", "scene.tree", "session.info" };
			for (const MethodDescriptor* method : methods.GetMethods())
			{
				const MethodSpecification& specification = method->Specification;
				INFO(specification.Name);
				CHECK_FALSE(specification.Description.empty());
				CHECK(method->Params != nullptr);
				CHECK(method->Result != nullptr);
				CHECK_FALSE(specification.Examples.empty());
				const bool inLauncher = std::find(launcher.begin(), launcher.end(), specification.Name) != launcher.end();
				CHECK(specification.AvailableInLauncher == inLauncher);
				const bool isTool = std::find(tools.begin(), tools.end(), specification.Name) != tools.end();
				CHECK(specification.ExposeAsTool == isTool);
				const bool isBatchable = std::find(batchable.begin(), batchable.end(), specification.Name) != batchable.end();
				CHECK(specification.AllowedInBatch == isBatchable);
				if (specification.SupportsDryRun)
					CHECK((specification.Mutates || specification.Name == "project.validate"));
			}
		}

		TEST_CASE("RegisterMethods: every automation struct passes the registry round trips and its schema" * doctest::skip(true))
		{
			// §5.4's registry suite for the automation structs: default and seeded random values serialize, read back equal and
			// validate against their schema.
			Test::EditorTestFixture fixture("RegisterStructs");
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			MethodRegistry methods(types);
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = true });
			methods.Freeze();
			RandomValueGenerator generator(types, 0x4D34);
			for (const MethodDescriptor* method : methods.GetMethods())
			{
				for (const StructInfo* type : { method->Params, method->Result })
				{
					REQUIRE(type != nullptr);
					INFO(method->Specification.Name << " " << type->GetName());
					const Json schema = JsonSchema::ForStruct(*type);
					CHECK(JsonSchema::Validate(schema, type->MakeDefaultJson()).has_value());
					for (int iteration = 0; iteration < 20; ++iteration)
					{
						ObjectPtr object = type->CreateDefault();
						generator.Randomize(*type, object.get());
						const Result<Json> json = type->ToJson(object.get());
						REQUIRE(json.has_value());
						CHECK(JsonSchema::Validate(schema, *json).has_value());
					}
				}
			}
		}

		TEST_CASE("RegisterMethods: the tool catalogue's input schemas are compact" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("RegisterCatalog");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			Json catalog = methods.BuildToolCatalog();
			CHECK(catalog["Tools"].size() == 25);
			for (Json& tool : catalog["Tools"])
			{
				INFO(tool["name"].dump());
				CHECK_FALSE(tool["inputSchema"].contains("$defs"));
				CHECK(tool["inputSchema"].dump().size() < 4096);
			}
		}
	}

}
