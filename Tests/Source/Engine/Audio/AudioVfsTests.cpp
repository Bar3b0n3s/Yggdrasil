#include "TestsPCH.h"

#include "Engine/Audio/AudioVfs.h"

#include "Support/AssetTestFixture.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The VFS bridge of the audio module (Architecture §10.1; Docs/Decisions/0015-m12-decisions.md).

namespace Engine {

	// Every byte of `stream`, read in chunks of `chunk` bytes; fails the test case on error.
	static Buffer ReadAll(IFileStream& stream, size_t chunk = 7)
	{
		Buffer bytes;
		std::vector<std::byte> buffer(chunk);
		for (;;)
		{
			const Result<size_t> read = stream.Read(buffer);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			if (*read == 0)
				return bytes;
			bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));
		}
	}

	// The error code of a failed result; nullopt for a success.
	template<typename T>
	static std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
	{
		if (result.has_value())
			return std::nullopt;
		return result.error().GetCode();
	}

	TEST_SUITE("Audio")
	{
		TEST_CASE("AudioVfs: a memory file is served by name and shares its bytes")
		{
			VirtualFileSystem vfs;
			AudioVfs audioVfs(vfs);
			const Ref<const Buffer> bytes = CreateRef<const Buffer>(Buffer{ std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 } });
			REQUIRE(audioVfs.AddMemoryFile("00000000000000aa", bytes).has_value());
			CHECK(audioVfs.HasMemoryFile("00000000000000aa"));
			CHECK(audioVfs.GetMemoryFileCount() == 1);
			// Shared, never copied: the engine keeps the clip's bytes alive through this reference.
			CHECK(bytes.use_count() > 1);

			Result<Scope<IFileStream>> stream = audioVfs.Open("00000000000000aa");
			REQUIRE_MESSAGE(stream.has_value(), stream.error().ToString());
			CHECK((*stream)->GetSize() == 4);
			const Buffer whole = ReadAll(**stream, 3);
			CHECK(whole == *bytes);
			REQUIRE((*stream)->Seek(2).has_value());
			CHECK((*stream)->GetPosition() == 2);
			const Buffer tail = ReadAll(**stream);
			CHECK(tail == Buffer{ std::byte{ 3 }, std::byte{ 4 } });
			CHECK(ErrorCodeOf((*stream)->Seek(5)) == ErrorCode::InvalidArgument);
		}

		TEST_CASE("AudioVfs: other names are opened through the engine VFS")
		{
			Test::AssetTestFixture fixture;
			const Buffer bytes = { std::byte{ 9 }, std::byte{ 8 }, std::byte{ 7 } };
			fixture.WriteProjectFile("Assets/Audio/Music.ogg", bytes);
			AudioVfs audioVfs(fixture.GetVfs());
			Result<Scope<IFileStream>> stream = audioVfs.Open("project://Assets/Audio/Music.ogg");
			REQUIRE_MESSAGE(stream.has_value(), stream.error().ToString());
			const Buffer read = ReadAll(**stream);
			CHECK(read == bytes);
			CHECK(ErrorCodeOf(audioVfs.Open("project://Assets/Audio/Missing.ogg")) == ErrorCode::NotFound);
			// The VFS's case policy applies (§4.10).
			CHECK(ErrorCodeOf(audioVfs.Open("project://Assets/Audio/music.ogg")) == ErrorCode::Validation);
		}

		TEST_CASE("AudioVfs: invalid, duplicate and unknown names are rejected")
		{
			VirtualFileSystem vfs;
			AudioVfs audioVfs(vfs);
			const Ref<const Buffer> bytes = CreateRef<const Buffer>(Buffer(16));
			CHECK(ErrorCodeOf(audioVfs.AddMemoryFile("", bytes)) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(audioVfs.AddMemoryFile("project://Assets/A.wav", bytes)) == ErrorCode::InvalidArgument);
			REQUIRE(audioVfs.AddMemoryFile("0000000000000001", bytes).has_value());
			CHECK(ErrorCodeOf(audioVfs.AddMemoryFile("0000000000000001", bytes)) == ErrorCode::AlreadyExists);
			CHECK(ErrorCodeOf(audioVfs.RemoveMemoryFile("0000000000000002")) == ErrorCode::NotFound);
			// Neither a memory file nor a VFS path; and a path whose scheme is not mounted.
			CHECK(ErrorCodeOf(audioVfs.Open("0000000000000002")) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(audioVfs.Open("nowhere://Assets/A.wav")) == ErrorCode::NotFound);
			CHECK(audioVfs.GetMemoryFileCount() == 1);
		}

		TEST_CASE("AudioVfs: a removed memory file keeps its open streams")
		{
			VirtualFileSystem vfs;
			AudioVfs audioVfs(vfs);
			Ref<const Buffer> bytes = CreateRef<const Buffer>(Buffer{ std::byte{ 5 }, std::byte{ 6 } });
			REQUIRE(audioVfs.AddMemoryFile("0000000000000001", bytes).has_value());
			Result<Scope<IFileStream>> stream = audioVfs.Open("0000000000000001");
			REQUIRE(stream.has_value());
			REQUIRE(audioVfs.RemoveMemoryFile("0000000000000001").has_value());
			bytes.reset();
			CHECK_FALSE(audioVfs.HasMemoryFile("0000000000000001"));
			const Buffer kept = ReadAll(**stream);
			CHECK(kept == Buffer{ std::byte{ 5 }, std::byte{ 6 } });
			CHECK(ErrorCodeOf(audioVfs.Open("0000000000000001")) == ErrorCode::InvalidArgument);
		}
	}

}
