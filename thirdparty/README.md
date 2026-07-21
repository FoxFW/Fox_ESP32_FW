# Vendored word list - LDNOOBW

`ldnoobw_en.txt` is the English word list from the Shutterstock
**"List of Dirty, Naughty, Obscene, and Otherwise Bad Words"** project:

https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words

Retrieved from the `en` file on that repo's `master` branch, unmodified
except for dropping the single trailing emoji entry (a raw Unicode
codepoint doesn't fit this project's plain-ASCII PROGMEM string table
without extra encoding work, and it's not something anyone is typing
from a Flipper's on-screen keyboard anyway).

**License:** © 2012-2020 Shutterstock, Inc. Licensed under a
[Creative Commons Attribution 4.0 International License](http://creativecommons.org/licenses/by/4.0/)
(CC BY 4.0). Redistribution requires attribution, which this notice is
that attribution.

**Why this list, not a bigger one:** A follow-up project
(`LDNOOBWV2/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words_V2`)
extends this into ~13,000 English entries including heavy slang and
spelling-variation coverage. That's better recall, but on a device
doing plain substring/whole-word matching (see `profanity_filter.cpp`,
no ML/context awareness), a much bigger list also means a much bigger
false-positive surface - more everyday words accidentally containing a
listed substring. This smaller, more curated ~400-entry list was kept
deliberately to favor precision over exhaustive coverage for FoxChat's
use case (short, casual chat messages). Swapping in the bigger list
later is a drop-in change if coverage ever matters more than false
positives for a given deployment - see `generate_profanity_data.py` in
this folder for how the list gets turned into `profanity_words_data.h`.

**Known limitation:** this is a static word/phrase list matched via
plain substring search (word-boundary-checked for single tokens, not
for multi-word phrases) - it doesn't catch leetspeak substitutions,
spacing tricks, or anything not literally in the list, and it can still
false-positive on a word that legitimately contains a shorter blocked
entry as a substring (the "Scunthorpe problem"). See
`profanity_filter.cpp`'s header comment for the exact matching rules.
