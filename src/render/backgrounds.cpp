// Storage for the single shared Backgrounds instance. State is non-trivial
// (~520 B) so we don't want to live in BSS-init via a header-only define;
// one explicit definition keeps it in one place.
//
// (added in phase 2.5)

#include "backgrounds.h"

Backgrounds g_backgrounds;
