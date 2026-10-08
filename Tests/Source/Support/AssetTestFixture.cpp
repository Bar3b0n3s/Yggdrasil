#include "TestsPCH.h"
#include "Support/AssetTestFixture.h"

#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Graphics/Image.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

namespace Engine {

	namespace Test {

		AssetTestFixture::AssetTestFixture(uint64_t seed, uint32_t workerCount)
			: AssetTestFixture(AssetTestFixtureOptions{ .Seed = seed, .WorkerCount = workerCount })
		{
		}

		AssetTestFixture::AssetTestFixture(const AssetTestFixtureOptions& options)
			: m_JobSystem(options.WorkerCount, m_MainThreadQueue), m_Registry(CreateBuiltinRegistry()), m_Generator(UUIDGenerator::CreateRandom(options.Seed))
		{
			REQUIRE(m_Vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
			REQUIRE(m_Vfs.Mount("cache", CreateScope<MemoryMount>()).has_value());
			REQUIRE(m_Vfs.CreateDirectories(ProjectPath("Assets")).has_value());
			if (options.EngineResources)
			{
				REQUIRE(m_Vfs.Mount("engine", CreateScope<NativeDirectoryMount>(GetRepositoryRoot() / "Resources", MountAccess::ReadOnly)).has_value());
				REQUIRE(m_Vfs.Mount("enginecache", CreateScope<MemoryMount>()).has_value());
			}
			RegisterBuiltinImporters(m_Importers);
			RegisterBuiltinLoaders(m_Loaders);
			m_Manager = CreateScope<EditorAssetManager>(EditorAssetManagerSpecification{
				.Vfs = &m_Vfs,
				.Jobs = &m_JobSystem,
				.MainThread = &m_MainThreadQueue,
				.Events = &m_EventLog,
				.Registry = m_Registry.get(),
				.IdGenerator = &m_Generator,
				.Importers = &m_Importers,
				.Loaders = &m_Loaders,
				.EnvironmentBaker = options.EnvironmentBaker,
				.ScriptDiagnostics = nullptr,
				.EngineAssetGenerators = options.EngineAssetGenerators,
			});
		}

		AssetTestFixture::~AssetTestFixture()
		{
			// The manager uses every service; it goes first.
			m_Manager.reset();
		}

		VfsPath AssetTestFixture::ProjectPath(std::string_view relative) const
		{
			Result<VfsPath> path = VfsPath::Create("project", relative);
			REQUIRE_MESSAGE(path.has_value(), path.error().ToString());
			return std::move(*path);
		}

		void AssetTestFixture::WriteProjectFile(std::string_view relative, std::span<const std::byte> bytes)
		{
			const VfsPath path = ProjectPath(relative);
			REQUIRE(m_Vfs.CreateDirectories(path.GetParent()).has_value());
			const Status written = m_Vfs.WriteFileAtomic(path, bytes);
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
		}

		void AssetTestFixture::WriteProjectText(std::string_view relative, std::string_view text)
		{
			WriteProjectFile(relative, AsBytes(text));
		}

		Buffer AssetTestFixture::ReadProjectFile(std::string_view relative)
		{
			Result<Buffer> bytes = m_Vfs.ReadFile(ProjectPath(relative));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

		AssetRefreshReport AssetTestFixture::OpenProject(bool hotReload)
		{
			Result<AssetRefreshReport> report = m_Manager->OpenProject({
				.AssetsRoot = ProjectPath("Assets"),
				.CacheRoot = ParseVfsPath("cache://"),
				.ReadOnly = false,
				.HotReload = hotReload,
			});
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			return std::move(*report);
		}

		VfsPath ParseVfsPath(std::string_view text)
		{
			Result<VfsPath> path = VfsPath::Parse(text);
			REQUIRE_MESSAGE(path.has_value(), path.error().ToString());
			return std::move(*path);
		}

		Buffer MakeTestPng(uint32_t width, uint32_t height, uint32_t seed)
		{
			Result<Image> image = CreateImage(width, height, nvrhi::Format::RGBA8_UNORM);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			for (uint32_t y = 0; y < height; ++y)
			{
				for (uint32_t x = 0; x < width; ++x)
				{
					const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
					image->Pixels[offset + 0] = static_cast<std::byte>((x * 37 + seed) & 0xFF);
					image->Pixels[offset + 1] = static_cast<std::byte>((y * 59 + seed * 3) & 0xFF);
					image->Pixels[offset + 2] = static_cast<std::byte>(((x ^ y) * 11 + seed * 7) & 0xFF);
					image->Pixels[offset + 3] = std::byte{ 0xFF };
				}
			}
			Result<Buffer> png = EncodePng(*image);
			REQUIRE_MESSAGE(png.has_value(), png.error().ToString());
			return std::move(*png);
		}

	}

}
