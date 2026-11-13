#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <xlocale.h>

static void require(int condition, const char *what) {
    if (!condition) { fprintf(stderr, "locale time failed: %s\n", what); exit(1); }
}
static void check_time(locale_t locale, locale_t current) {
    struct tm time = {.tm_sec=8, .tm_min=52, .tm_hour=20, .tm_mday=11,
                      .tm_mon=10, .tm_year=126, .tm_wday=3, .tm_yday=314};
    char text[128];
    wchar_t wide[128];
    errno = EDOM;
    require(strftime_l(text, sizeof text, "%Y-%m-%d (%A) %H:%M:%S", &time, locale) == 31,
            "format date with Darwin locale semantics");
    require(strcmp(text, "2026-11-11 (Wednesday) 20:52:08") == 0, "numeric date and locale weekday");
    require(errno == EDOM && uselocale(NULL) == current, "formatting preserves caller locale and errno");
    require(wcsftime_l(wide, 128, L"%Y-%m-%d %A", &time, locale) == 20, "wide date format");
    require(wcscmp(wide, L"2026-11-11 Wednesday") == 0, "wide date content");
    struct tm parsed = {0};
    const char *source = "2026-11-11T20:52:08 tail";
    char *end = strptime_l(source, "%Y-%m-%dT%H:%M:%S", &parsed, locale);
    require(end && strcmp(end, " tail") == 0, "date parser returns unconsumed suffix");
    require(parsed.tm_year == 126 && parsed.tm_mon == 10 && parsed.tm_mday == 11 &&
            parsed.tm_hour == 20 && parsed.tm_min == 52 && parsed.tm_sec == 8, "parsed calendar fields");
    require(uselocale(NULL) == current, "parsing restores caller locale");
    require(strftime_l(text, 1, "%Y", &time, locale) == 0, "formatting reports insufficient space");
}
int main(void) {
    locale_t locale = newlocale(LC_ALL_MASK, "C", NULL);
    require(locale != NULL, "create explicit locale");
    locale_t old = uselocale(locale);
    check_time(NULL, locale);
    check_time(locale, locale);
    check_time(LC_GLOBAL_LOCALE, locale);
    require(uselocale(old) == locale, "restore original thread locale");
    check_time(NULL, old);
    freelocale(locale);
    puts("SDK date formatting/parsing, null and global locales, wide output and caller state passed");
    return 0;
}
