# Locale and Internationalization

## Overview

Locale support allows the OS to handle different languages, character encodings, number formats, date formats, and sorting rules.

## Locale Category Constants

| Category | Constant | Controls |
|---|---|---|
| Character classification | `LC_CTYPE` | `isalpha`, `tolower`, multibyte chars |
| Collation | `LC_COLLATE` | `strcoll`, `strxfrm` (locale-aware string comparison) |
| Messages | `LC_MESSAGES` | Language for system messages |
| Numeric | `LC_NUMERIC` | Decimal point, thousands separator |
| Monetary | `LC_MONETARY` | Currency symbol, format |
| Date/time | `LC_TIME` | `strftime` format, day/month names |

## `setlocale`

```c
char *setlocale(int category, const char *locale);
```

Example:
```c
setlocale(LC_ALL, "");           // use environment variables (LANG, LC_*)
setlocale(LC_CTYPE, "en_US.UTF-8");
setlocale(LC_TIME, "de_DE.UTF-8");
```

## UTF-8 and Multibyte

The OS uses **UTF-8** as the native encoding. UTF-8 encodes all Unicode code points:
- U+0000 to U+007F: single byte (ASCII compatible).
- U+0080 to U+07FF: 2 bytes.
- U+0800 to U+FFFF: 3 bytes.
- U+10000 to U+10FFFF: 4 bytes.

`mbstowcs(dest, src, n)`: convert multibyte string (UTF-8) to wide characters (`wchar_t`, 32-bit Unicode code points).
`wcstombs(dest, src, n)`: convert wide characters to multibyte (UTF-8).

## `wchar_t` vs `char32_t`

The `wchar_t` type is 32 bits (one Unicode code point per element). `char32_t` (from `<uchar.h>`) is the same size and is the preferred type for modern code. Locale-aware functions operate on `wchar_t` strings:
- `wcslen`, `wcscpy`, `wcscat`, `wcscmp`, `wcsncmp`
- `iswupper`, `iswlower`, `towupper`, `towlower`

## Locale Files

Locale data is stored as binary files in `/usr/share/locale/`:
```
/usr/share/locale/
  en_US.UTF-8/
    LC_CTYPE
    LC_COLLATE
    LC_TIME
    ...
  de_DE.UTF-8/
    LC_TIME
    ...
```

These binary files are loaded by `setlocale()` and mapped into the process address space.

## Environment Variables

| Variable | Effect |
|---|---|
| `LANG` | Default locale for all categories |
| `LC_ALL` | Override for all categories (highest priority) |
| `LC_CTYPE` | Override for character classification |
| `LC_MESSAGES` | Language for error messages |
| `LC_TIME` | Date/time format |
| `LC_NUMERIC` | Number format |

## Related Documents

- [libc.md](libc.md)
- [overview.md](overview.md)
