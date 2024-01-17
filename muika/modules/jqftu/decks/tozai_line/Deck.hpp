// SPDX-License-Identifier: GPL-2.0-only

#ifndef MUIKA__MODULES__JQFTU__DECKS__TOZAI_LINE__DECK_HPP
#define MUIKA__MODULES__JQFTU__DECKS__TOZAI_LINE__DECK_HPP

#include "muika/modules/jqftu/Deck.hpp"
#include "muika/modules/jqftu/decks/tozai_line/Card.hpp"

#include <vector>

namespace muika {
namespace modules {
namespace jqftu {
namespace decks {
namespace tozai_line {

class Deck: public muika::modules::jqftu::Deck {
private:
	std::vector<Card> deck_;
	void loadDeck(void);
	char *getDeckJsonString(void);

public:
	Deck(void);
	virtual ~Deck(void) = default;
	virtual void shuffle(void) override;
	virtual Card *draw(void) override;
	virtual bool isFinished(void) const override;
};

} /* namespace muika::modules::jqftu::decks::tozai_line */
} /* namespace muika::modules::jqftu::decks */
} /* namespace muika::modules::jqftu */
} /* namespace muika::modules */
} /* namespace muika */

#endif /* MUIKA__MODULES__JQFTU__DECKS__TOZAI_LINE__DECK_HPP */
