#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Base.h"

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace Engine {

	template<typename Signature>
	class UniqueFunction;

	// A move-only type-erased callable: the task type of JobSystem and MainThreadQueue, whose jobs and continuations
	// capture move-only values (Scope<T>, Buffer, Result<T>). std::function requires copyable callables, and
	// std::move_only_function is not available in every supported standard library (the libc++ of the minimum Xcode),
	// so the engine carries this minimal equivalent. Calling an empty UniqueFunction is a programmer error (asserted).
	// Not thread-safe (one owner).
	template<typename Return, typename... Args>
	class UniqueFunction<Return(Args...)>
	{
	public:
		UniqueFunction() = default;

		// Implicit by design, like std::function, so that lambdas convert where a task is expected.
		template<typename Function>
			requires(!std::same_as<std::remove_cvref_t<Function>, UniqueFunction>
				&& std::is_invocable_r_v<Return, std::decay_t<Function>&, Args...>)
		UniqueFunction(Function&& function)
			: m_Callable(CreateScope<Model<std::decay_t<Function>>>(std::forward<Function>(function)))
		{
		}

		UniqueFunction(UniqueFunction&&) noexcept = default;
		UniqueFunction& operator=(UniqueFunction&&) noexcept = default;
		UniqueFunction(const UniqueFunction&) = delete;
		UniqueFunction& operator=(const UniqueFunction&) = delete;

		explicit operator bool() const { return m_Callable != nullptr; }

		Return operator()(Args... args)
		{
			ENGINE_CORE_ASSERT(m_Callable != nullptr, "Calling an empty UniqueFunction");
			return m_Callable->Invoke(std::forward<Args>(args)...);
		}
	private:
		struct Concept
		{
			virtual ~Concept() = default;
			virtual Return Invoke(Args... args) = 0;
		};

		template<typename Function>
		struct Model final : Concept
		{
			template<typename Source>
			explicit Model(Source&& source)
				: Callable(std::forward<Source>(source))
			{
			}

			Return Invoke(Args... args) override
			{
				if constexpr (std::is_void_v<Return>)
					std::invoke(Callable, std::forward<Args>(args)...);
				else
					return std::invoke(Callable, std::forward<Args>(args)...);
			}

			Function Callable;
		};
	private:
		Scope<Concept> m_Callable;
	};

}
