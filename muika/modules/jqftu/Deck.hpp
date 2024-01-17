// SPDX-License-Identifier: GPL-2.0-only

#ifndef MUIKA__MODULES__JQFTU__CARD_HPP
#define MUIKA__MODULES__JQFTU__CARD_HPP

#include <string>

namespace muika {
namespace modules {
namespace jqftu {

class Deck {
protected:
	std::string name_;
	inline void setName(const std::string &name) { name_ = name; }

public:
	Deck(void);
	virtual ~Deck(void) = default;
	virtual void shuffle(void) = 0;
	virtual Card *draw(void) = 0;

	const std::string &getName(void) const { return name_; }
};

} /* namespace muika::modules::jqftu */
} /* namespace muika::modules */
} /* namespace muika */

#endif /* MUIKA__MODULES__JQFTU__CARD_HPP */
