# Code Style

This repository follows the code style of the [Hazel engine](https://docs.hazelengine.com/HazelForEngineers/DeveloperGuide#naming): PascalCase types and functions, `m_` members, `s_` statics, camelCase locals, tabs, and braces on their own line. Hazel's developer guide only defines naming, so the formatting rules below were inferred from Hazel's source code and encoded in `.clang-format`. The goal is code that "reads like an English book", which is how the Hazel guide puts it.

The rules apply to everything under `Engine/`, `Editor/`, `Runtime/`, `Tests/`, `Tools/` and `Projects/`. Code under `Vendor/` keeps its upstream style and is never reformatted.

Some rules here are stricter than Hazel's own code. Those rules say so. Topics marked **[Architecture]** are settled by the architecture document in `Docs/`, and this guide does not invent policy for them. Until that document covers a topic, do not introduce an ad-hoc convention for it.

## Contents

1. [Tooling](#1-tooling)
2. [Naming](#2-naming)
3. [Formatting](#3-formatting)
4. [Files, includes and the PCH](#4-files-includes-and-the-pch)
5. [Namespaces](#5-namespaces)
6. [Types, const-correctness and parameters](#6-types-const-correctness-and-parameters)
7. [Ownership and lifetime](#7-ownership-and-lifetime)
8. [Casts](#8-casts)
9. [`auto`](#9-auto)
10. [Comments](#10-comments)
11. [Logging and assertions](#11-logging-and-assertions)
12. [Error handling](#12-error-handling)
13. [C++23 usage](#13-c23-usage)
14. [Unit tests (doctest)](#14-unit-tests-doctest)
15. [Other languages](#15-other-languages)
16. [Topics refined by the architecture document](#16-topics-refined-by-the-architecture-document)
17. [Why `.clang-format` is configured this way](#17-why-clang-format-is-configured-this-way)

---

## 1. Tooling

| File | Purpose |
|------|---------|
| `.clang-format` | Mechanical C++ formatting. It is the source of truth for layout. |
| `.editorconfig` | Encoding, line endings, indentation and trailing whitespace for every text file type. |

- **clang-format 22.x is the canonical formatter.** Visual Studio 2026 ships it at `VC/Tools/Llvm/x64/bin/clang-format.exe`. On Linux and macOS, install the same major version, for example with `pip install "clang-format==22.1.*"` or from apt.llvm.org. Other major versions format some constructs differently, and older ones reject options used in the config.
- Format a file in place with `clang-format -i --style=file <files>`. Check without modifying with `clang-format --dry-run -Werror --style=file <files>`. The developer script `Scripts/Format.py` provides both modes for the whole repository and must skip `Vendor/`.
- In Visual Studio, enable *Tools > Options > Text Editor > C/C++ > Code Style > Formatting > Enable ClangFormat support*. VS then picks up `.clang-format` automatically.
- Code must be clang-format clean before it is committed.
- `// clang-format off` / `// clang-format on` is allowed only around genuinely tabular data where column alignment carries meaning, such as a lookup table or a matrix literal. The pair must open and close in the same scope. Never use it to hide sloppy formatting.

## 2. Naming

| Construct | Convention | Example |
|-----------|------------|---------|
| Namespace | PascalCase | `Engine`, `Engine::Utils` |
| Class, struct, union, enum, type alias, concept | PascalCase | `SceneSerializer`, `using VoiceHandle = uint32_t;`, `concept VolumeLike` |
| Function, member function | PascalCase | `LoadScene()`, `GetWidth()` |
| Public data member of a plain struct (components, specifications, POD data) | PascalCase, no prefix | `TransformComponent::Translation`, `WindowSpecification::Width` |
| Private or protected data member | `m_` + PascalCase | `m_Registry`, `m_IsRunning` |
| Mutable static: static data member, file-scope `static`, function-local `static`, `thread_local` | `s_` + PascalCase | `s_Instance`, `s_InstanceCount` |
| Global variable with external linkage (avoid) | `g_` + PascalCase | `g_ShutdownRequested` |
| Compile-time constant (`constexpr` / `static constexpr`, at any scope) | PascalCase, no prefix | `MaxTextureSlots`, `InvalidVoice` |
| Local variable, function parameter, lambda parameter | camelCase | `entityCount`, `deltaTime`, `fileName` |
| Enum type and enumerators (always `enum class`) | PascalCase | `AudioBus::Effects` |
| Template parameter | `T`, or a descriptive PascalCase name | `typename T`, `typename... Components`, `typename Func` |
| Macro | `ENGINE_` + UPPER_SNAKE_CASE | `ENGINE_CORE_ASSERT`, `ENGINE_PLATFORM_WINDOWS` |
| Source file | PascalCase, named after the primary type | `SceneSerializer.h`, `SceneSerializer.cpp` |
| Directory | PascalCase | `Engine/Source/Engine/Renderer/`, `Projects/RollingBall/` |
| Test file | `<Unit>Tests.cpp` | `SceneSerializerTests.cpp` |

```cpp
namespace Engine {

	static uint32_t s_LiveMixerCount = 0;

	constexpr uint32_t MaxVoices = 64;

	enum class FadeCurve
	{
		Linear = 0,
		Exponential
	};

	struct FadeSpecification
	{
		float DurationSeconds = 0.5f;
		FadeCurve Curve = FadeCurve::Linear;
	};

	class VoiceFader
	{
	public:
		explicit VoiceFader(const FadeSpecification& specification);

		void Start(float targetVolume);
		bool IsFading() const { return m_IsFading; }
	private:
		FadeSpecification m_Specification;
		float m_CurrentVolume = 1.0f;
		bool m_IsFading = false;
	};

	void VoiceFader::Start(float targetVolume)
	{
		const float distance = targetVolume - m_CurrentVolume;
		m_IsFading = distance != 0.0f;
	}

}
```

Further rules:

- **Acronyms** keep their capitalization inside PascalCase names, as in Hazel: `UUID`, `GetUUID()`, `m_RendererID`, `SSAOPass`, `IBLSettings`. When an acronym starts a camelCase name it is all lowercase: `uuid`, `idMap`, `gpuBuffer`.
- **Verbs and prefixes:**
  - `GetX()` / `SetX()` for accessors.
  - `IsX()` / `HasX()` / `CanX()` for booleans. Boolean members use the same words: `m_IsPaused`.
  - `OnX()` for event and lifecycle handlers: `OnUpdate`, `OnEvent`, `OnRuntimeStart`.
  - `static Create(...)` for factories that return `Ref` or `Scope`.
  - `XToString` / `XFromString` / `AToB` for conversion helpers.
- **Descriptive names:** abbreviations are limited to universally understood ones (`ts` for a `Timestep`, `e` for an event in a handler, `i` for a loop index).
- **Standard protocols are the only exception to PascalCase functions.** Containers used with range-for or algorithms provide `begin()`, `end()`, `size()`, `data()` and similar. Specializations of `std::hash` and `std::formatter`, ADL `swap`, and callbacks whose names a third-party API dictates also keep their required names.
- **Reserved and decorated names are forbidden:** no leading or trailing underscores, no double underscores, and no Hungarian notation beyond the `m_` / `s_` / `g_` prefixes.
- **The product name never appears in code.** No namespace, identifier, macro, file name, directory name inside a source tree, shader or script symbol contains it. Code is project-agnostic and says `Engine`. A user-visible display name comes from configuration (premake workspace, project files).

## 3. Formatting

clang-format enforces everything in this section. The examples show the expected result.

### 3.1 Indentation and whitespace

- **Tabs** for indentation and for continuation lines (tab width 4). Spaces appear only for alignment inside a line.
- **Indentation scope:** everything inside a namespace is indented. Access specifiers sit at the indentation of the `class` keyword. `case` labels are indented one level inside `switch`.
- **Clean whitespace:** UTF-8, LF line endings, a final newline, and no trailing whitespace. `.editorconfig` covers these for every file type.

### 3.2 Braces

- **Allman everywhere except namespaces.** Classes, structs, enums, functions, control statements, `case` blocks and lambdas open their brace on its own line. Namespaces open on the same line.
- **Namespace bodies** start and end with one blank line. There is no closing `// namespace` comment.
- **`else`, `catch` and `while`:** `else` and `catch` start a new line, while `do { } while ();` keeps `while` after the closing brace.

```cpp
namespace Engine {

	class PhysicsWorld
	{
	public:
		void Step(float deltaTime);
	private:
		float m_Accumulator = 0.0f;
	};

	void PhysicsWorld::Step(float deltaTime)
	{
		m_Accumulator += deltaTime;
		while (m_Accumulator >= FixedTimeStep)
		{
			StepOnce();
			m_Accumulator -= FixedTimeStep;
		}

		if (m_Accumulator < 0.0f)
			m_Accumulator = 0.0f;
		else
			Interpolate(m_Accumulator / FixedTimeStep);
	}

}
```

### 3.3 Single-line forms

Allowed:

- Trivial member functions defined inside the class body: `bool IsPaused() const { return m_IsPaused; }`.
- Short `case` labels: `case AudioBus::Music: return "Music";`. clang-format aligns consecutive ones.
- An empty lambda: `[]() {}`.

Not allowed: a statement on the same line as its `if`, `for` or `while`, single-line blocks, and functions at namespace scope written on one line.

### 3.4 Braceless bodies

You may omit braces when the body of `if`, `else`, `for` or `while` is a single statement on a single line, as Hazel does. In every other case, use braces:

- If any branch of an `if` / `else if` / `else` chain needs braces, brace every branch.
- A body that spans more than one line, including a nested `if` or a wrapped statement, needs braces.

```cpp
for (const Ref<AudioClip>& clip : m_PendingClips)
	clip->Load();

if (voices.empty())
{
	ENGINE_CORE_TRACE("No voices to mix, writing silence");
	m_Output.Clear();
}
else
{
	MixVoices(voices);
}
```

### 3.5 Line length and wrapping

- **No hard column limit.** Hazel has none, and clang-format runs with `ColumnLimit: 0`, so it keeps your line breaks.
- **Soft limit of about 140 columns** at tab width 4. Wrap longer lines by hand:
  - Break after a comma, or before a binary or ternary operator.
  - Indent the continuation by one extra tab. Do not align it to the opening parenthesis.
  - Fill each line before wrapping. Do not put one parameter or argument per line.

```cpp
void Renderer::SubmitMesh(const Ref<Mesh>& mesh, const Ref<MaterialInstance>& material, const glm::mat4& transform,
	uint32_t submeshIndex, int32_t entityID)
{
	const glm::mat4 normalMatrix = glm::transpose(glm::inverse(transform))
		* glm::scale(glm::mat4(1.0f), m_GlobalScale);

	const bool visible = m_Frustum.Contains(mesh->GetBounds(submeshIndex), transform)
		&& material->GetBlendMode() != BlendMode::Hidden;
}
```

### 3.6 Constructor initializer lists

- **Placement:** the list always goes on the line after the signature, indented one tab and starting with `: `. It never shares a line with the signature.
- **Packing:** initializers go on one line when that is reasonable. If you break the list, clang-format puts one initializer per line.
- **Body:** an empty constructor body is still written as `{` and `}` on their own lines.

```cpp
AudioMixer::AudioMixer(const AudioMixerSpecification& specification)
	: m_Specification(specification), m_BusVolumes(BusCount, 1.0f)
{
}
```

### 3.7 Lambdas

- **Non-empty bodies use Allman braces.** The opening brace sits on its own line at the indentation of the statement that contains the lambda. clang-format cannot keep a non-empty lambda body on one line when there is no column limit, so short predicates are written the same way.
- **Empty lambdas** stay on one line: `[]() {}`.

```cpp
std::erase_if(m_Voices, [](const auto& entry)
{
	return entry.second.Clip == nullptr;
});

m_Window->SetEventCallback([this](Event& event)
{
	OnEvent(event);
});
```

### 3.8 `switch`

- **Variables inside a case:** a case that declares variables or has several statements gets a brace block, with `break;` inside the braces.
- **No `default:` when switching over an `enum class`,** so that the compiler's unhandled-enumerator warning (`-Wswitch`, MSVC `C4062`) can flag a missing case. Handle the impossible case after the switch (see [11](#11-logging-and-assertions)).
- **Fallthrough** between non-empty cases is marked with `[[fallthrough]];`.

```cpp
switch (state)
{
	case SceneState::Edit:
	{
		const float speed = m_EditorCamera.GetSpeed();
		m_EditorCamera.OnUpdate(ts, speed);
		break;
	}
	case SceneState::Play:
	{
		m_ActiveScene->OnUpdateRuntime(ts);
		break;
	}
}
```

### 3.9 Braced initializer lists

- **Spacing:** spaces inside the braces, `{ 0.0f, 1.0f, 0.0f }`, and no space before the brace in `Type{ ... }`.
- **Multi-line lists:** one element per line, with a **trailing comma**. The trailing comma is required: without it clang-format joins the opening line with the first element.
- **Designated initializers** are encouraged for specification structs.

```cpp
const glm::vec3 up = { 0.0f, 1.0f, 0.0f };
Entity entity{ handle, this };

layout.SetElements({
	{ VertexAttribute::Position, Format::RGB32Float },
	{ VertexAttribute::Normal, Format::RGB32Float },
	{ VertexAttribute::TexCoord, Format::RG32Float },
});

WindowSpecification specification = {
	.Title = "Editor",
	.Width = 1600,
	.Height = 900,
};
```

### 3.10 Spacing

- **Pointers and references** bind to the type: `const Ref<Texture2D>& texture`, `Entity* parent`.
- **Templates:** `template<typename T>`, with no space after `template`.
- **Keywords and calls:** control keywords get a space before the parenthesis (`if (`, `for (`, `while (`, `switch (`). Function calls and declarations get none (`Update(ts)`).
- **Range-for:** `for (const auto& [name, value] : properties)`.
- **Trailing comments:** one space before `//` and one after it. Consecutive trailing comments are aligned.

### 3.11 Blank lines

- **At most one** consecutive blank line.
- **Not at the edges of a block:** no blank line at the start or end of a block, except the one after `namespace X {` and before its closing brace.
- **No blank lines around access specifiers** (`public:`, `private:`). Inside a class, a repeated `private:` separates groups, as in [4.5](#45-class-layout).
- **Between functions:** one blank line separates function definitions.
- **Inside functions:** use blank lines to separate logical steps, as Hazel does.

### 3.12 Templates and constraints

`template<...>` always goes on its own line. A `requires` clause goes on its own line, indented one tab.

```cpp
template<typename T>
	requires std::is_trivially_copyable_v<T>
T ReadValue(std::span<const std::byte> bytes)
{
	T value{};
	std::memcpy(&value, bytes.data(), sizeof(T));
	return value;
}
```

### 3.13 Preprocessor

- **Directive indentation:** nested directives are indented with tabs before the `#`, by nesting level.
- **Macro bodies are not formatted by clang-format.** A short macro stays on one line. A long one uses backslash continuation with tab indentation. Group related macros as a table aligned with spaces.

```cpp
#if defined(ENGINE_PLATFORM_WINDOWS)
	#define ENGINE_DEBUGBREAK() __debugbreak()
#elif defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)
	#include <csignal>
	#define ENGINE_DEBUGBREAK() std::raise(SIGTRAP)
#endif
```

## 4. Files, includes and the PCH

### 4.1 Layout

- **Engine sources** live in `Engine/Source/Engine/<Module>/`. Headers are included relative to `Engine/Source`, for example `#include "Engine/Scene/Scene.h"`.
- **One primary type per file pair:** `Name.h` and `Name.cpp`. Small helper types that only serve the primary type may live in the same files.
- **Extensions:** `.h` for headers and `.cpp` for sources. `.hpp`, `.cc` and `.inl` are not used. Templates are defined in the header.
- **Platform-specific code** belongs in separate files guarded by `ENGINE_PLATFORM_*` macros. Its directory layout is **[Architecture]**.

### 4.2 Header template

```cpp
#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <vector>

namespace JPH {

	class Body;

}

namespace Engine {

	class PhysicsMaterial;
	class Scene;

	class CharacterController
	{
	public:
		explicit CharacterController(Scene* scene);

		void Move(const glm::vec3& displacement);
	private:
		Scene* m_Scene = nullptr;
		JPH::Body* m_Body = nullptr;
		Ref<PhysicsMaterial> m_Material;
		std::vector<glm::vec3> m_PendingMoves;
	};

}
```

### 4.3 Source template

```cpp
#include "EnginePCH.h"
#include "Engine/Physics/CharacterController.h"

#include "Engine/Physics/PhysicsMaterial.h"

#include <glm/geometric.hpp>

#include <algorithm>

namespace Engine {

	CharacterController::CharacterController(Scene* scene)
		: m_Scene(scene)
	{
		ENGINE_CORE_ASSERT(scene, "CharacterController needs a scene");
	}

	void CharacterController::Move(const glm::vec3& displacement)
	{
		const float friction = std::clamp(m_Material->GetFriction(), 0.0f, 1.0f);
		if (glm::length(displacement) > 0.0f)
			m_PendingMoves.push_back(displacement * (1.0f - friction));
	}

}
```

### 4.4 Include rules

1. **Include groups, most specific first,** separated by one blank line:
   1. `"EnginePCH.h"`, only in `.cpp` files of the Engine project, where it must be the first line.
   2. The file's own header.
   3. Other repository headers: `"Engine/..."`, or the Editor's, Runtime's and Tests' own.
   4. Third-party headers, in angle brackets: `<glm/glm.hpp>`, `<nvrhi/nvrhi.h>`.
   5. Standard library headers: `<vector>`.
2. **Quotes and paths:** repository headers use quotes and the full path from the include root. Never use relative paths (`"../Core/Log.h"`). Third-party and standard headers use angle brackets.
3. **Order within a group** is alphabetical unless order matters. Order matters when a configuration macro must precede a header, for example `#define GLM_ENABLE_EXPERIMENTAL`. In that case, add a comment explaining why. clang-format does not sort includes, because some orders are significant.
4. **Headers are self-contained:** each one compiles on its own and includes what it uses. It must not rely on the PCH or on a previous include. This is stricter than Hazel.
5. **Forward declarations:** prefer a forward declaration to an include when a header only needs a pointer or reference to a type.
6. **Never `#include` inside a namespace, and never include a `.cpp` file.**
7. **The PCH is for `.cpp` files only.** `EnginePCH.h` holds stable, frequently used headers: the standard library, the logging and assert macros, and large third-party headers that rarely change. Headers never include it. Whether Editor, Runtime and Tests get their own PCH is **[Architecture]**.

### 4.5 Class layout

Order sections as `public`, then `protected`, then `private`. Within a class, functions come before data. A repeated access specifier separates function groups from data groups, as in Hazel:

1. **`public:`**
   1. Nested types and aliases.
   2. Constructors and the destructor.
   3. Copy and move operations.
   4. The rest of the public API.
   5. Inline accessors.
   6. Static functions.
2. **`protected:`** if needed.
3. **`private:`** functions.
4. **`private:`** data members.
5. **`private:`** static data and `friend` declarations.

```cpp
class AudioMixer
{
public:
	using VoiceHandle = uint32_t;

	static constexpr VoiceHandle InvalidVoice = 0;

	explicit AudioMixer(const AudioMixerSpecification& specification);
	~AudioMixer();

	AudioMixer(const AudioMixer&) = delete;
	AudioMixer& operator=(const AudioMixer&) = delete;

	[[nodiscard]] VoiceHandle Play(const Ref<AudioClip>& clip, AudioBus bus);
	void Stop(VoiceHandle voice);

	uint32_t GetActiveVoiceCount() const { return static_cast<uint32_t>(m_Voices.size()); }

	static std::string_view BusToString(AudioBus bus);
private:
	VoiceHandle AllocateHandle();
private:
	AudioMixerSpecification m_Specification;
	std::unordered_map<VoiceHandle, Voice> m_Voices;
	VoiceHandle m_NextHandle = 1;
private:
	static uint32_t s_InstanceCount;
};
```

### 4.6 Inline and file-local code

- **What goes in the header:** trivial accessors are defined inline in the class body. Everything else is defined in the `.cpp`. Templates live in the header, and a long template body is defined below the class.
- **File-local helpers** in a `.cpp` are `static` functions, grouped in a nested `namespace Utils { }` block when there are several. File-local types go in an unnamed namespace, since `static` cannot apply to types.

## 5. Namespaces

- **All engine code lives in `namespace Engine`.** The product name is never used as a namespace.
- **Sub-namespaces only where clearly useful:**
  - `Utils` for helper functions.
  - `Detail` for implementation details that templates force into headers.
  - Anything else is **[Architecture]**.
- **Nesting:** sub-namespaces are written as nested blocks, and each level is indented.
- **Editor, Runtime, Tools and Tests:** their namespaces are **[Architecture]**. They must be project-agnostic as well.
- **`using namespace`:** never in a header at namespace or global scope. Never use `using namespace std;` anywhere. In a `.cpp` it is allowed inside a function body, or at file scope for literal namespaces such as `std::literals`.
- **Namespace aliases** such as `namespace fs = std::filesystem;` are allowed in `.cpp` files only.
- **`std` specializations** (`std::hash<Engine::UUID>`, `std::formatter<...>`) are written at global scope after the `Engine` namespace closes.

```cpp
namespace Engine {

	namespace Utils {

		static bool IsHiddenFile(const std::filesystem::path& path)
		{
			return path.filename().string().starts_with(".");
		}

	}

}
```

## 6. Types, const-correctness and parameters

- **Integer types:** fixed-width `uint32_t`, `int32_t`, `uint64_t` and so on, written unqualified as in Hazel. `size_t` is for sizes and container indices. `int` is fine for small local counters.
- **Literals and constants:**
  - Float literals always carry the suffix and a fractional part: `1.0f`, `0.5f`.
  - Use `nullptr`, never `NULL` or `0`.
  - Use `constexpr` for compile-time values.
  - A constant defined in a header is `inline constexpr` at namespace scope or `static constexpr` in a class.
- **West const:** `const T&` and `const T*`, never `T const&`.
- **Const member functions:** a member function that does not change observable state is `const`.
- **Const locals:** mark locals `const` when they are not modified and it helps the reader.
- **Exceptions to const-correctness:**
  - `mutable` is only for synchronization primitives and caches, with a comment.
  - `const_cast` is only for const-incorrect C APIs (see [8](#8-casts)).
- **Parameter passing:**
  - **Pass by value** for cheap types: scalars, enums, handles, `UUID`, `Timestep`, `std::string_view`, `std::span`.
  - **Pass by `const T&`** for everything else, including `std::string`, `glm` vectors and matrices, and `Ref<T>`.
  - **Sink parameters** that are stored are passed by value and moved.
  - **Return values** instead of output parameters. Return a struct when several values are needed.
- **Strings:** `std::string` owns. `std::string_view` is for read-only parameters. Use `const std::string&` instead when the callee needs a null-terminated string, as C APIs do. `std::filesystem::path` is for paths, and `const char*` only for literals and C interop.
- **Class design:**
  - `explicit` on single-argument constructors and on conversion operators, unless an implicit conversion is the intent, with a comment. This is stricter than Hazel.
  - Polymorphic base classes have a `virtual` destructor (`virtual ~Layer() = default;`).
  - Overriding functions are marked `override` or `final`, without repeating `virtual`. The compiler checks `override`. Hazel repeats `virtual`; this repository does not.
- **Rule of zero:** prefer it. A type that owns a resource either deletes copy and move explicitly or implements them correctly.
- **Member initialization:** every scalar and pointer member has a default member initializer (`bool m_IsRunning = false;`). No member is ever left uninitialized.
- **`[[nodiscard]]`** goes on functions whose result must not be ignored: factories, handles, and status or result values.
- **Enums:**
  - Use `enum class` only.
  - Give an explicit underlying type when the value is stored, serialized or packed (`enum class AudioBus : uint8_t`).
  - When a "no value" state is needed, it is the first enumerator, `None = 0`.
  - How bit-flag enums and their operators work is **[Architecture]**.

## 7. Ownership and lifetime

- **Ownership aliases:** `Scope<T>` is unique ownership (`std::unique_ptr`) and `Ref<T>` is shared ownership. Create them with `CreateScope<T>(...)` and `CreateRef<T>(...)`. They are defined in `Engine/Core/Base.h`. Whether `Ref` is `std::shared_ptr` or intrusively reference-counted is **[Architecture]**.
- **Default to values and `Scope`.** Use `Ref` only when ownership is genuinely shared, as with assets used by several owners.
- **Raw pointers and references never own.**
  - `T*` is a non-owning reference that may be null.
  - `T&` is a non-owning reference that is never null.
  - Code never `delete`s through a raw pointer.
- **No naked `new` / `delete`** outside a dedicated allocator. This is stricter than Hazel.
- **Third-party objects:**
  - C handles (GLFW windows, miniaudio objects) are wrapped in RAII types or in a `Scope` with a custom deleter.
  - NVRHI resources use NVRHI's handle types (`nvrhi::TextureHandle`, `nvrhi::BufferHandle`).
  - Jolt objects use `JPH::Ref` where Jolt's API expects it.
  - Do not wrap any of these in `Ref`.
- **Views:** `std::string_view` and `std::span` are never stored beyond the call that received them, unless the lifetime of the viewed data is guaranteed and documented.
- **No self-move:** a value is never move-assigned to itself, also not through an rvalue-qualified builder that returns `*this`: `error = std::move(error).WithHint(hint)` moves `error` onto itself. MSVC's standard library keeps the value; libc++ and libstdc++ leave its strings and vectors empty, so the bug shows only on Linux and macOS. Build into a new variable (`Error next = std::move(error).WithContext(context); error = std::move(next);`), build the whole value in one expression, or collect the parts and attach them once (`Error::WithIssues`). Lint reports `banned-self-move`.
- **Lambda captures:**
  - A lambda that is stored or runs later lists its captures explicitly (`[this, voice]`), so that lifetimes are visible.
  - Default captures (`[&]`, `[=]`) are only for lambdas that run synchronously before the enclosing function returns.
- **ECS:** store entity handles (`Entity`, `entt::entity`), which are values. Never keep a pointer or reference to a component across frames or structural changes, because component storage moves.

```cpp
class SceneRenderer
{
private:
	Scope<RenderGraph> m_RenderGraph; // sole owner
	Ref<Texture2D> m_BRDFLookup;      // shared with the asset cache
	Scene* m_Context = nullptr;       // observer, may be null
};
```

## 8. Casts

- **No C-style or functional casts** (`(int)x`, `float(x)`). This is stricter than Hazel. Use:
  - `static_cast` for numeric conversions and conversions between related types.
  - `std::to_underlying(e)` for enum-to-integer conversions.
  - `std::bit_cast` to reinterpret an object's bytes as another type.
  - `reinterpret_cast` only for byte-level interop: serialization buffers and native handles such as `ImTextureID`.
  - `const_cast` only to call a const-incorrect C API, with a comment.
- **`dynamic_cast` and RTTI:** whether they are allowed is **[Architecture]**.
- **Narrowing conversions** are always written as an explicit `static_cast`.

```cpp
const uint32_t width = static_cast<uint32_t>(m_ViewportSize.x);
const auto layer = std::to_underlying(CollisionLayer::Static);
const ImTextureID textureID = reinterpret_cast<ImTextureID>(descriptorSet);
```

## 9. `auto`

- **Use `auto` when the type is obvious or noise:**
  - The initializer names the type, as with `CreateRef<T>()`, `GetComponent<T>()` or a cast.
  - The type is unutterable or verbose, as with lambdas, iterators and views.
  - Structured bindings.
- **Spell out the type when it carries information:** arithmetic types, and results of calls whose return type is not obvious from the name.
- **Qualify `auto`:** use `auto&` or `const auto&` to avoid accidental copies, and `auto*` for pointers.
- **Return types:** no deduced return type on non-template functions declared in headers. `decltype(auto)` appears only in generic forwarding code.

```cpp
auto& transform = entity.GetComponent<TransformComponent>();
const auto view = m_Registry.view<TransformComponent, RigidBodyComponent>();
for (const auto& [id, voice] : m_Voices)
	UpdateVoice(id, voice);

const float aspectRatio = GetAspectRatio(); // not: auto aspectRatio = ...
```

## 10. Comments

- **Explain why, not what.** Names should make most comments unnecessary.
- **Form:** use `//` comments in sentence case with one space after `//`. `/* */` is only for naming an unused parameter inline (`void OnEvent(Event& /*event*/)`).
- **No commented-out code and no `#if 0` blocks.** Version control keeps history. This is stricter than Hazel.
- **Public API comments:** a public declaration gets a short comment only when its name and signature do not convey the contract. Typical contracts are units, ownership transfer, threading requirements and failure behavior. Do not write boilerplate Doxygen blocks that restate the signature.
- **Prefixes:**
  - `// NOTE:` marks a non-obvious constraint.
  - `// TODO:` describes a planned improvement. It must never mark a known bug, missing error handling or an unfinished feature in committed code.
- **Short label comments** (`// Physics`, `// Render`) may name the steps of a long function.
- **No file banners** or per-file license headers.

```cpp
// Jolt requires bodies to be removed before the shape they reference is destroyed.
m_BodyInterface->RemoveBody(bodyID);
m_BodyInterface->DestroyBody(bodyID);

// Returns the volume in linear units [0, 1]; thread-safe.
float GetBusVolume(AudioBus bus) const;
```

## 11. Logging and assertions

The macros are defined in `Engine/Core/Log.h` and `Engine/Core/Assert.h`. The exact set, the sinks and per-subsystem loggers are **[Architecture]**. The naming pattern is fixed:

| Use | Engine code (`Engine/`) | Client code (Editor, Runtime, game-side C++) |
|-----|------------------------|----------------------------------------------|
| Logging | `ENGINE_CORE_TRACE`, `ENGINE_CORE_INFO`, `ENGINE_CORE_WARN`, `ENGINE_CORE_ERROR`, `ENGINE_CORE_CRITICAL` | `ENGINE_TRACE`, `ENGINE_INFO`, `ENGINE_WARN`, `ENGINE_ERROR`, `ENGINE_CRITICAL` |
| Assertions | `ENGINE_CORE_ASSERT(condition, ...)` | `ENGINE_ASSERT(condition, ...)` |

**Logging**

- **Format:** messages use `{}` placeholders. Write them in sentence case without a trailing period. Quote names and paths with `'{}'`, and include the context needed to act on the message.
- **Levels:**
  - **Trace:** high-volume diagnostics.
  - **Info:** lifecycle milestones, such as subsystem initialization, a project or scene loaded, or an export finished.
  - **Warn:** an unexpected but recoverable condition.
  - **Error:** an operation failed and the engine continues.
  - **Critical:** the engine cannot continue.
- **Hot paths:** no Info or Warn messages emitted every frame, and no logging in tight loops.
- **No other output channels:** no `printf`, `std::cout` or `std::cerr` for diagnostics.

```cpp
ENGINE_CORE_ERROR("Failed to load texture '{}': {}", path.string(), reason);
ENGINE_CORE_INFO("Loaded scene '{}' ({} entities) in {:.2f} ms", scene->GetName(), entityCount, elapsedMs);
```

**Assertions**

- **What they are for:** invariants and programmer errors, such as violated preconditions of an internal API.
- **When they run:** asserts are active in Debug and Release, and **compiled out in Dist**. So:
  - The condition must have no side effects.
  - An assert is never the only guard against invalid external input: files, scene data, scripts, automation commands or user actions. Those are runtime errors, handled according to [12](#12-error-handling).
- **Not `assert()`:** never use `assert()` from `<cassert>`.
- **Impossible cases** after an exhaustive `switch` over an `enum class`: assert, then return a safe fallback.

```cpp
std::string_view AudioMixer::BusToString(AudioBus bus)
{
	switch (bus)
	{
		case AudioBus::Master:  return "Master";
		case AudioBus::Music:   return "Music";
		case AudioBus::Effects: return "Effects";
		case AudioBus::Voice:   return "Voice";
	}

	ENGINE_CORE_ASSERT(false, "Unknown AudioBus {}", std::to_underlying(bus));
	return "Unknown";
}
```

## 12. Error handling

**[Architecture]** The architecture document defines the error-handling policy. It covers exceptions versus `std::expected` / result types, how failures surface to the editor, the automation protocol and scripts, and how fatal errors are reported. Until it does:

- Do not introduce ad-hoc error mechanisms.
- Never ignore a failure reported by a third-party API: Vulkan/NVRHI results, file I/O, miniaudio and Jolt results, or JSON parsing. Check it, then log it or propagate it.

## 13. C++23 usage

The language standard is C++23 for Engine, Editor, Runtime and Tests.

**Portability rule:** a language or library feature may be used only if all supported toolchains provide it:

- MSVC 14.51 (Visual Studio 2026) and its STL.
- GCC 14 with libstdc++ 14.
- Clang 19 or later (Clang 18 cannot use libstdc++'s `std::expected`).
- Apple Clang with the libc++ of the minimum supported Xcode **[Architecture]**.

When in doubt, check the compiler-support tables on cppreference. CI on all three platforms has the final say.

**Encouraged**

- **Language features:**
  - `enum class`, `constexpr`, `consteval`, `if constexpr`.
  - Concepts and `requires` clauses to constrain templates instead of SFINAE.
  - Structured bindings, range-based `for`, designated initializers.
  - `using enum` in a local scope.
  - `[[nodiscard]]`, `[[maybe_unused]]`, `[[fallthrough]]`, `[[likely]]` / `[[unlikely]]` (only where profiling justifies them).
- **Library facilities:**
  - `std::span`, `std::string_view`, `std::optional`, `std::array`.
  - `std::variant` with `std::visit`, used sparingly.
  - `std::ranges` algorithms and simple view pipelines, plus `std::ranges::to`.
  - `std::to_underlying`, `std::bit_cast`, `std::byteswap`, `std::erase_if`.
  - `contains()` on associative containers, and `std::string::contains`, `starts_with`, `ends_with`.
  - `std::source_location` (used by the logging and assert plumbing).
  - `std::expected`, subject to the error policy in [12](#12-error-handling).

**Allowed with care**

- **Explicit object parameters ("deducing `this`"):** only to remove `const` / non-`const` duplication or to replace CRTP.
- **`std::format`:** mainly through the logging macros. Formatting library details are **[Architecture]**.
- **Threads and atomics:** follow the threading model in the architecture document **[Architecture]**.

**Forbidden**

- **Unavailable or unreliable on at least one supported toolchain:**
  - C++20 modules and `import std;` (toolchain and premake support is uneven).
  - `std::stacktrace` (needs an extra library on libstdc++; missing in libc++).
  - `std::generator` and coroutines in general, unless the architecture document approves them.
  - `std::flat_map` / `std::flat_set` (not in libstdc++ 14).
- **Bypasses the logging system:** `std::print` / `std::println`, `printf`-style logging, `std::cout`.
- **Discouraged constructs:**
  - C-style casts, naked `new` / `delete`, owning raw pointers.
  - `using namespace std;`.
  - `#define` for constants or for functions that `constexpr` or templates can express.
  - `goto`.
  - `std::endl` (use `'\n'`) and `std::bind` (use a lambda).
  - `rand()` / `srand()`.
  - `volatile` used for synchronization.
  - C variadic functions in our own APIs.
  - Variable-length arrays and other compiler extensions outside dedicated platform macros.
- **Pending the architecture document:** exceptions and RTTI remain **[Architecture]**.

## 14. Unit tests (doctest)

The Tests project is a doctest executable. The quality bar is that every public function and every behavior has tests, and every bug fix adds a regression test that fails before the fix.

- **Location:** a test file mirrors the path of the unit under test, relative to the include root. `Engine/Source/Engine/Audio/AudioMixer.h` is tested in `Tests/Source/Engine/Audio/AudioMixerTests.cpp`. There is one test file per unit.
- **Suites:** each file wraps its tests in `TEST_SUITE("<Module>")`, where the module is the directory name (`"Audio"`, `"Scene"`, `"Renderer"`). One module then runs with `--test-suite=Audio`.
- **Test case names:** `TEST_CASE("<Unit>: <expected behavior>")`, where the behavior is a present-tense statement of what must hold, unique within the suite. Example: `"SceneSerializer: round-trips every component"`.
- **Subcases:** `SUBCASE("<variation>")` covers variations that share setup. Its description is lowercase.
- **Namespaces and helpers:** tests are written inside `namespace Engine { }`. Test-local helper functions are `static`, and test-local types go in an unnamed namespace, so nothing collides with engine symbols or other test files.
- **Assertion macros:**
  - `CHECK` is the default.
  - `REQUIRE` is for when continuing makes no sense, for example before dereferencing a result.
  - `CHECK_FALSE` for negations.
  - Floating-point values are compared with `doctest::Approx`. Helpers for `glm` types are **[Architecture]**.
- **Determinism:**
  - No sleeps or wall-clock timing. A bounded wait is not timing: a deadline that only bounds the failure of a wait for another thread or process (`Test::WaitUntil`), or a timeout whose peer never answers, never asserted on (ADR 0008 decision 15); a spin count never bounds a wait.
  - No dependence on test order.
  - No network.
  - Random numbers use fixed seeds.
  - Files go only in a per-test temporary directory that the test removes.
  - Global or engine state is set up and torn down by the test or its fixture.
- **Test through the public API.** No `#define private public` and no test-only friend declarations.
- **Hardware-dependent tests:** tests that need a GPU, a window or an audio device, and how they run on headless CI, are **[Architecture]**. The same applies to the FeatureTest project that is run as part of testing.

```cpp
#include "Engine/Audio/AudioMixer.h"

#include <doctest/doctest.h>

namespace Engine {

	static AudioMixerSpecification MakeTestSpecification()
	{
		return AudioMixerSpecification{
			.SampleRate = 44100,
			.ChannelCount = 2,
		};
	}

	TEST_SUITE("Audio")
	{
		TEST_CASE("AudioMixer: bus volume is clamped to the unit range")
		{
			AudioMixer mixer(MakeTestSpecification());

			SUBCASE("values above one are clamped")
			{
				mixer.SetBusVolume(AudioBus::Music, 3.0f);
				CHECK(mixer.GetBusVolume(AudioBus::Music) == doctest::Approx(1.0f));
			}

			SUBCASE("negative values are clamped")
			{
				mixer.SetBusVolume(AudioBus::Effects, -1.0f);
				CHECK(mixer.GetBusVolume(AudioBus::Effects) == doctest::Approx(0.0f));
			}
		}

		TEST_CASE("AudioMixer: playing a null clip returns an invalid voice")
		{
			AudioMixer mixer(MakeTestSpecification());

			const AudioMixer::VoiceHandle voice = mixer.Play(nullptr, AudioBus::Effects);

			CHECK(voice == AudioMixer::InvalidVoice);
			CHECK_FALSE(mixer.IsPlaying(voice));
		}
	}

}
```

## 15. Other languages

These rules cover the basics. Tooling for these languages (formatters, linters) is **[Architecture]**.

- **Python** (`Scripts/`, `Tools/`, Python 3.10 or later):
  - Follow PEP 8 with a 120-column limit (the width the C++ comments and premake scripts are wrapped at; `Docs/Decisions/0002-m0-deviations.md`): 4-space indentation, `snake_case` functions and variables, `PascalCase` classes, `UPPER_SNAKE_CASE` constants.
  - Entry-point scripts use PascalCase file names (`Setup.py`, `Build.py`, `Test.py`, `Format.py`) and end with `if __name__ == "__main__": sys.exit(main())`.
  - Use type hints, `pathlib`, and `subprocess` with argument lists (never `shell=True`).
  - Exit with a non-zero code on failure.
  - Use only the standard library unless `Setup.py` installs the dependency.
- **Lua (premake scripts):**
  - Tabs, double-quoted strings, one setting per line.
  - Every `filter` block is closed with `filter {}`.
  - Configuration names are `Debug`, `Release` and `Dist`.
- **Luau (game scripts):** tabs for indentation. The naming conventions of the scripting API are **[Architecture]**.
- **Slang shaders:**
  - Same layout rules as C++: tabs, Allman braces, PascalCase types and functions, camelCase locals.
  - Shaders are not run through clang-format.
  - Resource and binding naming is **[Architecture]**.
- **JSON** (scene and project files, automation protocol): tabs, UTF-8, LF, no comments, no trailing commas. Key naming and schema are **[Architecture]**.

## 16. Topics refined by the architecture document

This guide deliberately leaves these open:

- **Errors and language features:**
  - Error-handling policy: exceptions, `std::expected`, result types, and failure reporting to the editor, automation and scripts.
  - Whether exceptions and RTTI are enabled.
  - The warning level and warnings-as-errors policy per toolchain.
- **Core infrastructure:**
  - The `Ref` implementation (`std::shared_ptr` or intrusive) and any `WeakRef`.
  - The exact logging and assert macro set, plus any verify macro that stays active in Dist.
  - Bit-flag enum helpers.
  - The threading model.
  - Memory allocation strategy.
- **Code organization:**
  - Namespaces for Editor, Runtime, Tools and Tests, and any engine sub-namespaces beyond `Utils` / `Detail`.
  - The platform abstraction directory layout.
  - PCH usage outside the Engine project.
- **Toolchains and tests:**
  - The minimum supported Xcode / libc++ version.
  - The headless and GPU test strategy.
  - `glm` test helpers.
  - How the FeatureTest project runs.
- **Data formats and other languages:**
  - Serialization formats and JSON key naming.
  - The Luau scripting API naming.
  - Slang binding conventions.
  - Python and Lua tooling.

## 17. Why `.clang-format` is configured this way

Each choice was checked by formatting Hazel source files with clang-format 22.1.3 and comparing the result with Hazel's original layout.

| Option | Choice | Reason |
|--------|--------|--------|
| `ColumnLimit` | `0` | Hazel has no line limit, and manual wrapping is part of its look. Any fixed limit re-wraps long signatures and joins deliberate manual breaks. |
| `BraceWrapping.BeforeLambdaBody` + `AllowShortLambdasOnASingleLine: Empty` | Allman lambdas | This is Hazel's dominant lambda style. With `ColumnLimit: 0`, clang-format always breaks before a non-empty lambda body, so one-line lambdas cannot be kept, and the config makes the multi-line form consistent. |
| `LambdaBodyIndentation` | `OuterScope` | Lambda bodies are indented from the statement, not from the lambda's position in the call. This matches Hazel. |
| `AllowShortEnumsOnASingleLine` | `false` | Keeping short enums on one line would require putting the brace on the same line for every enum, which contradicts Hazel. clang-format cannot pack several enumerators per line. |
| `PackConstructorInitializers` | `NextLineOnly` | Hazel's dominant form: the initializer list always goes on the line after the signature. |
| `SortIncludes` | disabled | Include order is sometimes significant. The order is defined in [4.4](#44-include-rules). |
| `SkipMacroDefinitionBody` | `true` | Formatting macro bodies as code splits one-line macros, especially ones that contain lambdas, into awkward backslash blocks. |
| `AlignArrayOfStructures` | `None` | It reproduces Hazel's tables, but it mis-indents lists without a trailing comma. |
| `UseTab` | `AlignWithSpaces` | Tabs for indentation, spaces for alignment, so code looks right at any tab width. |
