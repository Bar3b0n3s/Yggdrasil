#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace Engine {

	template<typename T>
	class HandlePool;

	// A generational reference into a HandlePool<T> (Architecture §4.7): a 32-bit slot index plus a 32-bit generation.
	// Used for audio voices and debug-draw buffers. Generation 0 is never issued, so a default-constructed handle is the
	// null handle. A handle whose object was destroyed is stale: the pool reports it as not alive, never as another
	// object. Plain value type.
	template<typename T>
	class Handle
	{
	public:
		constexpr Handle() = default;

		[[nodiscard]] constexpr bool IsNull() const { return m_Generation == 0; }
		[[nodiscard]] constexpr uint32_t GetIndex() const { return m_Index; }
		[[nodiscard]] constexpr uint32_t GetGeneration() const { return m_Generation; }

		// generation << 32 | index: a stable number for logs, scripts and automation.
		[[nodiscard]] constexpr uint64_t GetValue() const { return (static_cast<uint64_t>(m_Generation) << 32) | m_Index; }

		// The inverse of GetValue. The result may be stale or foreign; the pool checks it.
		[[nodiscard]] static constexpr Handle FromValue(uint64_t value)
		{
			return Handle(static_cast<uint32_t>(value & 0xffffffffu), static_cast<uint32_t>(value >> 32));
		}

		constexpr auto operator<=>(const Handle&) const = default;
	private:
		constexpr Handle(uint32_t index, uint32_t generation)
			: m_Index(index), m_Generation(generation)
		{
		}
	private:
		uint32_t m_Index = 0;
		uint32_t m_Generation = 0;
	private:
		friend class HandlePool<T>;
	};

	// Owns objects of type T addressed by Handle<T> (Architecture §4.7).
	//   - Create reuses the most recently freed slot (deterministic), with a generation one higher than the slot's
	//     previous occupant, so every handle to the previous occupant becomes stale. A slot whose generation would wrap
	//     around is retired and never reused, so a stale handle can never alias a later object.
	//   - TryGet and IsAlive report stale or null handles; Get treats them as a programmer error checked in every
	//     configuration (ENGINE_CORE_VERIFY), so a stale access is never undefined behaviour; Destroy returns NotFound.
	//   - ForEach visits live objects in slot-index order, which is deterministic for a given call history.
	// Pointers and references returned by TryGet and Get are invalidated by the next Create (the slot array may grow).
	// Not thread-safe (one owner).
	template<typename T>
	class HandlePool
	{
	public:
		static constexpr uint32_t UnlimitedCapacity = std::numeric_limits<uint32_t>::max();

		// At most `capacity` live objects (for example 64 voices, §10.1).
		explicit HandlePool(uint32_t capacity = UnlimitedCapacity)
			: m_Capacity(capacity)
		{
		}

		// Constructs a T from `args`. Errors: InvalidState when `capacity` objects are alive or no slot index is left.
		template<typename... Args>
		[[nodiscard]] Result<Handle<T>> Create(Args&&... args)
		{
			if (m_Size >= m_Capacity)
				return MakeError(ErrorCode::InvalidState, "handle pool is full ({} live objects)", m_Size);

			uint32_t index = 0;
			if (!m_FreeList.empty())
			{
				index = m_FreeList.back();
				m_FreeList.pop_back();
			}
			else
			{
				if (m_Slots.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
					return MakeError(ErrorCode::InvalidState, "handle pool has no slot index left");
				index = static_cast<uint32_t>(m_Slots.size());
				m_Slots.emplace_back();
			}

			Slot& slot = m_Slots[index];
			slot.Value.emplace(std::forward<Args>(args)...);
			++m_Size;
			return Handle<T>(index, slot.Generation);
		}

		// Destroys the object. Errors: NotFound for a null or stale handle.
		[[nodiscard]] Status Destroy(Handle<T> handle)
		{
			if (!IsAlive(handle))
				return MakeError(ErrorCode::NotFound, "handle {:#x} does not refer to a live object", handle.GetValue());

			Release(handle.GetIndex());
			return {};
		}

		[[nodiscard]] bool IsAlive(Handle<T> handle) const
		{
			if (handle.IsNull() || handle.GetIndex() >= m_Slots.size())
				return false;
			const Slot& slot = m_Slots[handle.GetIndex()];
			return slot.Value.has_value() && slot.Generation == handle.GetGeneration();
		}

		// The object, or nullptr for a null or stale handle.
		[[nodiscard]] T* TryGet(Handle<T> handle)
		{
			return IsAlive(handle) ? &*m_Slots[handle.GetIndex()].Value : nullptr;
		}

		[[nodiscard]] const T* TryGet(Handle<T> handle) const
		{
			return IsAlive(handle) ? &*m_Slots[handle.GetIndex()].Value : nullptr;
		}

		// The object; the handle must be alive (checked in every configuration).
		[[nodiscard]] T& Get(Handle<T> handle)
		{
			ENGINE_CORE_VERIFY(IsAlive(handle), "Stale or null handle {:#x}", handle.GetValue());
			return *m_Slots[handle.GetIndex()].Value;
		}

		[[nodiscard]] const T& Get(Handle<T> handle) const
		{
			ENGINE_CORE_VERIFY(IsAlive(handle), "Stale or null handle {:#x}", handle.GetValue());
			return *m_Slots[handle.GetIndex()].Value;
		}

		// Calls `function(Handle<T>, T&)` for every live object, in slot-index order. The function must not create or
		// destroy objects of this pool.
		template<typename Function>
		void ForEach(Function&& function)
		{
			for (size_t index = 0; index < m_Slots.size(); ++index)
			{
				Slot& slot = m_Slots[index];
				if (slot.Value.has_value())
					function(Handle<T>(static_cast<uint32_t>(index), slot.Generation), *slot.Value);
			}
		}

		// Destroys every object, in slot-index order. Every outstanding handle becomes stale.
		void Clear()
		{
			for (size_t index = 0; index < m_Slots.size(); ++index)
			{
				if (m_Slots[index].Value.has_value())
					Release(static_cast<uint32_t>(index));
			}
		}

		[[nodiscard]] uint32_t GetSize() const { return m_Size; }
		[[nodiscard]] uint32_t GetCapacity() const { return m_Capacity; }
	private:
		// Destroys the live object in slot `index` and advances the slot's generation, or retires the slot when the
		// generation cannot advance without wrapping to 0.
		void Release(uint32_t index)
		{
			Slot& slot = m_Slots[index];
			slot.Value.reset();
			--m_Size;
			if (slot.Generation == std::numeric_limits<uint32_t>::max())
				return;
			++slot.Generation;
			m_FreeList.push_back(index);
		}
	private:
		struct Slot
		{
			std::optional<T> Value;
			uint32_t Generation = 1; // of the current occupant, or of the next one while free
		};
	private:
		std::vector<Slot> m_Slots;
		std::vector<uint32_t> m_FreeList; // freed slot indices, most recent last
		uint32_t m_Size = 0;
		uint32_t m_Capacity = UnlimitedCapacity;
	};

}
