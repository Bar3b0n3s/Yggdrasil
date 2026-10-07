#include "TestsPCH.h"

#include "EditorCore/Automation/RegisterMethods.h"

#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/RandomValueGenerator.h"
#include "Engine/Reflection/StructInfo.h"
#include "Support/AutomationTestClient.h"

#include <set>

namespace Engine {

	// The M4 method set (Roadmap M4), M5's screenshot methods (registered at the M4/M5 merge, ADR 0009 decision 33) and M6's
	// asset.*, prefab.*, entity.bounds and project.refreshAssets (ADR 0010 decision 20), test hooks excluded.
	static const std::vector<std::string>& GetExpectedMethodNames()
	{
		static const std::vector<std::string> ExpectedNames = { "asset.create", "asset.delete", "asset.getImportSettings", "asset.getProperties",
			"asset.import", "asset.info", "asset.list", "asset.move", "asset.reimport", "asset.setImportSettings", "asset.setProperties",
			"component.list", "component.schema", "docs.get", "edit.batch", "edit.getSelection", "edit.history", "edit.redo", "edit.select",
			"edit.undo", "editor.screenshot", "entity.bounds", "entity.create", "entity.destroy", "entity.duplicate", "entity.get",
			"entity.reparent", "entity.update", "events.read", "log.read", "prefab.apply", "prefab.create", "prefab.instantiate", "prefab.revert",
			"prefab.unpack", "project.create", "project.getSettings", "project.info", "project.open", "project.refreshAssets", "project.save",
			"project.setSettings", "project.upgrade", "project.validate", "rpc.discover", "scene.diff", "scene.get", "scene.new", "scene.open",
			"scene.query", "scene.save", "scene.tree", "session.hello", "session.info", "session.shutdown", "viewport.screenshot" };
		return ExpectedNames;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("RegisterMethods: the editor registers exactly the M4 method set, the M5 screenshot methods and the M6 asset methods")
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

		TEST_CASE("RegisterMethods: test hooks are registered only when enabled")
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

		TEST_CASE("RegisterMethods: every method has a description, structs, an example and the documented flags")
		{
			Test::EditorTestFixture fixture("RegisterMetadata");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			const std::vector<std::string> launcher = { "docs.get", "project.create", "project.open", "rpc.discover", "session.hello",
				"session.info", "session.shutdown" };
			const std::vector<std::string> tools = { "asset.create", "asset.delete", "asset.import", "asset.list", "asset.move",
				"asset.setProperties", "component.list", "component.schema", "docs.get", "edit.batch", "edit.redo", "edit.undo", "editor.screenshot",
				"entity.bounds", "entity.create", "entity.destroy", "entity.duplicate", "entity.get", "entity.reparent", "entity.update", "log.read",
				"prefab.apply", "prefab.create", "prefab.instantiate", "project.create", "project.getSettings", "project.open", "project.save",
				"project.setSettings", "project.validate", "scene.diff", "scene.new", "scene.open", "scene.query", "scene.save", "scene.tree",
				"viewport.screenshot" };
			// The edit.batch ops (ADR 0008 decision 8): pure reads and methods whose effects all go through Execute. asset.import and
			// asset.reimport are pending operations; project.refreshAssets writes outside a command (ADR 0010 decision 20).
			const std::vector<std::string> batchable = { "asset.create", "asset.delete", "asset.getImportSettings", "asset.getProperties",
				"asset.info", "asset.list", "asset.move", "asset.setImportSettings", "asset.setProperties", "component.list", "component.schema",
				"docs.get", "edit.getSelection", "edit.history", "entity.bounds", "entity.create", "entity.destroy", "entity.duplicate", "entity.get",
				"entity.reparent", "entity.update", "events.read", "log.read", "prefab.apply", "prefab.create", "prefab.instantiate", "prefab.revert",
				"prefab.unpack", "project.getSettings", "project.info", "project.setSettings", "project.validate", "rpc.discover", "scene.diff",
				"scene.get", "scene.query", "scene.tree", "session.info" };
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

		TEST_CASE("RegisterMethods: every automation struct passes the registry round trips and its schema")
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

		TEST_CASE("RegisterMethods: every type RegisterEditorMethodTypes adds is described and round-trips through its schema")
		{
			// The editor's automation types follow the engine's in registration order, starting with EntitySummary
			// (RegisterAutomationCommonTypes runs first). §5.4's registry suite for them: descriptions everywhere, and default and
			// seeded random values serialize, read back equal and validate against their schema.
			Test::EditorTestFixture fixture("RegisterTypes");
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			const std::span<const StructInfo* const> structs = types.GetStructs();
			const auto first = std::find_if(structs.begin(), structs.end(), [](const StructInfo* type)
			{
				return type->GetName() == "EntitySummary";
			});
			REQUIRE(first != structs.end());
			const std::vector<const StructInfo*> automation(first, structs.end());
			// The importers' settings structs (RegisterAssetPipelineTypes) are engine data, which keeps its PascalCase keys
			// (convention 2 of MethodRegistry.h); every other automation struct has camelCase keys.
			TypeRegistry pipeline;
			RegisterAssetPipelineTypes(pipeline);
			std::set<std::string> engineData;
			for (const StructInfo* type : pipeline.GetStructs())
				engineData.insert(type->GetName());
			for (const std::string_view name : { "NoParams", "AssetSummary", "ProjectDiagnostic", "SessionHelloParams", "EntityCreateParams",
					 "EntityBoundsResult", "EditBatchOp", "LogReadMethodResult", "ViewportScreenshotParams", "EditorScreenshotResult",
					 "AssetInfoResult", "PrefabOverrideKey", "ProjectRefreshAssetsResult", "DebugPendResult" })
			{
				INFO(std::string(name));
				CHECK(std::ranges::any_of(automation, [name](const StructInfo* type)
				{
					return type->GetName() == name;
				}));
			}

			RandomValueGenerator generator(types, 0x4D35);
			for (const StructInfo* type : automation)
			{
				INFO(type->GetName());
				CHECK_FALSE(type->GetDescription().empty());
				for (const Scope<FieldInfo>& field : type->GetFields())
				{
					INFO(field->GetName());
					CHECK_FALSE(field->GetDescription().empty());
					// JSON keys are camelCase (ADR 0008 decision 8).
					if (!engineData.contains(type->GetName()))
						CHECK(std::islower(static_cast<unsigned char>(field->GetName().front())) != 0);
				}
				const Json schema = JsonSchema::ForStruct(*type);
				const Json defaults = type->MakeDefaultJson();
				CHECK(JsonSchema::Validate(schema, defaults).has_value());
				for (int iteration = 0; iteration < 20; ++iteration)
				{
					ObjectPtr object = type->CreateDefault();
					generator.Randomize(*type, object.get());
					const Result<Json> json = type->ToJson(object.get());
					REQUIRE(json.has_value());
					CHECK(JsonSchema::Validate(schema, *json).has_value());
					ObjectPtr read = type->CreateDefault();
					REQUIRE(type->FromJson(read.get(), JsonReader(*json), ReadContext{}).has_value());
					const Result<Json> again = type->ToJson(read.get());
					REQUIRE(again.has_value());
					CHECK(*again == *json);
				}
			}
		}

		TEST_CASE("RegisterMethods: the automation enums are registered with canonical names")
		{
			Test::EditorTestFixture fixture("RegisterEnums");
			const TypeRegistry& types = fixture.GetEngine().GetTypeRegistry();
			for (const std::string_view name : { "SceneTarget", "CommandOrigin", "DiagnosticSeverity", "LogLevel", "LogChannel", "EngineEventType",
					 "ValidationScope", "ProjectTemplate", "SceneTemplate", "SceneTreeFormat", "SceneDiffAgainst", "SceneEntityChangeKind", "ViewportView",
					 "AssetType", "AssetState", "AssetCreateType" })
			{
				INFO(std::string(name));
				const EnumInfo* info = types.FindEnum(name);
				REQUIRE(info != nullptr);
				CHECK_FALSE(info->GetEntries().empty());
			}
			const EnumInfo* format = types.FindEnum("SceneTreeFormat");
			REQUIRE(format != nullptr);
			REQUIRE(format->FindByNameIgnoreCase("JSON") != nullptr);
			CHECK(format->FindByNameIgnoreCase("JSON")->Name == "Json");
			const EnumInfo* events = types.FindEnum("EngineEventType");
			REQUIRE(events != nullptr);
			CHECK(events->GetEntries().size() == 11);
		}

		TEST_CASE("RegisterMethods: the tool catalogue's input schemas are compact")
		{
			Test::EditorTestFixture fixture("RegisterCatalog");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			Json catalog = methods.BuildToolCatalog();
			// The 25 M4 tools, viewport_screenshot and editor_screenshot, and M6's asset_list, asset_import, asset_create,
			// asset_set_properties, asset_move, asset_delete, prefab_create, prefab_instantiate, prefab_apply and entity_bounds.
			CHECK(catalog["Tools"].size() == 37);
			for (Json& tool : catalog["Tools"])
			{
				INFO(tool["name"].dump());
				CHECK_FALSE(tool["inputSchema"].contains("$defs"));
				CHECK(tool["inputSchema"].dump().size() < 4096);
			}
		}
	}

}
