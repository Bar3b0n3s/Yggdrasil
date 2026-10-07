#include "TestsPCH.h"

#include "Engine/Asset/RuntimeAssetManager.h"

#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/BuiltinTextures.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/PakWriter.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/RingBufferSink.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	namespace {

		constexpr AssetHandle TrackMesh{ 0x77e1a0c4d2b95f01ull };
		constexpr AssetHandle TrackTexture{ 0x77e1a0c4d2b95f02ull };
		constexpr AssetHandle TestTexture{ 0x0000000000000190ull };
		constexpr AssetHandle Music{ 0x5a5a5a5a5a5a5a5aull };

		Buffer MakeGamePak(bool corruptMesh)
		{
			PakWriter writer;
			REQUIRE(writer.Add({ .Handle = TrackMesh, .Type = "Mesh", .Path = "Assets/Models/Track.glb#mesh:0:Straight", .Data = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Plane), 1) })
					.has_value());
			Buffer pak = writer.Build();
			if (corruptMesh)
			{
				// Flip a byte in the middle of the only entry's data (it starts right after the 64-byte header).
				pak[64 + 100] ^= std::byte{ 0x10 };
			}
			return pak;
		}

		Ref<const PakReader> OpenPak(Buffer bytes, std::string name)
		{
			Result<Ref<const PakReader>> reader = PakReader::OpenMemory(std::move(bytes), std::move(name));
			REQUIRE_MESSAGE(reader.has_value(), reader.error().ToString());
			return *reader;
		}

		// A pak of the entries `add` adds, opened.
		template<typename Add>
		Ref<const PakReader> MakePak(std::string name, Add&& add)
		{
			PakWriter writer;
			add(writer);
			return OpenPak(writer.Build(), std::move(name));
		}

		// The manager's services for one test.
		struct RuntimeEnvironment
		{
			MainThreadQueue Queue;
			JobSystem Jobs;
			Scope<TypeRegistry> Registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry Loaders;

			explicit RuntimeEnvironment(uint32_t workerCount = 0)
				: Jobs(workerCount, Queue)
			{
				RegisterBuiltinLoaders(Loaders);
			}

			[[nodiscard]] RuntimeAssetManagerSpecification GetSpecification()
			{
				return { .Jobs = &Jobs, .MainThread = &Queue, .Registry = Registry.get(), .Loaders = &Loaders };
			}
		};

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("RuntimeAssetManager: serves cooked assets and built-ins from paks")
		{
			RuntimeEnvironment environment;
			RuntimeAssetManager manager(environment.GetSpecification());
			Result<Ref<const PakReader>> pak = PakReader::OpenMemory(MakeGamePak(false), "Game.pak");
			REQUIRE(pak.has_value());
			REQUIRE(manager.AddPak(*pak).has_value());

			CHECK(manager.Resolve("Assets/Models/Track.glb#mesh:0:Straight") == TrackMesh);
			CHECK(manager.Resolve(TrackMesh.ToString()) == TrackMesh);
			CHECK(manager.GetAssetType(TrackMesh) == AssetType::Mesh);
			CHECK(manager.GetReferencePath(TrackMesh) == "Assets/Models/Track.glb#mesh:0:Straight");
			CHECK(manager.GetMetadata(TrackMesh) == nullptr);
			Result<AssetRef<Asset>> loaded = manager.Load(TrackMesh);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK(AssetCast<MeshData>(*loaded) != nullptr);
			CHECK(manager.GetState(TrackMesh) == AssetState::Loaded);
			CHECK(manager.GetVersion(TrackMesh) == 1);

			// Procedural built-ins need no pak entry.
			CHECK(manager.Resolve("engine://Meshes/Cube") == BuiltinAssetHandles::CubeMesh);
			CHECK(manager.Load(BuiltinAssetHandles::CubeMesh).has_value());

			// The same pak twice: every handle is already served.
			Result<Ref<const PakReader>> again = PakReader::OpenMemory(MakeGamePak(false), "Again.pak");
			REQUIRE(again.has_value());
			const Status added = manager.AddPak(*again);
			REQUIRE_FALSE(added.has_value());
			CHECK(added.error().GetCode() == ErrorCode::AlreadyExists);
		}

		TEST_CASE("RuntimeAssetManager: a corrupted entry yields the placeholder and one diagnostic")
		{
			RuntimeEnvironment environment;
			RuntimeAssetManager manager(environment.GetSpecification());
			Result<Ref<const PakReader>> pak = PakReader::OpenMemory(MakeGamePak(true), "Game.pak");
			REQUIRE(pak.has_value());
			REQUIRE(manager.AddPak(*pak).has_value());

			Test::ExpectLog expected(LogLevel::Error, "corrupted");
			const AssetRef<MeshData> mesh = manager.GetOrPlaceholder<MeshData>(TrackMesh);
			REQUIRE(mesh != nullptr);
			CHECK(mesh.get() == AssetCast<MeshData>(manager.GetPlaceholder(AssetType::Mesh)).get());
			static_cast<void>(manager.GetOrPlaceholder<MeshData>(TrackMesh));
			CHECK(expected.GetMatchCount() == 1);
			CHECK(manager.GetState(TrackMesh) == AssetState::Failed);
			REQUIRE(manager.GetDiagnostics().size() == 1);
			CHECK(manager.GetDiagnostics().front().Code == AssetImportFailedCode);
		}

		TEST_CASE("RuntimeAssetManager: LoadAsync publishes on the main thread")
		{
			RuntimeEnvironment environment;
			RuntimeAssetManager manager(environment.GetSpecification());
			Result<Ref<const PakReader>> pak = PakReader::OpenMemory(MakeGamePak(false), "Game.pak");
			REQUIRE(pak.has_value());
			REQUIRE(manager.AddPak(*pak).has_value());
			JobHandle<AssetRef<Asset>> job = manager.LoadAsync(TrackMesh);
			manager.WaitIdle();
			REQUIRE(job.IsReady());
			CHECK(job.Take().has_value());
			CHECK(manager.GetState(TrackMesh) == AssetState::Loaded);
		}

		TEST_CASE("RuntimeAssetManager: the state is Loading until the main thread publishes")
		{
			RuntimeEnvironment environment;
			RuntimeAssetManager manager(environment.GetSpecification());
			REQUIRE(manager.AddPak(OpenPak(MakeGamePak(false), "Game.pak")).has_value());
			// The inline job system decodes inside LoadAsync; the result waits in the main-thread queue.
			JobHandle<AssetRef<Asset>> job = manager.LoadAsync(TrackMesh);
			CHECK(job.IsReady());
			CHECK(manager.GetState(TrackMesh) == AssetState::Loading);
			CHECK(manager.GetVersion(TrackMesh) == 0);
			CHECK(environment.Queue.Drain() == 1);
			CHECK(manager.GetState(TrackMesh) == AssetState::Loaded);
			CHECK(manager.GetVersion(TrackMesh) == 1);
			// A loaded asset completes at once, with the same object.
			JobHandle<AssetRef<Asset>> again = manager.LoadAsync(TrackMesh);
			Result<AssetRef<Asset>> first = job.Take();
			Result<AssetRef<Asset>> second = again.Take();
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(first->get() == second->get());
			CHECK(manager.GetVersion(TrackMesh) == 1);
		}

		TEST_CASE("RuntimeAssetManager: Engine.pak and Game.pak are served together, and Resolve accepts every spelling")
		{
			RuntimeEnvironment environment;
			RuntimeAssetManager manager(environment.GetSpecification());
			const std::string spirv = "spirv";
			const Ref<const PakReader> enginePak = MakePak("Engine.pak", [&spirv](PakWriter& writer)
			{
				REQUIRE(writer.Add({ .Handle = TestTexture, .Type = "Texture", .Path = "engine://Textures/Test", .Data = CookTexture(GenerateBuiltinTexture(BuiltinTexture::Checker), 1) })
						.has_value());
				REQUIRE(writer.Add({ .Handle = AssetHandle(), .Type = "File", .Path = "Shaders/A.spv", .Data = Buffer(AsBytes(spirv).begin(), AsBytes(spirv).end()) })
						.has_value());
			});
			REQUIRE(manager.AddPak(enginePak).has_value());
			REQUIRE(manager.AddPak(OpenPak(MakeGamePak(false), "Game.pak")).has_value());

			CHECK(manager.Resolve("engine://Textures/Test") == TestTexture);
			CHECK(manager.Resolve("project://Assets/Models/Track.glb#mesh:0:Straight") == TrackMesh);
			CHECK(manager.Resolve("77E1A0C4D2B95F01") == TrackMesh);
			CHECK(manager.Resolve("Assets/Models/Track.glb#mesh:1") == std::nullopt);
			CHECK(manager.Resolve("assets/Models/Track.glb#mesh:0:Straight") == std::nullopt);
			CHECK(manager.Resolve(TrackTexture.ToString()) == std::nullopt);
			CHECK(manager.Resolve("not a reference") == std::nullopt);
			// Plain files are PakMount's, not assets.
			CHECK(manager.Resolve("Shaders/A.spv") == std::nullopt);
			CHECK(manager.GetReferencePath(BuiltinAssetHandles::CubeMesh) == "engine://Meshes/Cube");
			CHECK(manager.GetAssetType(BuiltinAssetHandles::MissingTexture) == AssetType::Texture);
			CHECK(manager.GetAssetType(TrackTexture) == AssetType::None);
			CHECK(manager.GetReferencePath(TrackTexture).empty());
			CHECK(manager.GetState(TrackTexture) == AssetState::Unloaded);

			Result<AssetRef<Asset>> texture = manager.Load(TestTexture);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());
			CHECK(AssetCast<TextureData>(*texture) != nullptr);
		}

		TEST_CASE("RuntimeAssetManager: a refused pak adds nothing")
		{
			RuntimeEnvironment environment;
			RuntimeAssetManager manager(environment.GetSpecification());
			REQUIRE(manager.AddPak(OpenPak(MakeGamePak(false), "Game.pak")).has_value());
			// A new handle at a path the first pak serves: refused as a whole, naming both paks.
			const Ref<const PakReader> conflicting = MakePak("Patch.pak", [](PakWriter& writer)
			{
				REQUIRE(writer.Add({ .Handle = TrackTexture, .Type = "Texture", .Path = "Assets/Models/Track.png", .Data = CookTexture(GenerateBuiltinTexture(BuiltinTexture::White), 1) })
						.has_value());
				REQUIRE(writer.Add({ .Handle = AssetHandle(0x1234), .Type = "Mesh", .Path = "Assets/Models/Track.glb#mesh:0:Straight", .Data = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Cube), 1) })
						.has_value());
			});
			const Status added = manager.AddPak(conflicting);
			REQUIRE_FALSE(added.has_value());
			CHECK(added.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK(added.error().ToString().contains("Patch.pak"));
			CHECK(added.error().ToString().contains("Game.pak"));
			CHECK(manager.Resolve("Assets/Models/Track.png") == std::nullopt);
			CHECK(manager.GetAssetType(TrackTexture) == AssetType::None);
		}

		TEST_CASE("RuntimeAssetManager: null, unknown and unloadable handles fail with their codes")
		{
			RuntimeEnvironment environment;
			RuntimeAssetManager manager(environment.GetSpecification());
			const std::string audio = "RIFF";
			const Ref<const PakReader> gamePak = MakePak("Game.pak", [&audio](PakWriter& writer)
			{
				REQUIRE(writer.Add({ .Handle = Music, .Type = "AudioClip", .Path = "Assets/Audio/Music.wav", .Data = WriteCookedArtifact(AssetType::AudioClip, 1, 1, AsBytes(audio)) })
						.has_value());
			});
			REQUIRE(manager.AddPak(gamePak).has_value());

			Result<AssetRef<Asset>> null = manager.Load(AssetHandle());
			REQUIRE_FALSE(null.has_value());
			CHECK(null.error().GetCode() == ErrorCode::InvalidArgument);
			Result<AssetRef<Asset>> unknown = manager.Load(TrackTexture);
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			CHECK(manager.GetDiagnostics().empty());

			// A type this build has no loader for (audio clips arrive with M12) is served and fails when loaded, once.
			CHECK(manager.GetAssetType(Music) == AssetType::AudioClip);
			Test::ExpectLog expected(LogLevel::Error, "Assets/Audio/Music.wav");
			Result<AssetRef<Asset>> clip = manager.Load(Music);
			REQUIRE_FALSE(clip.has_value());
			CHECK(clip.error().GetCode() == ErrorCode::Unsupported);
			CHECK(clip.error().ToString().contains(Music.ToString()));
			Result<AssetRef<Asset>> remembered = manager.Load(Music);
			REQUIRE_FALSE(remembered.has_value());
			CHECK(remembered.error().GetCode() == ErrorCode::Unsupported);
			CHECK(manager.GetState(Music) == AssetState::Failed);
			CHECK(expected.GetMatchCount() == 1);
			REQUIRE(manager.GetDiagnostics().size() == 1);
			CHECK(manager.GetDiagnostics().front().Path == "Assets/Audio/Music.wav");
		}

		TEST_CASE("RuntimeAssetManager: asynchronous loads on workers publish their results and failures")
		{
			RuntimeEnvironment environment(3);
			RuntimeAssetManager manager(environment.GetSpecification());
			REQUIRE(manager.AddPak(OpenPak(MakeGamePak(true), "Game.pak")).has_value());
			const Ref<const PakReader> enginePak = MakePak("Engine.pak", [](PakWriter& writer)
			{
				REQUIRE(writer.Add({ .Handle = TestTexture, .Type = "Texture", .Path = "engine://Textures/Test", .Data = CookTexture(GenerateBuiltinTexture(BuiltinTexture::Checker), 1) })
						.has_value());
			});
			REQUIRE(manager.AddPak(enginePak).has_value());

			Test::ExpectLog expected(LogLevel::Error, "corrupted");
			JobHandle<AssetRef<Asset>> texture = manager.LoadAsync(TestTexture);
			JobHandle<AssetRef<Asset>> mesh = manager.LoadAsync(TrackMesh);
			JobHandle<AssetRef<Asset>> cube = manager.LoadAsync(BuiltinAssetHandles::CubeMesh);
			manager.WaitIdle();
			CHECK(texture.Take().has_value());
			Result<AssetRef<Asset>> failed = mesh.Take();
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Validation);
			CHECK(cube.Take().has_value());
			CHECK(manager.GetState(TestTexture) == AssetState::Loaded);
			CHECK(manager.GetState(TrackMesh) == AssetState::Failed);
			CHECK(manager.GetState(BuiltinAssetHandles::CubeMesh) == AssetState::Loaded);
			CHECK(manager.GetVersion(TestTexture) == 1);
			CHECK(expected.GetMatchCount() == 1);
		}
	}

}
