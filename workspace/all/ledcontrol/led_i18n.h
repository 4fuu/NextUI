#ifndef __LED_I18N_H__
#define __LED_I18N_H__

#ifdef __cplusplus
extern "C"
{
#endif

// Dedicated translation table for the LedControl pak.
//
// The pak does not link the shared common/i18n.c table (that one belongs to the
// launcher and settings app); this module mirrors its design so the two can
// never drift into a shared-file conflict. English is the lookup key, the
// active language comes from the same persisted setting the rest of the system
// uses (CFG_getLanguage, i.e. lang= in .userdata/shared/minuisettings.txt), and
// an unknown or missing language renders English exactly as before.
//
// When the active language is English, or the table has no entry for the key,
// the key itself is returned unchanged, so callers always pass the exact
// English text they want to display.
//
// Keys are static string literals; the missing-translation log keeps pointers
// to them and does not copy the text.
const char* LEDI18N(const char* key);

#define TR(key) LEDI18N(key)

#ifdef __cplusplus
}
#endif

#endif
