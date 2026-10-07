#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	// A value with an rvalue-qualified builder, as Error has.
	class Notes
	{
	public:
		Notes&& WithNote(std::string note) &&
		{
			m_Notes.push_back(std::move(note));
			return std::move(*this);
		}

		[[nodiscard]] size_t GetCount() const { return m_Notes.size(); }
	private:
		std::vector<std::string> m_Notes;
	};

	[[nodiscard]] Notes CollectNotes(const std::vector<std::string>& lines);

}
