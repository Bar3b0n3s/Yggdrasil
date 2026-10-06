#include "TestsPCH.h"

#include "Engine/Core/VirtualFileSystem.h"

#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/Mounts/OverlayMount.h"
#include "Support/TempDirectory.h"

#include <atomic>
#include <functional>
#include <thread>

namespace Engine {

	namespace {

		// Threads that run `body` in a loop until the group is destroyed. The destructor stops and joins them, so a
		// REQUIRE that leaves the test early never destroys a joinable std::thread.
		class LoopingThreads
		{
		public:
			LoopingThreads(int count, std::function<void()> body)
				: m_Body(std::move(body))
			{
				for (int index = 0; index < count; ++index)
				{
					m_Threads.emplace_back([this]()
					{
						while (!m_Stop.load())
							m_Body();
					});
				}
			}

			~LoopingThreads()
			{
				m_Stop.store(true);
				for (std::thread& thread : m_Threads)
					thread.join();
			}

			LoopingThreads(const LoopingThreads&) = delete;
			LoopingThreads& operator=(const LoopingThreads&) = delete;
		private:
			std::function<void()> m_Body;
			std::atomic<bool> m_Stop = false;
			std::vector<std::thread> m_Threads;
		};

	}

	template<typename T>
	static std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
	{
		return result.has_value() ? std::nullopt : std::optional<ErrorCode>(result.error().GetCode());
	}

	static VfsPath Path(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE(path.has_value());
		return std::move(*path);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("VirtualFileSystem: case mismatch is a validation error")
		{
			Test::TempDirectory directory("VfsCase");
			REQUIRE(FileSystem::CreateDirectories(directory / "Assets/Scenes").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Assets/Scenes/Level1.scene", AsBytes("{}")).has_value());

			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", CreateScope<NativeDirectoryMount>(directory.GetPath())).has_value());

			CHECK(vfs.ReadText(Path("project://Assets/Scenes/Level1.scene")) == std::string("{}"));

			const Result<std::string> wrongCase = vfs.ReadText(Path("project://Assets/scenes/level1.scene"));
			REQUIRE_FALSE(wrongCase.has_value());
			CHECK(wrongCase.error().GetCode() == ErrorCode::Validation);
			CHECK(wrongCase.error().GetMessageText().contains("case mismatch"));
			CHECK_FALSE(vfs.Exists(Path("project://Assets/scenes/level1.scene")));

			CHECK(ErrorCodeOf(vfs.WriteFileAtomic(Path("project://assets/Scenes/New.scene"), AsBytes("{}"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(vfs.WriteFileAtomic(Path("project://Assets/Scenes/level1.scene"), AsBytes("[]")))
				== ErrorCode::Validation);
			CHECK(vfs.ReadText(Path("project://Assets/Scenes/Level1.scene")) == std::string("{}"));
			CHECK(ErrorCodeOf(vfs.ReadFile(Path("project://Assets/Scenes/Level2.scene"))) == ErrorCode::NotFound);
		}

		TEST_CASE("VirtualFileSystem: reads concurrent with Unmount and Mount see a whole mount or NotFound")
		{
			const VfsPath level = Path("project://Level.scene");
			VirtualFileSystem vfs;
			Scope<MemoryMount> project = CreateScope<MemoryMount>();
			REQUIRE(project->WriteFileAtomic(level, AsBytes("disk")).has_value());
			REQUIRE(vfs.Mount("project", std::move(project)).has_value());

			// Readers hammer project:// while this thread swaps it for an overlay and back, as a dry run does.
			std::atomic<uint32_t> unexpected = 0;
			{
				const LoopingThreads readers(4, [&vfs, &level, &unexpected]()
				{
					const Result<std::string> text = vfs.ReadText(level);
					const bool isWholeMount = text.has_value() && (*text == "disk" || *text == "overlay");
					const bool isUnmounted = !text.has_value() && text.error().GetCode() == ErrorCode::NotFound;
					if (!isWholeMount && !isUnmounted)
						unexpected.fetch_add(1);
				});

				for (int swap = 0; swap < 500; ++swap)
				{
					Result<Scope<IMount>> lower = vfs.Unmount("project");
					REQUIRE(lower.has_value());
					Scope<OverlayMount> overlay = CreateScope<OverlayMount>(std::move(*lower));
					OverlayMount* overlayView = overlay.get();
					REQUIRE(overlay->WriteFileAtomic(level, AsBytes("overlay")).has_value());
					REQUIRE(vfs.Mount("project", std::move(overlay)).has_value());

					Result<Scope<IMount>> wrapped = vfs.Unmount("project");
					REQUIRE(wrapped.has_value());
					REQUIRE(wrapped->get() == overlayView);
					REQUIRE(vfs.Mount("project", overlayView->ReleaseLower()).has_value());
				}
			}

			CHECK(unexpected.load() == 0);
			CHECK(vfs.ReadText(level) == std::string("disk"));
		}

		TEST_CASE("VirtualFileSystem: routes each scheme to its mount")
		{
			VirtualFileSystem vfs;
			Scope<MemoryMount> engine = CreateScope<MemoryMount>();
			REQUIRE(engine->WriteFileAtomic(Path("engine://Version.txt"), AsBytes("engine")).has_value());
			Scope<MemoryMount> project = CreateScope<MemoryMount>();
			REQUIRE(project->WriteFileAtomic(Path("project://Version.txt"), AsBytes("project")).has_value());

			REQUIRE(vfs.Mount("engine", std::move(engine)).has_value());
			REQUIRE(vfs.Mount("project", std::move(project)).has_value());

			CHECK(vfs.ReadText(Path("engine://Version.txt")) == std::string("engine"));
			CHECK(vfs.ReadText(Path("project://Version.txt")) == std::string("project"));
			CHECK(vfs.GetSchemes() == std::vector<std::string>{ "engine", "project" });
			CHECK(vfs.IsMounted("engine"));
			CHECK_FALSE(vfs.IsMounted("user"));

			REQUIRE(vfs.WriteFileAtomic(Path("project://Saved.txt"), AsBytes("saved")).has_value());
			const Result<std::vector<VfsEntry>> listed = vfs.List(Path("project://"));
			REQUIRE(listed.has_value());
			REQUIRE(listed->size() == 2);
			CHECK((*listed)[0].Path.ToString() == "project://Saved.txt");
			CHECK((*listed)[1].Path.ToString() == "project://Version.txt");
			CHECK((*listed)[1].Info.Size == 7);
		}

		TEST_CASE("VirtualFileSystem: unmounted schemes and the empty path are NotFound")
		{
			VirtualFileSystem vfs;
			CHECK(ErrorCodeOf(vfs.ReadFile(Path("user://Settings.json"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(vfs.ReadFile(VfsPath())) == ErrorCode::NotFound);
			CHECK_FALSE(vfs.Exists(Path("user://Settings.json")));
			CHECK(ErrorCodeOf(vfs.ReadText(Path("user://Settings.json"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(vfs.Open(Path("user://Settings.json"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(vfs.WriteFileAtomic(Path("user://Settings.json"), AsBytes("{}"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(vfs.List(Path("user://"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(vfs.CreateDirectories(Path("user://Logs"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(vfs.Remove(Path("user://Logs"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(vfs.Move(Path("user://A"), Path("user://B"))) == ErrorCode::NotFound);
		}

		TEST_CASE("VirtualFileSystem: forwards directory operations to the mount")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("user", CreateScope<MemoryMount>()).has_value());
			REQUIRE(vfs.CreateDirectories(Path("user://Logs/Old")).has_value());
			CHECK(vfs.Exists(Path("user://Logs/Old")));
			const Result<FileInfo> info = vfs.GetInfo(Path("user://Logs"));
			REQUIRE(info.has_value());
			CHECK(info->IsDirectory);

			REQUIRE(vfs.Remove(Path("user://Logs")).has_value());
			CHECK_FALSE(vfs.Exists(Path("user://Logs/Old")));
			CHECK(ErrorCodeOf(vfs.Remove(Path("user://"))) == ErrorCode::InvalidArgument);
		}

		TEST_CASE("VirtualFileSystem: Mount rejects a duplicate scheme and Unmount returns the mount")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("cache", CreateScope<MemoryMount>()).has_value());
			CHECK(ErrorCodeOf(vfs.Mount("cache", CreateScope<MemoryMount>())) == ErrorCode::AlreadyExists);
			CHECK(ErrorCodeOf(vfs.Mount("Cache", CreateScope<MemoryMount>())) == ErrorCode::Validation);

			Result<Scope<IMount>> unmounted = vfs.Unmount("cache");
			REQUIRE(unmounted.has_value());
			CHECK(*unmounted != nullptr);
			CHECK_FALSE(vfs.IsMounted("cache"));
			CHECK(ErrorCodeOf(vfs.Unmount("cache")) == ErrorCode::NotFound);

			REQUIRE(vfs.Mount("cache", std::move(*unmounted)).has_value());
			CHECK(vfs.IsMounted("cache"));
		}

		TEST_CASE("VirtualFileSystem: ReadText validates UTF-8")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
			const std::array<std::byte, 2> invalid = { std::byte{ 0xc3 }, std::byte{ 0x28 } };
			REQUIRE(vfs.WriteFileAtomic(Path("project://Bad.txt"), invalid).has_value());

			CHECK(vfs.ReadFile(Path("project://Bad.txt")).has_value());
			CHECK(ErrorCodeOf(vfs.ReadText(Path("project://Bad.txt"))) == ErrorCode::Validation);
		}

		TEST_CASE("VirtualFileSystem: Move within a scheme succeeds and across schemes is InvalidArgument")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
			REQUIRE(vfs.Mount("cache", CreateScope<MemoryMount>()).has_value());
			REQUIRE(vfs.WriteFileAtomic(Path("project://A.txt"), AsBytes("a")).has_value());

			CHECK(ErrorCodeOf(vfs.Move(Path("project://A.txt"), Path("cache://A.txt"))) == ErrorCode::InvalidArgument);
			REQUIRE(vfs.Move(Path("project://A.txt"), Path("project://B.txt")).has_value());
			CHECK_FALSE(vfs.Exists(Path("project://A.txt")));
			CHECK(vfs.ReadText(Path("project://B.txt")) == std::string("a"));
		}

		TEST_CASE("VirtualFileSystem: Open streams a file in chunks")
		{
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
			REQUIRE(vfs.WriteFileAtomic(Path("project://Clip.wav"), AsBytes("0123456789")).has_value());

			Result<Scope<IFileStream>> opened = vfs.Open(Path("project://Clip.wav"));
			REQUIRE(opened.has_value());
			IFileStream& stream = **opened;
			CHECK(stream.GetSize() == 10);

			std::array<std::byte, 4> chunk{};
			CHECK(stream.Read(chunk) == size_t{ 4 });
			CHECK(AsStringView(chunk) == "0123");
			REQUIRE(stream.Seek(8).has_value());
			CHECK(stream.Read(chunk) == size_t{ 2 });
			CHECK(stream.Read(chunk) == size_t{ 0 });
			CHECK(stream.GetPosition() == 10);
			CHECK(ErrorCodeOf(stream.Seek(11)) == ErrorCode::InvalidArgument);
		}
	}

}
