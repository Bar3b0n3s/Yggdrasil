#include "TestsPCH.h"
#include "Engine/Session/ReplayFingerprint.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		// A manager of a fixed set of cooked assets: each handle has a type, a readable path and the hash its cooked artifact
		// would have; nothing loads. Enough for ComputeReplayFingerprint, which reads only types, paths and cooked hashes.
		class CookedHashAssetManager final : public AssetManager
		{
		public:
			struct Entry
			{
				AssetType Type = AssetType::None;
				std::string Path{};
				uint64_t CookedHash = 0;
			};

			CookedHashAssetManager()
				: m_Jobs(0, m_MainThreadQueue)
			{
			}

			void Add(AssetHandle handle, Entry entry) { m_Entries[handle] = std::move(entry); }

			[[nodiscard]] Result<AssetRef<Asset>> Load(AssetHandle handle) override
			{
				return MakeError(ErrorCode::NotFound, "nothing loads here ({})", handle.ToString());
			}
			[[nodiscard]] JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override
			{
				Result<AssetRef<Asset>> loaded = Load(handle);
				return m_Jobs.Submit([loaded]() -> Result<AssetRef<Asset>>
				{
					return loaded;
				});
			}
			[[nodiscard]] AssetState GetState(AssetHandle /*handle*/) const override { return AssetState::Unloaded; }
			[[nodiscard]] const AssetMetadata* GetMetadata(AssetHandle /*handle*/) const override { return nullptr; }
			[[nodiscard]] AssetType GetAssetType(AssetHandle handle) const override
			{
				const auto found = m_Entries.find(handle);
				return found == m_Entries.end() ? AssetType::None : found->second.Type;
			}
			[[nodiscard]] std::optional<AssetHandle> Resolve(std::string_view /*reference*/) const override { return std::nullopt; }
			[[nodiscard]] std::string GetReferencePath(AssetHandle handle) const override
			{
				const auto found = m_Entries.find(handle);
				return found == m_Entries.end() ? std::string() : found->second.Path;
			}
			void WaitIdle() override {}
			[[nodiscard]] Result<uint64_t> GetCookedHash(AssetHandle handle) override
			{
				const auto found = m_Entries.find(handle);
				if (found == m_Entries.end())
					return MakeError(ErrorCode::NotFound, "no asset {}", handle.ToString());
				return found->second.CookedHash;
			}
		private:
			MainThreadQueue m_MainThreadQueue;
			JobSystem m_Jobs; // inline
			std::map<AssetHandle, Entry> m_Entries;
		};

		// The schema of the Behaviour script 0x515: Sound, an AudioClip asset field, and Target, an entity field. Both hold
		// 16-hex strings in a Script component's Fields; only the schema tells the asset from the entity.
		Ref<const ScriptFieldSchemaSource> MakeSoundSchemas()
		{
			auto script = CreateRef<ScriptData>();
			script->Kind = ScriptKind::Behaviour;
			script->Name = "Board";
			ScriptFieldSchema sound;
			sound.Name = "Sound";
			sound.Type = FieldType::AssetRef;
			sound.Meta.AssetFilter = "AudioClip";
			script->Fields.push_back(sound);
			ScriptFieldSchema target;
			target.Name = "Target";
			target.Type = FieldType::EntityRef;
			script->Fields.push_back(target);
			const auto schemas = ScriptFieldSchemaSource::Create({ { UUID(0x515), AssetRef<ScriptData>(script) } });
			REQUIRE_MESSAGE(schemas.has_value(), (schemas ? std::string() : schemas.error().ToString()));
			return *schemas;
		}

		std::vector<AssetHandle> Handles(const ReplayFingerprint& fingerprint)
		{
			std::vector<AssetHandle> handles;
			for (const ReplayAssetFingerprint& asset : fingerprint.Assets)
				handles.push_back(asset.Handle);
			return handles;
		}

		const ReplayEntityFingerprint* FindEntity(const ReplayFingerprint& fingerprint, UUID id)
		{
			for (const ReplayEntityFingerprint& entity : fingerprint.Entities)
			{
				if (entity.Entity == id)
					return &entity;
			}
			return nullptr;
		}

		std::string ComponentHash(const ReplayEntityFingerprint& entity, std::string_view component)
		{
			for (const ReplayComponentFingerprint& item : entity.Components)
			{
				if (item.Component == component)
					return item.Hash;
			}
			return {};
		}

	}

	TEST_SUITE("Session")
	{
		// Docs/Decisions/0030-m19-contract.md decision 2 (A-FPRINT): every entity of the scene in id order with its readable
		// path, the hash of its own members and of each serialized component (registry order), and every asset a component
		// names, once, in handle order, with its type, path and cooked hash: a Script component's asset fields among them,
		// typed by the session's script field schemas, so an entity field's 16-hex value is never taken for an asset. A
		// changed component changes only its own hash; a renamed entity only the entity's; the computation is deterministic.
		TEST_CASE("ReplayFingerprint: the fingerprint holds every entity and component of the scene and the assets they reference" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity board = scene.CreateEntityWithID(UUID(0x22), "Board");
			const Entity camera = scene.CreateEntityWithID(UUID(0x11), "Camera");
			static_cast<void>(camera);
			board.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(UUID(0x101)),
				.Materials = { TypedAssetHandle<AssetType::Material>(UUID(0x202)), TypedAssetHandle<AssetType::Material>(UUID(0x202)) } });
			board.AddComponent<ScriptComponent>(ScriptComponent{ .Script = TypedAssetHandle<AssetType::Script>(UUID(0x515)),
				.Fields = { { "Sound", VariantValue(Json("0000000000000303")) }, { "Target", VariantValue(Json("0000000000000011")) } } });
			CookedHashAssetManager assets;
			assets.Add(UUID(0x101), { .Type = AssetType::Mesh, .Path = "Assets/Models/Board.glb#mesh:0:Board", .CookedHash = 0xb0a7d });
			assets.Add(UUID(0x202), { .Type = AssetType::Material, .Path = "Assets/Materials/Wood.material", .CookedHash = 0x3009 });
			assets.Add(UUID(0x303), { .Type = AssetType::AudioClip, .Path = "Assets/Audio/Move.sfx", .CookedHash = 0x50d });
			assets.Add(UUID(0x515), { .Type = AssetType::Script, .Path = "Assets/Scripts/Board.luau", .CookedHash = 0x5c1 });
			// An asset whose handle equals the Camera's id: the Target field names the entity, never this asset.
			assets.Add(UUID(0x11), { .Type = AssetType::Material, .Path = "Assets/Materials/Decoy.material", .CookedHash = 0xdec0 });
			const Ref<const ScriptFieldSchemaSource> schemas = MakeSoundSchemas();

			const Result<ReplayFingerprint> fingerprint = ComputeReplayFingerprint(scene, schemas.get(), &assets);
			REQUIRE_MESSAGE(fingerprint.has_value(), fingerprint.error().ToString());
			REQUIRE(fingerprint->Entities.size() == 2);
			CHECK(fingerprint->Entities[0].Entity == UUID(0x11));
			CHECK(fingerprint->Entities[0].Path == "/Camera");
			CHECK(fingerprint->Entities[1].Entity == UUID(0x22));
			CHECK(fingerprint->Entities[1].Path == "/Board");
			const ReplayEntityFingerprint& boardPrint = fingerprint->Entities[1];
			CHECK(boardPrint.Hash.size() == 16);
			CHECK_FALSE(ComponentHash(boardPrint, "Transform").empty());
			CHECK_FALSE(ComponentHash(boardPrint, "MeshRenderer").empty());
			CHECK_FALSE(ComponentHash(boardPrint, "Script").empty());
			REQUIRE(fingerprint->Assets.size() == 4);
			CHECK(fingerprint->Assets[0].Handle == UUID(0x101));
			CHECK(fingerprint->Assets[0].Type == "Mesh");
			CHECK(fingerprint->Assets[0].Path == "Assets/Models/Board.glb#mesh:0:Board");
			CHECK(fingerprint->Assets[0].Hash == std::format("{:016x}", 0xb0a7dULL));
			CHECK(fingerprint->Assets[1].Handle == UUID(0x202));
			CHECK(fingerprint->Assets[1].Type == "Material");
			CHECK(fingerprint->Assets[1].Hash == std::format("{:016x}", 0x3009ULL));
			CHECK(fingerprint->Assets[2].Handle == UUID(0x303));
			CHECK(fingerprint->Assets[2].Type == "AudioClip");
			CHECK(fingerprint->Assets[3].Handle == UUID(0x515));
			CHECK(fingerprint->Assets[3].Type == "Script");

			// Without the schemas a script field names no asset; the Script component's script still does.
			const Result<ReplayFingerprint> untyped = ComputeReplayFingerprint(scene, nullptr, &assets);
			REQUIRE(untyped.has_value());
			CHECK(Handles(*untyped) == std::vector<AssetHandle>{ AssetHandle(0x101), AssetHandle(0x202), AssetHandle(0x515) });
			CHECK(untyped->Entities == fingerprint->Entities);

			// Deterministic.
			const Result<ReplayFingerprint> again = ComputeReplayFingerprint(scene, schemas.get(), &assets);
			REQUIRE(again.has_value());
			CHECK(*again == *fingerprint);

			// A changed component changes its own hash only.
			board.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.CastShadows = false;
			});
			const Result<ReplayFingerprint> patched = ComputeReplayFingerprint(scene, schemas.get(), &assets);
			REQUIRE(patched.has_value());
			const ReplayEntityFingerprint* patchedBoard = FindEntity(*patched, UUID(0x22));
			REQUIRE(patchedBoard != nullptr);
			CHECK(patchedBoard->Hash == boardPrint.Hash);
			CHECK(ComponentHash(*patchedBoard, "Transform") == ComponentHash(boardPrint, "Transform"));
			CHECK(ComponentHash(*patchedBoard, "MeshRenderer") != ComponentHash(boardPrint, "MeshRenderer"));
			CHECK(patched->Entities[0] == fingerprint->Entities[0]);

			// A renamed entity changes the entity's own hash, not its components'.
			board.SetName("Well");
			const Result<ReplayFingerprint> renamed = ComputeReplayFingerprint(scene, schemas.get(), &assets);
			REQUIRE(renamed.has_value());
			const ReplayEntityFingerprint* renamedBoard = FindEntity(*renamed, UUID(0x22));
			REQUIRE(renamedBoard != nullptr);
			CHECK(renamedBoard->Path == "/Well");
			CHECK(renamedBoard->Hash != boardPrint.Hash);
			CHECK(renamedBoard->Components == patchedBoard->Components);

			// Without an asset manager no asset is listed.
			const Result<ReplayFingerprint> withoutAssets = ComputeReplayFingerprint(scene, schemas.get(), nullptr);
			REQUIRE(withoutAssets.has_value());
			CHECK(withoutAssets->Assets.empty());
			CHECK(withoutAssets->Entities.size() == 2);
		}

		// Decision 2: the changes in their four groups (entities added or removed, entities changed, components, assets), each
		// line naming the item, at most the limit with "... and <n> more" last, and none for equal fingerprints.
		TEST_CASE("ReplayFingerprint: the changes name each entity, component and asset that differs, at most the limit" * doctest::skip(true))
		{
			ReplayFingerprint recorded;
			recorded.Entities = {
				{ .Entity = UUID(0x11), .Path = "/Camera", .Hash = "00000000000000a1", .Components = { { .Component = "Transform", .Hash = "00000000000000b1" }, { .Component = "Camera", .Hash = "00000000000000c1" } } },
				{ .Entity = UUID(0x22), .Path = "/Board", .Hash = "00000000000000a2", .Components = { { .Component = "Transform", .Hash = "00000000000000b2" } } },
			};
			recorded.Assets = {
				{ .Handle = UUID(0x515), .Path = "Assets/Scripts/Board.luau", .Type = "Script", .Hash = "00000000000000d1" },
				{ .Handle = UUID(0x606), .Path = "Assets/Materials/Red.material", .Type = "Material", .Hash = "00000000000000e1" },
			};
			CHECK(DescribeReplayFingerprintChanges(recorded, recorded).empty());

			ReplayFingerprint observed = recorded;
			observed.Entities[0].Components[1].Hash = "00000000000000c2";
			observed.Entities.erase(observed.Entities.begin() + 1);
			observed.Entities.push_back({ .Entity = UUID(0x33), .Path = "/New", .Hash = "00000000000000a3", .Components = { { .Component = "Transform", .Hash = "00000000000000b3" } } });
			observed.Assets[0].Hash = "00000000000000d2";
			observed.Assets.push_back({ .Handle = UUID(0x707), .Path = "Assets/Materials/Gold.material", .Type = "Material", .Hash = "00000000000000f1" });

			const std::vector<std::string> expected = {
				"entity /Board (0000000000000022) was removed",
				"entity /New (0000000000000033) was added",
				"component Camera of /Camera (0000000000000011) changed",
				"Script Assets/Scripts/Board.luau (0000000000000515) changed",
				"Material Assets/Materials/Gold.material (0000000000000707) is now referenced",
			};
			CHECK(DescribeReplayFingerprintChanges(recorded, observed) == expected);
			const std::vector<std::string> limited = { expected[0], expected[1], "... and 3 more" };
			CHECK(DescribeReplayFingerprintChanges(recorded, observed, 3) == limited);
		}
	}

}
