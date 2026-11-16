#ifndef __I18N_H__
#define __I18N_H__

#ifdef __cplusplus
extern "C"
{
#endif

// Translate an English source string to the active UI language.
//
// The English string is the lookup key. When the active language is English,
// or when the Chinese table has no entry for the key, the key itself is
// returned unchanged. Callers should therefore always pass the exact English
// text they want to display.
//
// Keys are static string literals; the missing-translation log keeps pointers
// to them and does not copy the text.
const char* I18N_translate(const char* key);

#define TR(key) I18N_translate(key)

#ifdef __cplusplus
}
#endif

#endif
