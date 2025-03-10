// Various bug and feature tests. Compiled and run by make test.
#include "config.h"  // IWYU pragma: keep

// IWYU pragma: no_include <cstring>
// IWYU pragma: no_include <cstddef>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ast.h"
#include "autoload.h"
#include "builtin.h"
#include "color.h"
#include "common.h"
#include "complete.h"
#include "env.h"
#include "env_universal_common.h"
#include "event.h"
#include "expand.h"
#include "fallback.h"  // IWYU pragma: keep
#include "fd_monitor.h"
#include "function.h"
#include "future_feature_flags.h"
#include "highlight.h"
#include "history.h"
#include "input.h"
#include "input_common.h"
#include "io.h"
#include "iothread.h"
#include "kill.h"
#include "lru.h"
#include "maybe.h"
#include "operation_context.h"
#include "pager.h"
#include "parse_constants.h"
#include "parse_tree.h"
#include "parse_util.h"
#include "parser.h"
#include "path.h"
#include "proc.h"
#include "reader.h"
#include "redirection.h"
#include "screen.h"
#include "signal.h"
#include "termsize.h"
#include "timer.h"
#include "tokenizer.h"
#include "topic_monitor.h"
#include "utf8.h"
#include "util.h"
#include "wcstringutil.h"
#include "wildcard.h"
#include "wutil.h"  // IWYU pragma: keep

static const char *const *s_arguments;
static int s_test_run_count = 0;

#define system_assert(command)                                     \
    if (system(command)) {                                         \
        err(L"Non-zero result on line %d: %s", __LINE__, command); \
    }

// Indicate if we should test the given function. Either we test everything (all arguments) or we
// run only tests that have a prefix in s_arguments.
// If \p default_on is set, then allow no args to run this test by default.
static bool should_test_function(const char *func_name, bool default_on = true) {
    bool result = false;
    if (!s_arguments || !s_arguments[0]) {
        // No args, test if defaulted on.
        result = default_on;
    } else {
        for (size_t i = 0; s_arguments[i] != nullptr; i++) {
            if (!std::strcmp(func_name, s_arguments[i])) {
                result = true;
                break;
            }
        }
    }
    if (result) s_test_run_count++;
    return result;
}

/// The number of tests to run.
#define ESCAPE_TEST_COUNT 100000
/// The average length of strings to unescape.
#define ESCAPE_TEST_LENGTH 100
/// The highest character number of character to try and escape.
#define ESCAPE_TEST_CHAR 4000

/// Number of encountered errors.
static int err_count = 0;

/// Print formatted output.
static void say(const wchar_t *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    std::vfwprintf(stdout, fmt, va);
    va_end(va);
    std::fwprintf(stdout, L"\n");
}

/// Print formatted error string.
static void err(const wchar_t *blah, ...) {
    va_list va;
    va_start(va, blah);
    err_count++;

    // Show errors in red.
    std::fputws(L"\x1B[31m", stdout);
    std::fwprintf(stdout, L"Error: ");
    std::vfwprintf(stdout, blah, va);
    va_end(va);

    // Return to normal color.
    std::fputws(L"\x1B[0m", stdout);
    std::fwprintf(stdout, L"\n");
}

/// Joins a wcstring_list_t via commas.
static wcstring comma_join(const wcstring_list_t &lst) {
    wcstring result;
    for (size_t i = 0; i < lst.size(); i++) {
        if (i > 0) {
            result.push_back(L',');
        }
        result.append(lst.at(i));
    }
    return result;
}

static std::vector<std::string> pushed_dirs;

/// Helper to chdir and then update $PWD.
static bool pushd(const char *path) {
    char cwd[PATH_MAX] = {};
    if (getcwd(cwd, sizeof cwd) == nullptr) {
        err(L"getcwd() from pushd() failed: errno = %d", errno);
        return false;
    }
    pushed_dirs.emplace_back(cwd);

    // We might need to create the directory. We don't care if this fails due to the directory
    // already being present.
    mkdir(path, 0770);

    int ret = chdir(path);
    if (ret != 0) {
        err(L"chdir(\"%s\") from pushd() failed: errno = %d", path, errno);
        return false;
    }

    env_stack_t::principal().set_pwd_from_getcwd();
    return true;
}

static void popd() {
    const std::string &old_cwd = pushed_dirs.back();
    if (chdir(old_cwd.c_str()) == -1) {
        err(L"chdir(\"%s\") from popd() failed: errno = %d", old_cwd.c_str(), errno);
    }
    pushed_dirs.pop_back();
    env_stack_t::principal().set_pwd_from_getcwd();
}

// Helper to return a string whose length greatly exceeds PATH_MAX.
wcstring get_overlong_path() {
    wcstring longpath;
    longpath.reserve(PATH_MAX * 2 + 10);
    while (longpath.size() <= PATH_MAX * 2) {
        longpath += L"/overlong";
    }
    return longpath;
}

// The odd formulation of these macros is to avoid "multiple unary operator" warnings from oclint
// were we to use the more natural "if (!(e)) err(..." form. We have to do this because the rules
// for the C preprocessor make it practically impossible to embed a comment in the body of a macro.
#define do_test(e)                                             \
    do {                                                       \
        if (e) {                                               \
            ;                                                  \
        } else {                                               \
            err(L"Test failed on line %lu: %s", __LINE__, #e); \
        }                                                      \
    } while (0)

#define do_test_from(e, from)                                                   \
    do {                                                                        \
        if (e) {                                                                \
            ;                                                                   \
        } else {                                                                \
            err(L"Test failed on line %lu (from %lu): %s", __LINE__, from, #e); \
        }                                                                       \
    } while (0)

#define do_test1(e, msg)                                           \
    do {                                                           \
        if (e) {                                                   \
            ;                                                      \
        } else {                                                   \
            err(L"Test failed on line %lu: %ls", __LINE__, (msg)); \
        }                                                          \
    } while (0)

/// Test that the fish functions for converting strings to numbers work.
static void test_str_to_num() {
    say(L"Testing str_to_num");
    const wchar_t *end;
    int i;
    long l;

    i = fish_wcstoi(L"");
    do_test1(errno == EINVAL && i == 0, L"converting empty string to int did not fail");
    i = fish_wcstoi(L" \n ");
    do_test1(errno == EINVAL && i == 0, L"converting whitespace string to int did not fail");
    i = fish_wcstoi(L"123");
    do_test1(errno == 0 && i == 123, L"converting valid num to int did not succeed");
    i = fish_wcstoi(L"-123");
    do_test1(errno == 0 && i == -123, L"converting valid num to int did not succeed");
    i = fish_wcstoi(L" 345  ");
    do_test1(errno == 0 && i == 345, L"converting valid num to int did not succeed");
    i = fish_wcstoi(L" -345  ");
    do_test1(errno == 0 && i == -345, L"converting valid num to int did not succeed");
    i = fish_wcstoi(L"x345");
    do_test1(errno == EINVAL && i == 0, L"converting invalid num to int did not fail");
    i = fish_wcstoi(L" x345");
    do_test1(errno == EINVAL && i == 0, L"converting invalid num to int did not fail");
    i = fish_wcstoi(L"456 x");
    do_test1(errno == -1 && i == 456, L"converting invalid num to int did not fail");
    i = fish_wcstoi(L"99999999999999999999999");
    do_test1(errno == ERANGE && i == INT_MAX, L"converting invalid num to int did not fail");
    i = fish_wcstoi(L"-99999999999999999999999");
    do_test1(errno == ERANGE && i == INT_MIN, L"converting invalid num to int did not fail");
    i = fish_wcstoi(L"567]", &end);
    do_test1(errno == -1 && i == 567 && *end == L']',
             L"converting valid num to int did not succeed");
    // This is subtle. "567" in base 8 is "375" in base 10. The final "8" is not converted.
    i = fish_wcstoi(L"5678", &end, 8);
    do_test1(errno == -1 && i == 375 && *end == L'8',
             L"converting invalid num to int did not fail");

    l = fish_wcstol(L"");
    do_test1(errno == EINVAL && l == 0, L"converting empty string to long did not fail");
    l = fish_wcstol(L" \t ");
    do_test1(errno == EINVAL && l == 0, L"converting whitespace string to long did not fail");
    l = fish_wcstol(L"123");
    do_test1(errno == 0 && l == 123, L"converting valid num to long did not succeed");
    l = fish_wcstol(L"-123");
    do_test1(errno == 0 && l == -123, L"converting valid num to long did not succeed");
    l = fish_wcstol(L" 345  ");
    do_test1(errno == 0 && l == 345, L"converting valid num to long did not succeed");
    l = fish_wcstol(L" -345  ");
    do_test1(errno == 0 && l == -345, L"converting valid num to long did not succeed");
    l = fish_wcstol(L"x345");
    do_test1(errno == EINVAL && l == 0, L"converting invalid num to long did not fail");
    l = fish_wcstol(L" x345");
    do_test1(errno == EINVAL && l == 0, L"converting invalid num to long did not fail");
    l = fish_wcstol(L"456 x");
    do_test1(errno == -1 && l == 456, L"converting invalid num to long did not fail");
    l = fish_wcstol(L"99999999999999999999999");
    do_test1(errno == ERANGE && l == LONG_MAX, L"converting invalid num to long did not fail");
    l = fish_wcstol(L"-99999999999999999999999");
    do_test1(errno == ERANGE && l == LONG_MIN, L"converting invalid num to long did not fail");
    l = fish_wcstol(L"567]", &end);
    do_test1(errno == -1 && l == 567 && *end == L']',
             L"converting valid num to long did not succeed");
    // This is subtle. "567" in base 8 is "375" in base 10. The final "8" is not converted.
    l = fish_wcstol(L"5678", &end, 8);
    do_test1(errno == -1 && l == 375 && *end == L'8',
             L"converting invalid num to long did not fail");
}

enum class test_enum { alpha, beta, gamma, COUNT };

template <>
struct enum_info_t<test_enum> {
    static constexpr auto count = test_enum::COUNT;
};

static void test_enum_set() {
    say(L"Testing enum set");
    enum_set_t<test_enum> es;
    do_test(es.none());
    do_test(!es.any());
    do_test(es.to_raw() == 0);
    do_test(es == enum_set_t<test_enum>::from_raw(0));
    do_test(es != enum_set_t<test_enum>::from_raw(1));

    es.set(test_enum::beta);
    do_test(es.get(test_enum::beta));
    do_test(!es.get(test_enum::alpha));
    do_test(es & test_enum::beta);
    do_test(!(es & test_enum::alpha));
    do_test(es.to_raw() == 2);
    do_test(es == enum_set_t<test_enum>::from_raw(2));
    do_test(es == enum_set_t<test_enum>{test_enum::beta});
    do_test(es != enum_set_t<test_enum>::from_raw(3));
    do_test(es.any());
    do_test(!es.none());

    do_test((enum_set_t<test_enum>{test_enum::beta} | test_enum::alpha).to_raw() == 3);
    do_test((enum_set_t<test_enum>{test_enum::beta} | enum_set_t<test_enum>{test_enum::alpha})
                .to_raw() == 3);

    unsigned idx = 0;
    for (auto v : enum_iter_t<test_enum>{}) {
        do_test(static_cast<unsigned>(v) == idx);
        idx++;
    }
    do_test(static_cast<unsigned>(test_enum::COUNT) == idx);
}

static void test_enum_array() {
    say(L"Testing enum array");
    enum_array_t<std::string, test_enum> es{};
    do_test(es.size() == enum_count<test_enum>());
    es[test_enum::beta] = "abc";
    do_test(es[test_enum::beta] == "abc");
    es.at(test_enum::gamma) = "def";
    do_test(es.at(test_enum::gamma) == "def");
}

/// Test sane escapes.
static void test_unescape_sane() {
    const struct test_t {
        const wchar_t *input;
        const wchar_t *expected;
    } tests[] = {
        {L"abcd", L"abcd"},           {L"'abcd'", L"abcd"},
        {L"'abcd\\n'", L"abcd\\n"},   {L"\"abcd\\n\"", L"abcd\\n"},
        {L"\"abcd\\n\"", L"abcd\\n"}, {L"\\143", L"c"},
        {L"'\\143'", L"\\143"},       {L"\\n", L"\n"}  // \n normally becomes newline
    };
    wcstring output;
    for (const auto &test : tests) {
        bool ret = unescape_string(test.input, &output, UNESCAPE_DEFAULT);
        if (!ret) {
            err(L"Failed to unescape '%ls'\n", test.input);
        } else if (output != test.expected) {
            err(L"In unescaping '%ls', expected '%ls' but got '%ls'\n", test.input, test.expected,
                output.c_str());
        }
    }

    // Test for overflow.
    if (unescape_string(L"echo \\UFFFFFF", &output, UNESCAPE_DEFAULT)) {
        err(L"Should not have been able to unescape \\UFFFFFF\n");
    }
    if (unescape_string(L"echo \\U110000", &output, UNESCAPE_DEFAULT)) {
        err(L"Should not have been able to unescape \\U110000\n");
    }
#if WCHAR_MAX != 0xffff
    // TODO: Make this work on MS Windows.
    if (!unescape_string(L"echo \\U10FFFF", &output, UNESCAPE_DEFAULT)) {
        err(L"Should have been able to unescape \\U10FFFF\n");
    }
#endif
}

/// Test the escaping/unescaping code by escaping/unescaping random strings and verifying that the
/// original string comes back.
static void test_escape_crazy() {
    say(L"Testing escaping and unescaping");
    wcstring random_string;
    wcstring escaped_string;
    wcstring unescaped_string;
    bool unescaped_success;
    for (size_t i = 0; i < ESCAPE_TEST_COUNT; i++) {
        random_string.clear();
        while (random() % ESCAPE_TEST_LENGTH) {
            random_string.push_back((random() % ESCAPE_TEST_CHAR) + 1);
        }

        escaped_string = escape_string(random_string, ESCAPE_ALL);
        unescaped_success = unescape_string(escaped_string, &unescaped_string, UNESCAPE_DEFAULT);

        if (!unescaped_success) {
            err(L"Failed to unescape string <%ls>", escaped_string.c_str());
            break;
        } else if (unescaped_string != random_string) {
            err(L"Escaped and then unescaped string '%ls', but got back a different string '%ls'",
                random_string.c_str(), unescaped_string.c_str());
            break;
        }
    }

    // Verify that not using `ESCAPE_ALL` also escapes backslashes so we don't regress on issue
    // #3892.
    random_string = L"line 1\\n\nline 2";
    escaped_string = escape_string(random_string, ESCAPE_NO_QUOTED);
    unescaped_success = unescape_string(escaped_string, &unescaped_string, UNESCAPE_DEFAULT);
    if (!unescaped_success) {
        err(L"Failed to unescape string <%ls>", escaped_string.c_str());
    } else if (unescaped_string != random_string) {
        err(L"Escaped and then unescaped string '%ls', but got back a different string '%ls'",
            random_string.c_str(), unescaped_string.c_str());
    }
}

static void test_escape_quotes() {
    say(L"Testing escaping with quotes");
    // These are "raw string literals"
    do_test(parse_util_escape_string_with_quote(L"abc", L'\0') == L"abc");
    do_test(parse_util_escape_string_with_quote(L"abc~def", L'\0') == L"abc\\~def");
    do_test(parse_util_escape_string_with_quote(L"abc~def", L'\0', true) == L"abc~def");
    do_test(parse_util_escape_string_with_quote(L"abc\\~def", L'\0') == L"abc\\\\\\~def");
    do_test(parse_util_escape_string_with_quote(L"abc\\~def", L'\0', true) == L"abc\\\\~def");
    do_test(parse_util_escape_string_with_quote(L"~abc", L'\0') == L"\\~abc");
    do_test(parse_util_escape_string_with_quote(L"~abc", L'\0', true) == L"~abc");
    do_test(parse_util_escape_string_with_quote(L"~abc|def", L'\0') == L"\\~abc\\|def");
    do_test(parse_util_escape_string_with_quote(L"|abc~def", L'\0') == L"\\|abc\\~def");
    do_test(parse_util_escape_string_with_quote(L"|abc~def", L'\0', true) == L"\\|abc~def");
    do_test(parse_util_escape_string_with_quote(L"foo\nbar", L'\0') == L"foo\\nbar");

    // Note tildes are not expanded inside quotes, so no_tilde is ignored with a quote.
    do_test(parse_util_escape_string_with_quote(L"abc", L'\'') == L"abc");
    do_test(parse_util_escape_string_with_quote(L"abc\\def", L'\'') == L"abc\\\\def");
    do_test(parse_util_escape_string_with_quote(L"abc'def", L'\'') == L"abc\\'def");
    do_test(parse_util_escape_string_with_quote(L"~abc'def", L'\'') == L"~abc\\'def");
    do_test(parse_util_escape_string_with_quote(L"~abc'def", L'\'', true) == L"~abc\\'def");
    do_test(parse_util_escape_string_with_quote(L"foo\nba'r", L'\'') == L"foo'\\n'ba\\'r");
    do_test(parse_util_escape_string_with_quote(L"foo\\\\bar", L'\'') == L"foo\\\\\\\\bar");

    do_test(parse_util_escape_string_with_quote(L"abc", L'"') == L"abc");
    do_test(parse_util_escape_string_with_quote(L"abc\\def", L'"') == L"abc\\\\def");
    do_test(parse_util_escape_string_with_quote(L"~abc'def", L'"') == L"~abc'def");
    do_test(parse_util_escape_string_with_quote(L"~abc'def", L'"', true) == L"~abc'def");
    do_test(parse_util_escape_string_with_quote(L"foo\nba'r", L'"') == L"foo\"\\n\"ba'r");
    do_test(parse_util_escape_string_with_quote(L"foo\\\\bar", L'"') == L"foo\\\\\\\\bar");
}

static void test_format() {
    say(L"Testing formatting functions");
    struct {
        unsigned long long val;
        const char *expected;
    } tests[] = {{0, "empty"},  {1, "1B"},       {2, "2B"},
                 {1024, "1kB"}, {1870, "1.8kB"}, {4322911, "4.1MB"}};
    for (const auto &test : tests) {
        char buff[128];
        format_size_safe(buff, test.val);
        do_test(!std::strcmp(buff, test.expected));
    }

    for (int j = -129; j <= 129; j++) {
        char buff1[128], buff2[128];
        format_long_safe(buff1, j);
        sprintf(buff2, "%d", j);
        do_test(!std::strcmp(buff1, buff2));

        wchar_t wbuf1[128], wbuf2[128];
        format_long_safe(wbuf1, j);
        std::swprintf(wbuf2, 128, L"%d", j);
        do_test(!std::wcscmp(wbuf1, wbuf2));
    }

    long q = LONG_MIN;
    char buff1[128], buff2[128];
    format_long_safe(buff1, q);
    sprintf(buff2, "%ld", q);
    do_test(!std::strcmp(buff1, buff2));
}

/// Helper to convert a narrow string to a sequence of hex digits.
static char *str2hex(const char *input) {
    char *output = (char *)malloc(5 * std::strlen(input) + 1);
    char *p = output;
    for (; *input; input++) {
        sprintf(p, "0x%02X ", (int)*input & 0xFF);
        p += 5;
    }
    *p = '\0';
    return output;
}

/// Test wide/narrow conversion by creating random strings and verifying that the original string
/// comes back through double conversion.
static void test_convert() {
    say(L"Testing wide/narrow string conversion");
    for (int i = 0; i < ESCAPE_TEST_COUNT; i++) {
        std::string orig{};
        while (random() % ESCAPE_TEST_LENGTH) {
            char c = random();
            orig.push_back(c);
        }

        const wcstring w = str2wcstring(orig);
        std::string n = wcs2string(w);
        if (orig != n) {
            char *o2 = str2hex(orig.c_str());
            char *n2 = str2hex(n.c_str());
            err(L"Line %d - %d: Conversion cycle of string:\n%4d chars: %s\n"
                L"produced different string:\n%4d chars: %s",
                __LINE__, i, orig.size(), o2, n.size(), n2);
            free(o2);
            free(n2);
        }
    }
}

/// Verify that ASCII narrow->wide conversions are correct.
static void test_convert_ascii() {
    std::string s(4096, '\0');
    for (size_t i = 0; i < s.size(); i++) {
        s[i] = (i % 10) + '0';
    }

    // Test a variety of alignments.
    for (size_t left = 0; left < 16; left++) {
        for (size_t right = 0; right < 16; right++) {
            const char *start = s.data() + left;
            size_t len = s.size() - left - right;
            wcstring wide = str2wcstring(start, len);
            std::string narrow = wcs2string(wide);
            do_test(narrow == std::string(start, len));
        }
    }

    // Put some non-ASCII bytes in and ensure it all still works.
    for (char &c : s) {
        char saved = c;
        c = 0xF7;
        do_test(wcs2string(str2wcstring(s)) == s);
        c = saved;
    }
}

/// fish uses the private-use range to encode bytes that could not be decoded using the user's
/// locale. If the input could be decoded, but decoded to private-use codepoints, then fish should
/// also use the direct encoding for those bytes. Verify that characters in the private use area are
/// correctly round-tripped. See #7723.
static void test_convert_private_use() {
    for (wchar_t wc = ENCODE_DIRECT_BASE; wc < ENCODE_DIRECT_END; wc++) {
        // Encode the char via the locale. Do not use fish functions which interpret these
        // specially.
        char converted[MB_LEN_MAX];
        mbstate_t state{};
        size_t len = std::wcrtomb(converted, wc, &state);
        if (len == static_cast<size_t>(-1)) {
            // Could not be encoded in this locale.
            continue;
        }
        std::string s(converted, len);

        // Ask fish to decode this via str2wcstring.
        // str2wcstring should notice that the decoded form collides with its private use and encode
        // it directly.
        wcstring ws = str2wcstring(s);

        // Each byte should be encoded directly, and round tripping should work.
        do_test(ws.size() == s.size());
        do_test(wcs2string(ws) == s);
    }
}

static void perf_convert_ascii() {
    std::string s(128 * 1024, '\0');
    for (size_t i = 0; i < s.size(); i++) {
        s[i] = (i % 10) + '0';
    }
    (void)str2wcstring(s);

    double start = timef();
    const int iters = 1024;
    for (int i = 0; i < iters; i++) {
        (void)str2wcstring(s);
    }
    double end = timef();
    auto usec = static_cast<unsigned long long>(((end - start) * 1E6) / iters);
    say(L"ASCII string conversion perf: %lu bytes in %llu usec", s.size(), usec);
}

/// Verify correct behavior with embedded nulls.
static void test_convert_nulls() {
    say(L"Testing convert_nulls");
    const wchar_t in[] = L"AAA\0BBB";
    const size_t in_len = (sizeof in / sizeof *in) - 1;
    const wcstring in_str = wcstring(in, in_len);
    std::string out_str = wcs2string(in_str);
    if (out_str.size() != in_len) {
        err(L"Embedded nulls mishandled in wcs2string");
    }
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] != out_str.at(i)) {
            err(L"Embedded nulls mishandled in wcs2string at index %lu", (unsigned long)i);
        }
    }

    wcstring out_wstr = str2wcstring(out_str);
    if (out_wstr.size() != in_len) {
        err(L"Embedded nulls mishandled in str2wcstring");
    }
    for (size_t i = 0; i < in_len; i++) {
        if (in[i] != out_wstr.at(i)) {
            err(L"Embedded nulls mishandled in str2wcstring at index %lu", (unsigned long)i);
        }
    }
}

/// Test the tokenizer.
static void test_tokenizer() {
    say(L"Testing tokenizer");
    {
        const wchar_t *str = L"alpha beta";
        tokenizer_t t(str, 0);
        maybe_t<tok_t> token{};

        token = t.next();  // alpha
        do_test(token.has_value());
        do_test(token->type == token_type_t::string);
        do_test(token->offset == 0);
        do_test(token->length == 5);
        do_test(t.text_of(*token) == L"alpha");

        token = t.next();  // beta
        do_test(token.has_value());
        do_test(token->type == token_type_t::string);
        do_test(token->offset == 6);
        do_test(token->length == 4);
        do_test(t.text_of(*token) == L"beta");

        token = t.next();
        do_test(!token.has_value());
    }

    const wchar_t *str =
        L"string <redirection  2>&1 'nested \"quoted\" '(string containing subshells "
        L"){and,brackets}$as[$well (as variable arrays)] not_a_redirect^ ^ ^^is_a_redirect "
        L"&| &> "
        L"&&& ||| "
        L"&& || & |"
        L"Compress_Newlines\n  \n\t\n   \nInto_Just_One";
    using tt = token_type_t;
    const token_type_t types[] = {
        tt::string,     tt::redirect, tt::string, tt::redirect, tt::string,   tt::string,
        tt::string,     tt::string,   tt::string, tt::pipe,     tt::redirect, tt::andand,
        tt::background, tt::oror,     tt::pipe,   tt::andand,   tt::oror,     tt::background,
        tt::pipe,       tt::string,   tt::end,    tt::string};

    say(L"Test correct tokenization");

    {
        tokenizer_t t(str, 0);
        size_t i = 0;
        while (auto token = t.next()) {
            if (i >= sizeof types / sizeof *types) {
                err(L"Too many tokens returned from tokenizer");
                std::fwprintf(stdout, L"Got excess token type %ld\n", (long)token->type);
                break;
            }
            if (types[i] != token->type) {
                err(L"Tokenization error:");
                std::fwprintf(
                    stdout,
                    L"Token number %zu of string \n'%ls'\n, expected type %ld, got token type "
                    L"%ld\n",
                    i + 1, str, (long)types[i], (long)token->type);
            }
            i++;
        }
        if (i < sizeof types / sizeof *types) {
            err(L"Too few tokens returned from tokenizer");
        }
    }

    // Test some errors.
    {
        tokenizer_t t(L"abc\\", 0);
        auto token = t.next();
        do_test(token.has_value());
        do_test(token->type == token_type_t::error);
        do_test(token->error == tokenizer_error_t::unterminated_escape);
        do_test(token->error_offset_within_token == 3);
    }

    {
        tokenizer_t t(L"abc )defg(hij", 0);
        auto token = t.next();
        do_test(token.has_value());
        token = t.next();
        do_test(token.has_value());
        do_test(token->type == token_type_t::error);
        do_test(token->error == tokenizer_error_t::closing_unopened_subshell);
        do_test(token->offset == 4);
        do_test(token->error_offset_within_token == 0);
    }

    {
        tokenizer_t t(L"abc defg(hij (klm)", 0);
        auto token = t.next();
        do_test(token.has_value());
        token = t.next();
        do_test(token.has_value());
        do_test(token->type == token_type_t::error);
        do_test(token->error == tokenizer_error_t::unterminated_subshell);
        do_test(token->error_offset_within_token == 4);
    }

    {
        tokenizer_t t(L"abc defg[hij (klm)", 0);
        auto token = t.next();
        do_test(token.has_value());
        token = t.next();
        do_test(token.has_value());
        do_test(token->type == token_type_t::error);
        do_test(token->error == tokenizer_error_t::unterminated_slice);
        do_test(token->error_offset_within_token == 4);
    }

    // Test some redirection parsing.
    auto pipe_or_redir = [](const wchar_t *s) { return pipe_or_redir_t::from_string(s); };
    do_test(pipe_or_redir(L"|")->is_pipe);
    do_test(pipe_or_redir(L"0>|")->is_pipe);
    do_test(pipe_or_redir(L"0>|")->fd == 0);
    do_test(pipe_or_redir(L"2>|")->is_pipe);
    do_test(pipe_or_redir(L"2>|")->fd == 2);
    do_test(pipe_or_redir(L">|")->is_pipe);
    do_test(pipe_or_redir(L">|")->fd == STDOUT_FILENO);
    do_test(!pipe_or_redir(L">")->is_pipe);
    do_test(pipe_or_redir(L">")->fd == STDOUT_FILENO);
    do_test(pipe_or_redir(L"2>")->fd == STDERR_FILENO);
    do_test(pipe_or_redir(L"9999999999999>")->fd == -1);
    do_test(pipe_or_redir(L"9999999999999>&2")->fd == -1);
    do_test(pipe_or_redir(L"9999999999999>&2")->is_valid() == false);
    do_test(pipe_or_redir(L"9999999999999>&2")->is_valid() == false);

    do_test(pipe_or_redir(L"&|")->is_pipe);
    do_test(pipe_or_redir(L"&|")->stderr_merge);
    do_test(!pipe_or_redir(L"&>")->is_pipe);
    do_test(pipe_or_redir(L"&>")->stderr_merge);
    do_test(pipe_or_redir(L"&>>")->stderr_merge);
    do_test(pipe_or_redir(L"&>?")->stderr_merge);

    auto get_redir_mode = [](const wchar_t *s) -> maybe_t<redirection_mode_t> {
        if (auto redir = pipe_or_redir_t::from_string(s)) {
            return redir->mode;
        }
        return none();
    };

    if (get_redir_mode(L"<") != redirection_mode_t::input)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L">") != redirection_mode_t::overwrite)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L"2>") != redirection_mode_t::overwrite)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L">>") != redirection_mode_t::append)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L"2>>") != redirection_mode_t::append)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L"2>?") != redirection_mode_t::noclob)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L"9999999999999999>?") != redirection_mode_t::noclob)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L"2>&3") != redirection_mode_t::fd)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L"3<&0") != redirection_mode_t::fd)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    if (get_redir_mode(L"3</tmp/filetxt") != redirection_mode_t::input)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);

    // Test ^ with our feature flag on and off.
    auto saved_flags = fish_features();
    mutable_fish_features().set(features_t::stderr_nocaret, false);
    if (get_redir_mode(L"^") != redirection_mode_t::overwrite)
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    mutable_fish_features().set(features_t::stderr_nocaret, true);
    if (get_redir_mode(L"^") != none())
        err(L"redirection_type_for_string failed on line %ld", (long)__LINE__);
    mutable_fish_features() = saved_flags;
}

// Little function that runs in a background thread, bouncing to the main.
static int test_iothread_thread_call(std::atomic<int> *addr) {
    int before = *addr;
    iothread_perform_on_main([=]() { *addr += 1; });
    int after = *addr;

    // Must have incremented it at least once.
    if (before >= after) {
        err(L"Failed to increment from background thread");
    }
    return after;
}

static void test_fd_monitor() {
    say(L"Testing fd_monitor");

    // Helper to make an item which counts how many times its callback is invoked.
    struct item_maker_t : public noncopyable_t {
        std::atomic<bool> did_timeout{false};
        std::atomic<size_t> length_read{0};
        std::atomic<size_t> pokes{0};
        std::atomic<size_t> total_calls{0};
        fd_monitor_item_id_t item_id{0};
        bool always_exit{false};
        fd_monitor_item_t item;
        autoclose_fd_t writer;

        explicit item_maker_t(uint64_t timeout_usec) {
            auto pipes = make_autoclose_pipes().acquire();
            writer = std::move(pipes.write);
            auto callback = [this](autoclose_fd_t &fd, item_wake_reason_t reason) {
                bool was_closed = false;
                switch (reason) {
                    case item_wake_reason_t::timeout:
                        this->did_timeout = true;
                        break;
                    case item_wake_reason_t::poke:
                        this->pokes += 1;
                        break;
                    case item_wake_reason_t::readable:
                        char buff[4096];
                        ssize_t amt = read(fd.fd(), buff, sizeof buff);
                        this->length_read += amt;
                        was_closed = (amt == 0);
                        break;
                }
                total_calls += 1;
                if (always_exit || was_closed) {
                    fd.close();
                }
            };
            item = fd_monitor_item_t(std::move(pipes.read), std::move(callback), timeout_usec);
        }

        // Write 42 bytes to our write end.
        void write42() const {
            char buff[42] = {0};
            (void)write_loop(writer.fd(), buff, sizeof buff);
        }
    };

    constexpr uint64_t usec_per_msec = 1000;

    // Items which will never receive data or be called back.
    item_maker_t item_never(fd_monitor_item_t::kNoTimeout);
    item_maker_t item_hugetimeout(100000000LLU * usec_per_msec);

    // Item which should get no data, and time out.
    item_maker_t item0_timeout(16 * usec_per_msec);

    // Item which should get exactly 42 bytes, then time out.
    item_maker_t item42_timeout(16 * usec_per_msec);

    // Item which should get exactly 42 bytes, and not time out.
    item_maker_t item42_nottimeout(fd_monitor_item_t::kNoTimeout);

    // Item which should get 42 bytes, then get notified it is closed.
    item_maker_t item42_thenclose(16 * usec_per_msec);

    // Item which gets one poke.
    item_maker_t item_pokee(fd_monitor_item_t::kNoTimeout);

    // Item which should be called back once.
    item_maker_t item_oneshot(16 * usec_per_msec);
    item_oneshot.always_exit = true;

    {
        fd_monitor_t monitor;
        for (item_maker_t *item :
             {&item_never, &item_hugetimeout, &item0_timeout, &item42_timeout, &item42_nottimeout,
              &item42_thenclose, &item_pokee, &item_oneshot}) {
            item->item_id = monitor.add(std::move(item->item));
        }
        item42_timeout.write42();
        item42_nottimeout.write42();
        item42_thenclose.write42();
        item42_thenclose.writer.close();
        item_oneshot.write42();
        monitor.poke_item(item_pokee.item_id);

        // May need to loop here to ensure our fd_monitor gets scheduled - see #7699.
        for (int i = 0; i < 100; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(84));
            if (item0_timeout.did_timeout) {
                break;
            }
        }
    }

    do_test(!item_never.did_timeout);
    do_test(item_never.length_read == 0);
    do_test(item_never.pokes == 0);

    do_test(!item_hugetimeout.did_timeout);
    do_test(item_hugetimeout.length_read == 0);
    do_test(item_hugetimeout.pokes == 0);

    do_test(item0_timeout.length_read == 0);
    do_test(item0_timeout.did_timeout);
    do_test(item0_timeout.pokes == 0);

    do_test(item42_timeout.length_read == 42);
    do_test(item42_timeout.did_timeout);
    do_test(item42_timeout.pokes == 0);

    do_test(item42_nottimeout.length_read == 42);
    do_test(!item42_nottimeout.did_timeout);
    do_test(item42_nottimeout.pokes == 0);

    do_test(item42_thenclose.did_timeout == false);
    do_test(item42_thenclose.length_read == 42);
    do_test(item42_thenclose.total_calls == 2);
    do_test(item42_thenclose.pokes == 0);

    do_test(!item_oneshot.did_timeout);
    do_test(item_oneshot.length_read == 42);
    do_test(item_oneshot.total_calls == 1);
    do_test(item_oneshot.pokes == 0);

    do_test(!item_pokee.did_timeout);
    do_test(item_pokee.length_read == 0);
    do_test(item_pokee.total_calls == 1);
    do_test(item_pokee.pokes == 1);
}

static void test_iothread() {
    say(L"Testing iothreads");
    std::unique_ptr<std::atomic<int>> int_ptr = make_unique<std::atomic<int>>(0);
    int iterations = 64;
    for (int i = 0; i < iterations; i++) {
        iothread_perform([&]() { test_iothread_thread_call(int_ptr.get()); });
    }
    iothread_drain_all();

    // Should have incremented it once per thread.
    do_test(*int_ptr == iterations);
    if (*int_ptr != iterations) {
        say(L"Expected int to be %d, but instead it was %d", iterations, int_ptr->load());
    }
}

static void test_pthread() {
    say(L"Testing pthreads");
    std::atomic<int> val{3};
    std::promise<void> promise;
    bool made = make_detached_pthread([&]() {
        val = val + 2;
        promise.set_value();
    });
    do_test(made);
    promise.get_future().wait();
    do_test(val == 5);
}

static void test_debounce() {
    say(L"Testing debounce");
    // Run 8 functions using a condition variable.
    // Only the first and last should run.
    debounce_t db;
    constexpr size_t count = 8;
    std::array<bool, count> handler_ran = {};
    std::array<bool, count> completion_ran = {};

    bool ready_to_go = false;
    std::mutex m;
    std::condition_variable cv;

    // "Enqueue" all functions. Each one waits until ready_to_go.
    for (size_t idx = 0; idx < count; idx++) {
        do_test(handler_ran[idx] == false);
        db.perform(
            [&, idx] {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return ready_to_go; });
                handler_ran[idx] = true;
                return idx;
            },
            [&](size_t idx) { completion_ran[idx] = true; });
    }

    // We're ready to go.
    {
        std::unique_lock<std::mutex> lock(m);
        ready_to_go = true;
    }
    cv.notify_all();

    // Wait until the last completion is done.
    while (!completion_ran.back()) {
        iothread_service_main();
    }
    iothread_drain_all();

    // Each perform() call may displace an existing queued operation.
    // Each operation waits until all are queued.
    // Therefore we expect the last perform() to have run, and at most one more.

    do_test(handler_ran.back());
    do_test(completion_ran.back());

    size_t total_ran = 0;
    for (size_t idx = 0; idx < count; idx++) {
        total_ran += (handler_ran[idx] ? 1 : 0);
        do_test(handler_ran[idx] == completion_ran[idx]);
    }
    do_test(total_ran <= 2);
}

static void test_debounce_timeout() {
    using namespace std::chrono;
    say(L"Testing debounce timeout");

    // Verify that debounce doesn't wait forever.
    // Use a shared_ptr so we don't have to join our threads.
    const long timeout_ms = 50;
    struct data_t {
        debounce_t db{timeout_ms};
        bool exit_ok = false;
        std::mutex m;
        std::condition_variable cv;
        relaxed_atomic_t<uint32_t> running{0};
    };
    auto data = std::make_shared<data_t>();

    // Our background handler. Note this just blocks until exit_ok is set.
    std::function<void()> handler = [data] {
        data->running++;
        std::unique_lock<std::mutex> lock(data->m);
        data->cv.wait(lock, [&] { return data->exit_ok; });
    };

    // Spawn the handler twice. This should not modify the thread token.
    uint64_t token1 = data->db.perform(handler);
    uint64_t token2 = data->db.perform(handler);
    do_test(token1 == token2);

    // Wait 75 msec, then enqueue something else; this should spawn a new thread.
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms + timeout_ms / 2));
    do_test(data->running == 1);
    uint64_t token3 = data->db.perform(handler);
    do_test(token3 > token2);

    // Release all the threads.
    std::unique_lock<std::mutex> lock(data->m);
    data->exit_ok = true;
    data->cv.notify_all();
}

static parser_test_error_bits_t detect_argument_errors(const wcstring &src) {
    using namespace ast;
    auto ast = ast_t::parse_argument_list(src, parse_flag_none);
    if (ast.errored()) {
        return PARSER_TEST_ERROR;
    }
    const ast::argument_t *first_arg =
        ast.top()->as<freestanding_argument_list_t>()->arguments.at(0);
    if (!first_arg) {
        err(L"Failed to parse an argument");
        return 0;
    }
    return parse_util_detect_errors_in_argument(*first_arg, first_arg->source(src));
}

/// Test the parser.
static void test_parser() {
    say(L"Testing parser");

    auto detect_errors = [](const wcstring &s) {
        return parse_util_detect_errors(s, nullptr, true /* accept incomplete */);
    };

    say(L"Testing block nesting");
    if (!detect_errors(L"if; end")) {
        err(L"Incomplete if statement undetected");
    }
    if (!detect_errors(L"if test; echo")) {
        err(L"Missing end undetected");
    }
    if (!detect_errors(L"if test; end; end")) {
        err(L"Unbalanced end undetected");
    }

    say(L"Testing detection of invalid use of builtin commands");
    if (!detect_errors(L"case foo")) {
        err(L"'case' command outside of block context undetected");
    }
    if (!detect_errors(L"switch ggg; if true; case foo;end;end")) {
        err(L"'case' command outside of switch block context undetected");
    }
    if (!detect_errors(L"else")) {
        err(L"'else' command outside of conditional block context undetected");
    }
    if (!detect_errors(L"else if")) {
        err(L"'else if' command outside of conditional block context undetected");
    }
    if (!detect_errors(L"if false; else if; end")) {
        err(L"'else if' missing command undetected");
    }

    if (!detect_errors(L"break")) {
        err(L"'break' command outside of loop block context undetected");
    }

    if (detect_errors(L"break --help")) {
        err(L"'break --help' incorrectly marked as error");
    }

    if (!detect_errors(L"while false ; function foo ; break ; end ; end ")) {
        err(L"'break' command inside function allowed to break from loop outside it");
    }

    if (!detect_errors(L"exec ls|less") || !detect_errors(L"echo|return")) {
        err(L"Invalid pipe command undetected");
    }

    if (detect_errors(L"for i in foo ; switch $i ; case blah ; break; end; end ")) {
        err(L"'break' command inside switch falsely reported as error");
    }

    if (detect_errors(L"or cat | cat") || detect_errors(L"and cat | cat")) {
        err(L"boolean command at beginning of pipeline falsely reported as error");
    }

    if (!detect_errors(L"cat | and cat")) {
        err(L"'and' command in pipeline not reported as error");
    }

    if (!detect_errors(L"cat | or cat")) {
        err(L"'or' command in pipeline not reported as error");
    }

    if (!detect_errors(L"cat | exec") || !detect_errors(L"exec | cat")) {
        err(L"'exec' command in pipeline not reported as error");
    }

    if (!detect_errors(L"begin ; end arg")) {
        err(L"argument to 'end' not reported as error");
    }

    if (!detect_errors(L"switch foo ; end arg")) {
        err(L"argument to 'end' not reported as error");
    }

    if (!detect_errors(L"if true; else if false ; end arg")) {
        err(L"argument to 'end' not reported as error");
    }

    if (!detect_errors(L"if true; else ; end arg")) {
        err(L"argument to 'end' not reported as error");
    }

    if (detect_errors(L"begin ; end 2> /dev/null")) {
        err(L"redirection after 'end' wrongly reported as error");
    }

    if (detect_errors(L"true | ") != PARSER_TEST_INCOMPLETE) {
        err(L"unterminated pipe not reported properly");
    }

    if (detect_errors(L"echo (\nfoo\n  bar") != PARSER_TEST_INCOMPLETE) {
        err(L"unterminated multiline subshell not reported properly");
    }

    if (detect_errors(L"begin ; true ; end | ") != PARSER_TEST_INCOMPLETE) {
        err(L"unterminated pipe not reported properly");
    }

    if (detect_errors(L" | true ") != PARSER_TEST_ERROR) {
        err(L"leading pipe not reported properly");
    }

    if (detect_errors(L"true | # comment") != PARSER_TEST_INCOMPLETE) {
        err(L"comment after pipe not reported as incomplete");
    }

    if (detect_errors(L"true | # comment \n false ")) {
        err(L"comment and newline after pipe wrongly reported as error");
    }

    if (detect_errors(L"true | ; false ") != PARSER_TEST_ERROR) {
        err(L"semicolon after pipe not detected as error");
    }

    if (detect_argument_errors(L"foo")) {
        err(L"simple argument reported as error");
    }

    if (detect_argument_errors(L"''")) {
        err(L"Empty string reported as error");
    }

    if (!(detect_argument_errors(L"foo$$") & PARSER_TEST_ERROR)) {
        err(L"Bad variable expansion not reported as error");
    }

    if (!(detect_argument_errors(L"foo$@") & PARSER_TEST_ERROR)) {
        err(L"Bad variable expansion not reported as error");
    }

    // Within command substitutions, we should be able to detect everything that
    // parse_util_detect_errors can detect.
    if (!(detect_argument_errors(L"foo(cat | or cat)") & PARSER_TEST_ERROR)) {
        err(L"Bad command substitution not reported as error");
    }

    if (!(detect_argument_errors(L"foo\\xFF9") & PARSER_TEST_ERROR)) {
        err(L"Bad escape not reported as error");
    }

    if (!(detect_argument_errors(L"foo(echo \\xFF9)") & PARSER_TEST_ERROR)) {
        err(L"Bad escape in command substitution not reported as error");
    }

    if (!(detect_argument_errors(L"foo(echo (echo (echo \\xFF9)))") & PARSER_TEST_ERROR)) {
        err(L"Bad escape in nested command substitution not reported as error");
    }

    if (!detect_errors(L"false & ; and cat")) {
        err(L"'and' command after background not reported as error");
    }

    if (!detect_errors(L"true & ; or cat")) {
        err(L"'or' command after background not reported as error");
    }

    if (detect_errors(L"true & ; not cat")) {
        err(L"'not' command after background falsely reported as error");
    }

    if (!detect_errors(L"if true & ; end")) {
        err(L"backgrounded 'if' conditional not reported as error");
    }

    if (!detect_errors(L"if false; else if true & ; end")) {
        err(L"backgrounded 'else if' conditional not reported as error");
    }

    if (!detect_errors(L"while true & ; end")) {
        err(L"backgrounded 'while' conditional not reported as error");
    }

    if (!detect_errors(L"true | || false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (!detect_errors(L"|| false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (!detect_errors(L"&& false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (!detect_errors(L"true ; && false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (!detect_errors(L"true ; || false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (!detect_errors(L"true || && false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (!detect_errors(L"true && || false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (!detect_errors(L"true && && false")) {
        err(L"bogus boolean statement error not detected on line %d", __LINE__);
    }

    if (detect_errors(L"true && ") != PARSER_TEST_INCOMPLETE) {
        err(L"unterminated conjunction not reported properly");
    }

    if (detect_errors(L"true && \n true")) {
        err(L"newline after && reported as error");
    }

    if (detect_errors(L"true || \n") != PARSER_TEST_INCOMPLETE) {
        err(L"unterminated conjunction not reported properly");
    }

    say(L"Testing basic evaluation");

    // Ensure that we don't crash on infinite self recursion and mutual recursion. These must use
    // the principal parser because we cannot yet execute jobs on other parsers.
    auto parser = parser_t::principal_parser().shared();
    say(L"Testing recursion detection");
    parser->eval(L"function recursive ; recursive ; end ; recursive; ", io_chain_t());

    parser->eval(
        L"function recursive1 ; recursive2 ; end ; "
        L"function recursive2 ; recursive1 ; end ; recursive1; ",
        io_chain_t());

    say(L"Testing empty function name");
    parser->eval(L"function '' ; echo fail; exit 42 ; end ; ''", io_chain_t());

    say(L"Testing eval_args");
    completion_list_t comps = parser_t::expand_argument_list(L"alpha 'beta gamma' delta",
                                                             expand_flags_t{}, parser->context());
    do_test(comps.size() == 3);
    do_test(comps.at(0).completion == L"alpha");
    do_test(comps.at(1).completion == L"beta gamma");
    do_test(comps.at(2).completion == L"delta");
}

static void test_1_cancellation(const wchar_t *src) {
    auto filler = io_bufferfill_t::create();
    pthread_t thread = pthread_self();
    double delay = 0.50 /* seconds */;
    iothread_perform([=]() {
        /// Wait a while and then SIGINT the main thread.
        usleep(delay * 1E6);
        pthread_kill(thread, SIGINT);
    });
    eval_res_t res = parser_t::principal_parser().eval(src, io_chain_t{filler});
    separated_buffer_t buffer = io_bufferfill_t::finish(std::move(filler));
    if (buffer.size() != 0) {
        err(L"Expected 0 bytes in out_buff, but instead found %lu bytes, for command %ls\n",
            buffer.size(), src);
    }
    do_test(res.status.signal_exited() && res.status.signal_code() == SIGINT);
    iothread_drain_all();
}

static void test_cancellation() {
    say(L"Testing Ctrl-C cancellation. If this hangs, that's a bug!");

    // Enable fish's signal handling here.
    signal_set_handlers(true);

    // This tests that we can correctly ctrl-C out of certain loop constructs, and that nothing gets
    // printed if we do.

    // Here the command substitution is an infinite loop. echo never even gets its argument, so when
    // we cancel we expect no output.
    test_1_cancellation(L"echo (while true ; echo blah ; end)");

    // Nasty infinite loop that doesn't actually execute anything.
    test_1_cancellation(L"echo (while true ; end) (while true ; end) (while true ; end)");
    test_1_cancellation(L"while true ; end");
    test_1_cancellation(L"while true ; echo nothing > /dev/null; end");
    test_1_cancellation(L"for i in (while true ; end) ; end");

    signal_reset_handlers();

    // Ensure that we don't think we should cancel.
    reader_reset_interrupted();
    signal_clear_cancel();
}

namespace indent_tests {
// A struct which is either text or a new indent.
struct segment_t {
    // The indent to set
    int indent{0};
    const char *text{nullptr};

    /* implicit */ segment_t(int indent) : indent(indent) {}
    /* implicit */ segment_t(const char *text) : text(text) {}
};

using test_t = std::vector<segment_t>;
using test_list_t = std::vector<test_t>;

// Add a new test to a test list based on a series of ints and texts.
template <typename... Types>
void add_test(test_list_t *v, const Types &...types) {
    segment_t segments[] = {types...};
    v->emplace_back(std::begin(segments), std::end(segments));
}
}  // namespace indent_tests

static void test_indents() {
    say(L"Testing indents");
    using namespace indent_tests;

    test_list_t tests;
    add_test(&tests,              //
             0, "if", 1, " foo",  //
             0, "\nend");

    add_test(&tests,              //
             0, "if", 1, " foo",  //
             1, "\nfoo",          //
             0, "\nend");

    add_test(&tests,                //
             0, "if", 1, " foo",    //
             1, "\nif", 2, " bar",  //
             1, "\nend",            //
             0, "\nend");

    add_test(&tests,                //
             0, "if", 1, " foo",    //
             1, "\nif", 2, " bar",  //
             2, "\n",               //
             1, "\nend\n");

    add_test(&tests,                //
             0, "if", 1, " foo",    //
             1, "\nif", 2, " bar",  //
             2, "\n");

    add_test(&tests,      //
             0, "begin",  //
             1, "\nfoo",  //
             1, "\n");

    add_test(&tests,      //
             0, "begin",  //
             1, "\n;",    //
             0, "end",    //
             0, "\nfoo", 0, "\n");

    add_test(&tests,      //
             0, "begin",  //
             1, "\n;",    //
             0, "end",    //
             0, "\nfoo", 0, "\n");

    add_test(&tests,                //
             0, "if", 1, " foo",    //
             1, "\nif", 2, " bar",  //
             2, "\nbaz",            //
             1, "\nend", 1, "\n");

    add_test(&tests,           //
             0, "switch foo",  //
             1, "\n"           //
    );

    add_test(&tests,           //
             0, "switch foo",  //
             1, "\ncase bar",  //
             1, "\ncase baz",  //
             2, "\nquux",      //
             2, "\nquux"       //
    );

    add_test(&tests,           //
             0, "switch foo",  //
             1, "\ncas"        // parse error indentation handling
    );

    add_test(&tests,                   //
             0, "while", 1, " false",  //
             1, "\n# comment",         // comment indentation handling
             1, "\ncommand",           //
             1, "\n# comment 2"        //
    );

    add_test(&tests,      //
             0, "begin",  //
             1, "\n",     // "begin" is special because this newline belongs to the block header
             1, "\n"      //
    );

    // Continuation lines.
    add_test(&tests,                            //
             0, "echo 'continuation line' \\",  //
             1, "\ncont",                       //
             0, "\n"                            //
    );
    add_test(&tests,                                  //
             0, "echo 'empty continuation line' \\",  //
             1, "\n"                                  //
    );
    add_test(&tests,                                   //
             0, "begin # continuation line in block",  //
             1, "\necho \\",                           //
             2, "\ncont"                               //
    );
    add_test(&tests,                                         //
             0, "begin # empty continuation line in block",  //
             1, "\necho \\",                                 //
             2, "\n",                                        //
             0, "\nend"                                      //
    );
    add_test(&tests,                                      //
             0, "echo 'multiple continuation lines' \\",  //
             1, "\nline1 \\",                             //
             1, "\n# comment",                            //
             1, "\n# more comment",                       //
             1, "\nline2 \\",                             //
             1, "\n"                                      //
    );
    add_test(&tests,                                   //
             0, "echo # inline comment ending in \\",  //
             0, "\nline"                               //
    );
    add_test(&tests,                            //
             0, "# line comment ending in \\",  //
             0, "\nline"                        //
    );
    add_test(&tests,                                            //
             0, "echo 'multiple empty continuation lines' \\",  //
             1, "\n\\",                                         //
             1, "\n",                                           //
             0, "\n"                                            //
    );
    add_test(&tests,                                                      //
             0, "echo 'multiple statements with continuation lines' \\",  //
             1, "\nline 1",                                               //
             0, "\necho \\",                                              //
             1, "\n"                                                      //
    );
    // This is an edge case, probably okay to change the behavior here.
    add_test(&tests,                                              //
             0, "begin", 1, " \\",                                //
             2, "\necho 'continuation line in block header' \\",  //
             2, "\n",                                             //
             1, "\n",                                             //
             0, "\nend"                                           //
    );

    int test_idx = 0;
    for (const test_t &test : tests) {
        // Construct the input text and expected indents.
        wcstring text;
        std::vector<int> expected_indents;
        int current_indent = 0;
        for (const segment_t &segment : test) {
            if (!segment.text) {
                current_indent = segment.indent;
            } else {
                wcstring tmp = str2wcstring(segment.text);
                text.append(tmp);
                expected_indents.insert(expected_indents.end(), tmp.size(), current_indent);
            }
        }
        do_test(expected_indents.size() == text.size());

        // Compute the indents.
        std::vector<int> indents = parse_util_compute_indents(text);

        if (expected_indents.size() != indents.size()) {
            err(L"Indent vector has wrong size! Expected %lu, actual %lu", expected_indents.size(),
                indents.size());
        }
        do_test(expected_indents.size() == indents.size());
        for (size_t i = 0; i < text.size(); i++) {
            if (expected_indents.at(i) != indents.at(i)) {
                err(L"Wrong indent at index %lu (char 0x%02x) in test #%lu (expected %d, actual "
                    L"%d):\n%ls\n",
                    i, text.at(i), test_idx, expected_indents.at(i), indents.at(i), text.c_str());
                break;  // don't keep showing errors for the rest of the test
            }
        }
        test_idx++;
    }
}

static void test_parse_util_cmdsubst_extent() {
    const wchar_t *a = L"echo (echo (echo hi";
    const wchar_t *begin = nullptr, *end = nullptr;

    parse_util_cmdsubst_extent(a, 0, &begin, &end);
    if (begin != a || end != begin + std::wcslen(begin)) {
        err(L"parse_util_cmdsubst_extent failed on line %ld", (long)__LINE__);
    }
    parse_util_cmdsubst_extent(a, 1, &begin, &end);
    if (begin != a || end != begin + std::wcslen(begin)) {
        err(L"parse_util_cmdsubst_extent failed on line %ld", (long)__LINE__);
    }
    parse_util_cmdsubst_extent(a, 2, &begin, &end);
    if (begin != a || end != begin + std::wcslen(begin)) {
        err(L"parse_util_cmdsubst_extent failed on line %ld", (long)__LINE__);
    }
    parse_util_cmdsubst_extent(a, 3, &begin, &end);
    if (begin != a || end != begin + std::wcslen(begin)) {
        err(L"parse_util_cmdsubst_extent failed on line %ld", (long)__LINE__);
    }

    parse_util_cmdsubst_extent(a, 8, &begin, &end);
    if (begin != a + const_strlen(L"echo (")) {
        err(L"parse_util_cmdsubst_extent failed on line %ld", (long)__LINE__);
    }

    parse_util_cmdsubst_extent(a, 17, &begin, &end);
    if (begin != a + const_strlen(L"echo (echo (")) {
        err(L"parse_util_cmdsubst_extent failed on line %ld", (long)__LINE__);
    }
}

static struct wcsfilecmp_test {
    const wchar_t *str1;
    const wchar_t *str2;
    int expected_rc;
} wcsfilecmp_tests[] = {{L"", L"", 0},
                        {L"", L"def", -1},
                        {L"abc", L"", 1},
                        {L"abc", L"def", -1},
                        {L"abc", L"DEF", -1},
                        {L"DEF", L"abc", 1},
                        {L"abc", L"abc", 0},
                        {L"ABC", L"ABC", 0},
                        {L"AbC", L"abc", -1},
                        {L"AbC", L"ABC", 1},
                        {L"def", L"abc", 1},
                        {L"1ghi", L"1gHi", 1},
                        {L"1ghi", L"2ghi", -1},
                        {L"1ghi", L"01ghi", 1},
                        {L"1ghi", L"02ghi", -1},
                        {L"01ghi", L"1ghi", -1},
                        {L"1ghi", L"002ghi", -1},
                        {L"002ghi", L"1ghi", 1},
                        {L"abc01def", L"abc1def", -1},
                        {L"abc1def", L"abc01def", 1},
                        {L"abc12", L"abc5", 1},
                        {L"51abc", L"050abc", 1},
                        {L"abc5", L"abc12", -1},
                        {L"5abc", L"12ABC", -1},
                        {L"abc0789", L"abc789", -1},
                        {L"abc0xA789", L"abc0xA0789", 1},
                        {L"abc002", L"abc2", -1},
                        {L"abc002g", L"abc002", 1},
                        {L"abc002g", L"abc02g", -1},
                        {L"abc002.txt", L"abc02.txt", -1},
                        {L"abc005", L"abc012", -1},
                        {L"abc02", L"abc002", 1},
                        {L"abc002.txt", L"abc02.txt", -1},
                        {L"GHI1abc2.txt", L"ghi1abc2.txt", -1},
                        {L"a0", L"a00", -1},
                        {L"a00b", L"a0b", -1},
                        {L"a0b", L"a00b", 1},
                        {L"a-b", L"azb", 1},
                        {nullptr, nullptr, 0}};

/// Verify the behavior of the `wcsfilecmp()` function.
static void test_wcsfilecmp() {
    for (auto test = wcsfilecmp_tests; test->str1; test++) {
        int rc = wcsfilecmp(test->str1, test->str2);
        if (rc != test->expected_rc) {
            err(L"New failed on line %lu: [\"%ls\" <=> \"%ls\"]: "
                L"expected return code %d but got %d",
                __LINE__, test->str1, test->str2, test->expected_rc, rc);
        }
    }
}

static void test_const_strlen() {
    do_test(const_strlen("") == 0);
    do_test(const_strlen(L"") == 0);
    do_test(const_strlen("\0") == 0);
    do_test(const_strlen(L"\0") == 0);
    do_test(const_strlen("\0abc") == 0);
    do_test(const_strlen(L"\0abc") == 0);
    do_test(const_strlen("x") == 1);
    do_test(const_strlen(L"x") == 1);
    do_test(const_strlen("abc") == 3);
    do_test(const_strlen(L"abc") == 3);
    do_test(const_strlen("abc\0def") == 3);
    do_test(const_strlen(L"abc\0def") == 3);
    do_test(const_strlen("abcdef\0") == 6);
    do_test(const_strlen(L"abcdef\0") == 6);
    static_assert(const_strlen("hello") == 5, "const_strlen failure");
}

static void test_const_strcmp() {
    static_assert(const_strcmp("", "a") < 0, "const_strcmp failure");
    static_assert(const_strcmp("a", "a") == 0, "const_strcmp failure");
    static_assert(const_strcmp("a", "") > 0, "const_strcmp failure");
    static_assert(const_strcmp("aa", "a") > 0, "const_strcmp failure");
    static_assert(const_strcmp("a", "aa") < 0, "const_strcmp failure");
    static_assert(const_strcmp("b", "aa") > 0, "const_strcmp failure");
}

static void test_is_sorted_by_name() {
    struct named_t {
        const wchar_t *name;
    };

    static constexpr named_t sorted[] = {
        {L"a"}, {L"aa"}, {L"aaa"}, {L"aaa"}, {L"aaa"}, {L"aazz"}, {L"aazzzz"},
    };
    static_assert(is_sorted_by_name(sorted), "is_sorted_by_name failure");
    do_test(get_by_sorted_name(L"", sorted) == nullptr);
    do_test(get_by_sorted_name(L"nope", sorted) == nullptr);
    do_test(get_by_sorted_name(L"aaaaaaaaaaa", sorted) == nullptr);
    wcstring last;
    for (const auto &v : sorted) {
        // We have multiple items with the same name; only test the first.
        if (last != v.name) {
            last = v.name;
            do_test(get_by_sorted_name(last, sorted) == &v);
        }
    }

    static constexpr named_t not_sorted[] = {
        {L"a"}, {L"aa"}, {L"aaa"}, {L"q"}, {L"aazz"}, {L"aazz"}, {L"aazz"}, {L"aazzzz"},
    };
    static_assert(!is_sorted_by_name(not_sorted), "is_sorted_by_name failure");
}

static void test_utility_functions() {
    say(L"Testing utility functions");
    test_wcsfilecmp();
    test_parse_util_cmdsubst_extent();
    test_const_strlen();
    test_const_strcmp();
    test_is_sorted_by_name();
}

// UTF8 tests taken from Alexey Vatchenko's utf8 library. See http://www.bsdua.org/libbsdua.html.
static void test_utf82wchar(const char *src, size_t slen, const wchar_t *dst, size_t dlen,
                            int flags, size_t res, const char *descr) {
    size_t size;
    wchar_t *mem = nullptr;

#if WCHAR_MAX == 0xffff
    // Hack: if wchar is only UCS-2, and the UTF-8 input string contains astral characters, then
    // tweak the expected size to 0.
    if (src) {
        // A UTF-8 code unit may represent an astral code point if it has 4 or more leading 1s.
        const unsigned char astral_mask = 0xF0;
        for (size_t i = 0; i < slen; i++) {
            if ((src[i] & astral_mask) == astral_mask) {
                // Astral char. We want this conversion to fail.
                res = 0;  //!OCLINT(parameter reassignment)
                break;
            }
        }
    }
#endif

    if (!dst) {
        size = utf8_to_wchar(src, slen, nullptr, flags);
    } else {
        mem = (wchar_t *)malloc(dlen * sizeof(*mem));
        if (!mem) {
            err(L"u2w: %s: MALLOC FAILED\n", descr);
            return;
        }

        std::wstring buff;
        size = utf8_to_wchar(src, slen, &buff, flags);
        std::copy(buff.begin(), buff.begin() + std::min(dlen, buff.size()), mem);
    }

    if (res != size) {
        err(L"u2w: %s: FAILED (rv: %lu, must be %lu)", descr, size, res);
    } else if (mem && std::memcmp(mem, dst, size * sizeof(*mem)) != 0) {
        err(L"u2w: %s: BROKEN", descr);
    }

    free(mem);
}

// Annoying variant to handle uchar to avoid narrowing conversion warnings.
static void test_utf82wchar(const unsigned char *usrc, size_t slen, const wchar_t *dst, size_t dlen,
                            int flags, size_t res, const char *descr) {
    const char *src = reinterpret_cast<const char *>(usrc);
    return test_utf82wchar(src, slen, dst, dlen, flags, res, descr);
}

static void test_wchar2utf8(const wchar_t *src, size_t slen, const char *dst, size_t dlen,
                            int flags, size_t res, const char *descr) {
    size_t size;
    char *mem = nullptr;

#if WCHAR_MAX == 0xffff
    // Hack: if wchar is simulating UCS-2, and the wchar_t input string contains astral characters,
    // then tweak the expected size to 0.
    if (src) {
        const uint32_t astral_mask = 0xFFFF0000U;
        for (size_t i = 0; i < slen; i++) {
            if ((src[i] & astral_mask) != 0) {
                // Astral char. We want this conversion to fail.
                res = 0;  //!OCLINT(parameter reassignment)
                break;
            }
        }
    }
#endif

    if (dst) {
        // We want to pass a valid pointer to wchar_to_utf8, so allocate at least one byte.
        mem = (char *)malloc(dlen + 1);
        if (!mem) {
            err(L"w2u: %s: MALLOC FAILED", descr);
            return;
        }
    }

    size = wchar_to_utf8(src, slen, mem, dlen, flags);
    if (res != size) {
        err(L"w2u: %s: FAILED (rv: %lu, must be %lu)", descr, size, res);
    } else if (dst && std::memcmp(mem, dst, size) != 0) {
        err(L"w2u: %s: BROKEN", descr);
    }

    free(mem);
}

// Annoying variant to handle uchar to avoid narrowing conversion warnings.
static void test_wchar2utf8(const wchar_t *src, size_t slen, const unsigned char *udst, size_t dlen,
                            int flags, size_t res, const char *descr) {
    const char *dst = reinterpret_cast<const char *>(udst);
    return test_wchar2utf8(src, slen, dst, dlen, flags, res, descr);
}

static void test_utf8() {
    say(L"Testing utf8");
    wchar_t w1[] = {0x54, 0x65, 0x73, 0x74};
    wchar_t w2[] = {0x0422, 0x0435, 0x0441, 0x0442};
    wchar_t w3[] = {0x800, 0x1e80, 0x98c4, 0x9910, 0xff00};
    wchar_t wm[] = {0x41, 0x0441, 0x3042, 0xff67, 0x9b0d};
    wchar_t wb2[] = {0xd800, 0xda00, 0x41, 0xdfff, 0x0a};
    wchar_t wbom[] = {0xfeff, 0x41, 0x0a};
    wchar_t wbom2[] = {0x41, 0xa};
    wchar_t wbom22[] = {0xfeff, 0x41, 0x0a};
    unsigned char u1[] = {0x54, 0x65, 0x73, 0x74};
    unsigned char u2[] = {0xd0, 0xa2, 0xd0, 0xb5, 0xd1, 0x81, 0xd1, 0x82};
    unsigned char u3[] = {0xe0, 0xa0, 0x80, 0xe1, 0xba, 0x80, 0xe9, 0xa3,
                          0x84, 0xe9, 0xa4, 0x90, 0xef, 0xbc, 0x80};
    unsigned char um[] = {0x41, 0xd1, 0x81, 0xe3, 0x81, 0x82, 0xef, 0xbd, 0xa7, 0xe9, 0xac, 0x8d};
    unsigned char uc080[] = {0xc0, 0x80};
    unsigned char ub2[] = {0xed, 0xa1, 0x8c, 0xed, 0xbe, 0xb4, 0x0a};
    unsigned char ubom[] = {0x41, 0xa};
    unsigned char ubom2[] = {0xef, 0xbb, 0xbf, 0x41, 0x0a};
#if WCHAR_MAX != 0xffff
    wchar_t w4[] = {0x15555, 0xf7777, 0x0a};
    wchar_t wb[] = {(wchar_t)-2, 0xa, (wchar_t)0xffffffff, 0x0441};
    wchar_t wb1[] = {0x0a, 0x0422};
    unsigned char u4[] = {0xf0, 0x95, 0x95, 0x95, 0xf3, 0xb7, 0x9d, 0xb7, 0x0a};
    unsigned char ub[] = {0xa, 0xd1, 0x81};
    unsigned char ub1[] = {0xa, 0xff, 0xd0, 0xa2, 0xfe, 0x8f, 0xe0, 0x80};
#endif

    // UTF-8 -> UCS-4 string.
    test_utf82wchar(ubom2, sizeof(ubom2), wbom2, sizeof(wbom2) / sizeof(*wbom2), UTF8_SKIP_BOM,
                    sizeof(wbom2) / sizeof(*wbom2), "ubom2 skip BOM");
    test_utf82wchar(ubom2, sizeof(ubom2), wbom22, sizeof(wbom22) / sizeof(*wbom22), 0,
                    sizeof(wbom22) / sizeof(*wbom22), "ubom2 BOM");
    test_utf82wchar(uc080, sizeof(uc080), nullptr, 0, 0, 0, "uc080 c0 80 - forbitten by rfc3629");
    test_utf82wchar(ub2, sizeof(ub2), nullptr, 0, 0, 3, "ub2 resulted in forbitten wchars (len)");
    test_utf82wchar(ub2, sizeof(ub2), wb2, sizeof(wb2) / sizeof(*wb2), 0, 0,
                    "ub2 resulted in forbitten wchars");
    test_utf82wchar(ub2, sizeof(ub2), L"\x0a", 1, UTF8_IGNORE_ERROR, 1,
                    "ub2 resulted in ignored forbitten wchars");
    test_utf82wchar(u1, sizeof(u1), w1, sizeof(w1) / sizeof(*w1), 0, sizeof(w1) / sizeof(*w1),
                    "u1/w1 1 octet chars");
    test_utf82wchar(u2, sizeof(u2), w2, sizeof(w2) / sizeof(*w2), 0, sizeof(w2) / sizeof(*w2),
                    "u2/w2 2 octets chars");
    test_utf82wchar(u3, sizeof(u3), w3, sizeof(w3) / sizeof(*w3), 0, sizeof(w3) / sizeof(*w3),
                    "u3/w3 3 octets chars");
    test_utf82wchar("\xff", 1, nullptr, 0, 0, 0, "broken utf-8 0xff symbol");
    test_utf82wchar("\xfe", 1, nullptr, 0, 0, 0, "broken utf-8 0xfe symbol");
    test_utf82wchar("\x8f", 1, nullptr, 0, 0, 0, "broken utf-8, start from 10 higher bits");
    test_utf82wchar((const char *)nullptr, 0, nullptr, 0, 0, 0, "invalid params, all 0");
    test_utf82wchar(u1, 0, nullptr, 0, 0, 0, "invalid params, src buf not NULL");
    test_utf82wchar((const char *)nullptr, 10, nullptr, 0, 0, 0,
                    "invalid params, src length is not 0");

    // UCS-4 -> UTF-8 string.
    const char *const nullc = nullptr;
    test_wchar2utf8(wbom, sizeof(wbom) / sizeof(*wbom), ubom, sizeof(ubom), UTF8_SKIP_BOM,
                    sizeof(ubom), "BOM");
    test_wchar2utf8(wb2, sizeof(wb2) / sizeof(*wb2), nullc, 0, 0, 0, "prohibited wchars");
    test_wchar2utf8(wb2, sizeof(wb2) / sizeof(*wb2), nullc, 0, UTF8_IGNORE_ERROR, 2,
                    "ignore prohibited wchars");
    test_wchar2utf8(w1, sizeof(w1) / sizeof(*w1), u1, sizeof(u1), 0, sizeof(u1),
                    "w1/u1 1 octet chars");
    test_wchar2utf8(w2, sizeof(w2) / sizeof(*w2), u2, sizeof(u2), 0, sizeof(u2),
                    "w2/u2 2 octets chars");
    test_wchar2utf8(w3, sizeof(w3) / sizeof(*w3), u3, sizeof(u3), 0, sizeof(u3),
                    "w3/u3 3 octets chars");
    test_wchar2utf8(nullptr, 0, nullc, 0, 0, 0, "invalid params, all 0");
    test_wchar2utf8(w1, 0, nullc, 0, 0, 0, "invalid params, src buf not NULL");
    test_wchar2utf8(w1, sizeof(w1) / sizeof(*w1), u1, 0, 0, 0, "invalid params, dst is not NULL");
    test_wchar2utf8(nullptr, 10, nullc, 0, 0, 0, "invalid params, src length is not 0");

    test_wchar2utf8(wm, sizeof(wm) / sizeof(*wm), um, sizeof(um), 0, sizeof(um),
                    "wm/um mixed languages");
    test_wchar2utf8(wm, sizeof(wm) / sizeof(*wm), um, sizeof(um) - 1, 0, 0, "wm/um boundaries -1");
    test_wchar2utf8(wm, sizeof(wm) / sizeof(*wm), um, sizeof(um) + 1, 0, sizeof(um),
                    "wm/um boundaries +1");
    test_wchar2utf8(wm, sizeof(wm) / sizeof(*wm), nullc, 0, 0, sizeof(um),
                    "wm/um calculate length");
    test_utf82wchar(um, sizeof(um), wm, sizeof(wm) / sizeof(*wm), 0, sizeof(wm) / sizeof(*wm),
                    "um/wm mixed languages");
    test_utf82wchar(um, sizeof(um), wm, sizeof(wm) / sizeof(*wm) + 1, 0, sizeof(wm) / sizeof(*wm),
                    "um/wm boundaries +1");
    test_utf82wchar(um, sizeof(um), nullptr, 0, 0, sizeof(wm) / sizeof(*wm),
                    "um/wm calculate length");

// The following tests won't pass on systems (e.g., Cygwin) where sizeof wchar_t is 2. That's
// due to several reasons but the primary one is that narrowing conversions of literals assigned
// to the wchar_t arrays above don't result in values that will be treated as errors by the
// conversion functions.
#if WCHAR_MAX != 0xffff
    test_utf82wchar(u4, sizeof(u4), w4, sizeof(w4) / sizeof(*w4), 0, sizeof(w4) / sizeof(*w4),
                    "u4/w4 4 octets chars");
    test_wchar2utf8(w4, sizeof(w4) / sizeof(*w4), u4, sizeof(u4), 0, sizeof(u4),
                    "w4/u4 4 octets chars");
    test_wchar2utf8(wb, sizeof(wb) / sizeof(*wb), ub, sizeof(ub), 0, 0, "wb/ub bad chars");
    test_wchar2utf8(wb, sizeof(wb) / sizeof(*wb), ub, sizeof(ub), UTF8_IGNORE_ERROR, sizeof(ub),
                    "wb/ub ignore bad chars");
    test_wchar2utf8(wb, sizeof(wb) / sizeof(*wb), nullc, 0, 0, 0,
                    "wb calculate length of bad chars");
    test_wchar2utf8(wb, sizeof(wb) / sizeof(*wb), nullc, 0, UTF8_IGNORE_ERROR, sizeof(ub),
                    "calculate length, ignore bad chars");
    test_utf82wchar(ub1, sizeof(ub1), wb1, sizeof(wb1) / sizeof(*wb1), UTF8_IGNORE_ERROR,
                    sizeof(wb1) / sizeof(*wb1), "ub1/wb1 ignore bad chars");
    test_utf82wchar(ub1, sizeof(ub1), nullptr, 0, 0, 0, "ub1 calculate length of bad chars");
    test_utf82wchar(ub1, sizeof(ub1), nullptr, 0, UTF8_IGNORE_ERROR, sizeof(wb1) / sizeof(*wb1),
                    "ub1 calculate length, ignore bad chars");
#endif
}

static void test_feature_flags() {
    say(L"Testing future feature flags");
    using ft = features_t;
    ft f;
    do_test(f.test(ft::stderr_nocaret));
    f.set(ft::stderr_nocaret, true);
    do_test(f.test(ft::stderr_nocaret));
    f.set(ft::stderr_nocaret, false);
    do_test(!f.test(ft::stderr_nocaret));

    f.set_from_string(L"stderr-nocaret,nonsense");
    do_test(f.test(ft::stderr_nocaret));
    f.set_from_string(L"stderr-nocaret,no-stderr-nocaret,nonsense");
    do_test(!f.test(ft::stderr_nocaret));

    // Ensure every metadata is represented once.
    size_t counts[ft::flag_count] = {};
    for (const auto &md : ft::metadata) {
        counts[md.flag]++;
    }
    for (size_t c : counts) {
        do_test(c == 1);
    }
    do_test(ft::metadata[ft::stderr_nocaret].name == wcstring(L"stderr-nocaret"));
    do_test(ft::metadata_for(L"stderr-nocaret") == &ft::metadata[ft::stderr_nocaret]);
    do_test(ft::metadata_for(L"not-a-flag") == nullptr);
}

static void test_escape_sequences() {
    say(L"Testing escape_sequences");
    layout_cache_t lc;
    if (lc.escape_code_length(L"") != 0)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"abcd") != 0)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B[2J") != 4)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B[38;5;123mABC") != strlen("\x1B[38;5;123m"))
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B@") != 2)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);

    // iTerm2 escape sequences.
    if (lc.escape_code_length(L"\x1B]50;CurrentDir=test/foo\x07NOT_PART_OF_SEQUENCE") != 25)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B]50;SetMark\x07NOT_PART_OF_SEQUENCE") != 13)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B]6;1;bg;red;brightness;255\x07NOT_PART_OF_SEQUENCE") != 28)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B]Pg4040ff\x1B\\NOT_PART_OF_SEQUENCE") != 12)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B]blahblahblah\x1B\\") != 16)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
    if (lc.escape_code_length(L"\x1B]blahblahblah\x07") != 15)
        err(L"test_escape_sequences failed on line %d\n", __LINE__);
}

class test_lru_t : public lru_cache_t<int> {
   public:
    static constexpr size_t test_capacity = 16;
    using value_type = std::pair<wcstring, int>;

    test_lru_t() : lru_cache_t<int>(test_capacity) {}

    std::vector<value_type> values() const {
        std::vector<value_type> result;
        for (auto p : *this) {
            result.push_back(p);
        }
        return result;
    }

    std::vector<int> ints() const {
        std::vector<int> result;
        for (auto p : *this) {
            result.push_back(p.second);
        }
        return result;
    }
};

static void test_lru() {
    say(L"Testing LRU cache");

    test_lru_t cache;
    std::vector<std::pair<wcstring, int>> expected_evicted;
    std::vector<std::pair<wcstring, int>> expected_values;
    int total_nodes = 20;
    for (int i = 0; i < total_nodes; i++) {
        do_test(cache.size() == size_t(std::min(i, 16)));
        do_test(cache.values() == expected_values);
        if (i < 4) expected_evicted.emplace_back(to_string(i), i);
        // Adding the node the first time should work, and subsequent times should fail.
        do_test(cache.insert(to_string(i), i));
        do_test(!cache.insert(to_string(i), i + 1));

        expected_values.emplace_back(to_string(i), i);
        while (expected_values.size() > test_lru_t::test_capacity) {
            expected_values.erase(expected_values.begin());
        }
        cache.check_sanity();
    }
    do_test(cache.values() == expected_values);
    cache.check_sanity();

    // Stable-sort ints in reverse order
    // This a/2 check ensures that some different ints compare the same
    // It also gives us a different order than we started with
    auto comparer = [](int a, int b) { return a / 2 > b / 2; };
    std::vector<int> ints = cache.ints();
    std::stable_sort(ints.begin(), ints.end(), comparer);

    cache.stable_sort(comparer);
    std::vector<int> new_ints = cache.ints();
    if (new_ints != ints) {
        auto commajoin = [](const std::vector<int> &vs) {
            wcstring ret;
            for (int v : vs) {
                append_format(ret, L"%d,", v);
            }
            if (!ret.empty()) ret.pop_back();
            return ret;
        };
        err(L"LRU stable sort failed. Expected %ls, got %ls\n", commajoin(new_ints).c_str(),
            commajoin(ints).c_str());
    }

    cache.evict_all_nodes();
    do_test(cache.size() == 0);
}

/// An environment built around an std::map.
struct test_environment_t : public environment_t {
    std::map<wcstring, wcstring> vars;

    maybe_t<env_var_t> get(const wcstring &key,
                           env_mode_flags_t mode = ENV_DEFAULT) const override {
        UNUSED(mode);
        auto iter = vars.find(key);
        if (iter != vars.end()) {
            return env_var_t(iter->second, ENV_DEFAULT);
        }
        return none();
    }

    wcstring_list_t get_names(int flags) const override {
        UNUSED(flags);
        wcstring_list_t result;
        for (const auto &kv : vars) {
            result.push_back(kv.first);
        }
        return result;
    }
};

/// A test environment that knows about PWD.
struct pwd_environment_t : public test_environment_t {
    maybe_t<env_var_t> get(const wcstring &key,
                           env_mode_flags_t mode = ENV_DEFAULT) const override {
        if (key == L"PWD") {
            return env_var_t{wgetcwd(), 0};
        }
        return test_environment_t::get(key, mode);
    }

    wcstring_list_t get_names(int flags) const override {
        auto res = test_environment_t::get_names(flags);
        res.clear();
        if (std::count(res.begin(), res.end(), L"PWD") == 0) {
            res.emplace_back(L"PWD");
        }
        return res;
    }
};

/// Perform parameter expansion and test if the output equals the zero-terminated parameter list
/// supplied.
///
/// \param in the string to expand
/// \param flags the flags to send to expand_string
/// \param ... A zero-terminated parameter list of values to test.
/// After the zero terminator comes one more arg, a string, which is the error
/// message to print if the test fails.
static bool expand_test(const wchar_t *in, expand_flags_t flags, ...) {
    completion_list_t output;
    va_list va;
    bool res = true;
    wchar_t *arg;
    parse_error_list_t errors;
    pwd_environment_t pwd{};
    operation_context_t ctx{parser_t::principal_parser().shared(), pwd, no_cancel};

    if (expand_string(in, &output, flags, ctx, &errors) == expand_result_t::error) {
        if (errors.empty()) {
            err(L"Bug: Parse error reported but no error text found.");
        } else {
            err(L"%ls", errors.at(0).describe(in, ctx.parser->is_interactive()).c_str());
        }
        return false;
    }

    wcstring_list_t expected;

    va_start(va, flags);
    while ((arg = va_arg(va, wchar_t *)) != nullptr) {
        expected.emplace_back(arg);
    }
    va_end(va);

    std::set<wcstring> remaining(expected.begin(), expected.end());
    completion_list_t::const_iterator out_it = output.begin(), out_end = output.end();
    for (; out_it != out_end; ++out_it) {
        if (!remaining.erase(out_it->completion)) {
            res = false;
            break;
        }
    }
    if (!remaining.empty()) {
        res = false;
    }

    if (!res) {
        arg = va_arg(va, wchar_t *);
        if (arg) {
            wcstring msg = L"Expected [";
            bool first = true;
            for (const wcstring &exp : expected) {
                if (!first) msg += L", ";
                first = false;
                msg += '"';
                msg += exp;
                msg += '"';
            }
            msg += L"], found [";
            first = true;
            for (const auto &completion : output) {
                if (!first) msg += L", ";
                first = false;
                msg += '"';
                msg += completion.completion;
                msg += '"';
            }
            msg += L"]";
            err(L"%ls\n%ls", arg, msg.c_str());
        }
    }

    va_end(va);

    return res;
}

/// Test globbing and other parameter expansion.
static void test_expand() {
    say(L"Testing parameter expansion");
    const expand_flags_t noflags{};

    expand_test(L"foo", noflags, L"foo", 0, L"Strings do not expand to themselves");
    expand_test(L"a{b,c,d}e", noflags, L"abe", L"ace", L"ade", 0, L"Bracket expansion is broken");
    expand_test(L"a*", expand_flag::skip_wildcards, L"a*", 0, L"Cannot skip wildcard expansion");
    expand_test(L"/bin/l\\0", expand_flag::for_completions, 0,
                L"Failed to handle null escape in expansion");
    expand_test(L"foo\\$bar", expand_flag::skip_variables, L"foo$bar", 0,
                L"Failed to handle dollar sign in variable-skipping expansion");

    // bb
    //    x
    // bar
    // baz
    //    xxx
    //    yyy
    // bax
    //    xxx
    // lol
    //    nub
    //       q
    // .foo
    // aaa
    // aaa2
    //    x
    if (system("mkdir -p test/fish_expand_test/")) err(L"mkdir failed");
    if (system("mkdir -p test/fish_expand_test/bb/")) err(L"mkdir failed");
    if (system("mkdir -p test/fish_expand_test/baz/")) err(L"mkdir failed");
    if (system("mkdir -p test/fish_expand_test/bax/")) err(L"mkdir failed");
    if (system("mkdir -p test/fish_expand_test/lol/nub/")) err(L"mkdir failed");
    if (system("mkdir -p test/fish_expand_test/aaa/")) err(L"mkdir failed");
    if (system("mkdir -p test/fish_expand_test/aaa2/")) err(L"mkdir failed");
    if (system("touch test/fish_expand_test/.foo")) err(L"touch failed");
    if (system("touch test/fish_expand_test/bb/x")) err(L"touch failed");
    if (system("touch test/fish_expand_test/bar")) err(L"touch failed");
    if (system("touch test/fish_expand_test/bax/xxx")) err(L"touch failed");
    if (system("touch test/fish_expand_test/baz/xxx")) err(L"touch failed");
    if (system("touch test/fish_expand_test/baz/yyy")) err(L"touch failed");
    if (system("touch test/fish_expand_test/lol/nub/q")) err(L"touch failed");
    if (system("touch test/fish_expand_test/aaa2/x")) err(L"touch failed");

    // This is checking that .* does NOT match . and ..
    // (https://github.com/fish-shell/fish-shell/issues/270). But it does have to match literal
    // components (e.g. "./*" has to match the same as "*".
    const wchar_t *const wnull = nullptr;
    expand_test(L"test/fish_expand_test/.*", noflags, L"test/fish_expand_test/.foo", wnull,
                L"Expansion not correctly handling dotfiles");

    expand_test(L"test/fish_expand_test/./.*", noflags, L"test/fish_expand_test/./.foo", wnull,
                L"Expansion not correctly handling literal path components in dotfiles");

    expand_test(L"test/fish_expand_test/*/xxx", noflags, L"test/fish_expand_test/bax/xxx",
                L"test/fish_expand_test/baz/xxx", wnull, L"Glob did the wrong thing 1");

    expand_test(L"test/fish_expand_test/*z/xxx", noflags, L"test/fish_expand_test/baz/xxx", wnull,
                L"Glob did the wrong thing 2");

    expand_test(L"test/fish_expand_test/**z/xxx", noflags, L"test/fish_expand_test/baz/xxx", wnull,
                L"Glob did the wrong thing 3");

    expand_test(L"test/fish_expand_test////baz/xxx", noflags, L"test/fish_expand_test////baz/xxx",
                wnull, L"Glob did the wrong thing 3");

    expand_test(L"test/fish_expand_test/b**", noflags, L"test/fish_expand_test/bb",
                L"test/fish_expand_test/bb/x", L"test/fish_expand_test/bar",
                L"test/fish_expand_test/bax", L"test/fish_expand_test/bax/xxx",
                L"test/fish_expand_test/baz", L"test/fish_expand_test/baz/xxx",
                L"test/fish_expand_test/baz/yyy", wnull, L"Glob did the wrong thing 4");

    // A trailing slash should only produce directories.
    expand_test(L"test/fish_expand_test/b*/", noflags, L"test/fish_expand_test/bb/",
                L"test/fish_expand_test/baz/", L"test/fish_expand_test/bax/", wnull,
                L"Glob did the wrong thing 5");

    expand_test(L"test/fish_expand_test/b**/", noflags, L"test/fish_expand_test/bb/",
                L"test/fish_expand_test/baz/", L"test/fish_expand_test/bax/", wnull,
                L"Glob did the wrong thing 6");

    expand_test(L"test/fish_expand_test/**/q", noflags, L"test/fish_expand_test/lol/nub/q", wnull,
                L"Glob did the wrong thing 7");

    expand_test(L"test/fish_expand_test/BA", expand_flag::for_completions,
                L"test/fish_expand_test/bar", L"test/fish_expand_test/bax/",
                L"test/fish_expand_test/baz/", wnull, L"Case insensitive test did the wrong thing");

    expand_test(L"test/fish_expand_test/BA", expand_flag::for_completions,
                L"test/fish_expand_test/bar", L"test/fish_expand_test/bax/",
                L"test/fish_expand_test/baz/", wnull, L"Case insensitive test did the wrong thing");

    expand_test(L"test/fish_expand_test/bb/yyy", expand_flag::for_completions,
                /* nothing! */ wnull, L"Wrong fuzzy matching 1");

    expand_test(L"test/fish_expand_test/bb/x",
                expand_flags_t{expand_flag::for_completions, expand_flag::fuzzy_match}, L"",
                wnull,  // we just expect the empty string since this is an exact match
                L"Wrong fuzzy matching 2");

    // Some vswprintfs refuse to append ANY_STRING in a format specifiers, so don't use
    // format_string here.
    const expand_flags_t fuzzy_comp{expand_flag::for_completions, expand_flag::fuzzy_match};
    const wcstring any_str_str(1, ANY_STRING);
    expand_test(L"test/fish_expand_test/b/xx*", fuzzy_comp,
                (L"test/fish_expand_test/bax/xx" + any_str_str).c_str(),
                (L"test/fish_expand_test/baz/xx" + any_str_str).c_str(), wnull,
                L"Wrong fuzzy matching 3");

    expand_test(L"test/fish_expand_test/b/yyy", fuzzy_comp, L"test/fish_expand_test/baz/yyy", wnull,
                L"Wrong fuzzy matching 4");

    expand_test(L"test/fish_expand_test/aa/x", fuzzy_comp, L"test/fish_expand_test/aaa2/x", wnull,
                L"Wrong fuzzy matching 5");

    expand_test(L"test/fish_expand_test/aaa/x", fuzzy_comp, wnull,
                L"Wrong fuzzy matching 6 - shouldn't remove valid directory names (#3211)");

    if (!expand_test(L"test/fish_expand_test/.*", noflags, L"test/fish_expand_test/.foo", 0)) {
        err(L"Expansion not correctly handling dotfiles");
    }
    if (!expand_test(L"test/fish_expand_test/./.*", noflags, L"test/fish_expand_test/./.foo", 0)) {
        err(L"Expansion not correctly handling literal path components in dotfiles");
    }

    if (!pushd("test/fish_expand_test")) return;

    expand_test(L"b/xx", fuzzy_comp, L"bax/xxx", L"baz/xxx", wnull, L"Wrong fuzzy matching 5");

    // multiple slashes with fuzzy matching - #3185
    expand_test(L"l///n", fuzzy_comp, L"lol///nub/", wnull, L"Wrong fuzzy matching 6");

    popd();
}

static void test_expand_overflow() {
    say(L"Testing overflowing expansions");
    // Ensure that we have sane limits on number of expansions - see #7497.

    // Make a list of 64 elements, then expand it cartesian-style 64 times.
    // This is far too large to expand.
    wcstring_list_t vals;
    wcstring expansion;
    for (int i = 1; i <= 64; i++) {
        vals.push_back(to_string(i));
        expansion.append(L"$bigvar");
    }

    auto parser = parser_t::principal_parser().shared();
    parser->vars().push(true);
    int set = parser->vars().set(L"bigvar", ENV_LOCAL, std::move(vals));
    do_test(set == ENV_OK);

    parse_error_list_t errors;
    operation_context_t ctx{parser, parser->vars(), no_cancel};

    // We accept only 1024 completions.
    completion_receiver_t output{1024};

    auto res = expand_string(expansion, &output, expand_flags_t{}, ctx, &errors);
    do_test(!errors.empty());
    do_test(res == expand_result_t::error);

    parser->vars().pop();
}

static void test_fuzzy_match() {
    say(L"Testing fuzzy string matching");
    // Check that a string fuzzy match has the expected type and case folding.
    using type_t = string_fuzzy_match_t::contain_type_t;
    using case_fold_t = string_fuzzy_match_t::case_fold_t;
    auto test_fuzzy = [](const wchar_t *inp, const wchar_t *exp, type_t type,
                         case_fold_t fold) -> bool {
        auto m = string_fuzzy_match_string(inp, exp);
        return m && m->type == type && m->case_fold == fold;
    };

    do_test(test_fuzzy(L"", L"", type_t::exact, case_fold_t::samecase));
    do_test(test_fuzzy(L"alpha", L"alpha", type_t::exact, case_fold_t::samecase));
    do_test(test_fuzzy(L"alp", L"alpha", type_t::prefix, case_fold_t::samecase));
    do_test(test_fuzzy(L"alpha", L"AlPhA", type_t::exact, case_fold_t::smartcase));
    do_test(test_fuzzy(L"alpha", L"AlPhA!", type_t::prefix, case_fold_t::smartcase));
    do_test(test_fuzzy(L"ALPHA", L"alpha!", type_t::prefix, case_fold_t::icase));
    do_test(test_fuzzy(L"ALPHA!", L"alPhA!", type_t::exact, case_fold_t::icase));
    do_test(test_fuzzy(L"alPh", L"ALPHA!", type_t::prefix, case_fold_t::icase));
    do_test(test_fuzzy(L"LPH", L"ALPHA!", type_t::substr, case_fold_t::samecase));
    do_test(test_fuzzy(L"lph", L"AlPhA!", type_t::substr, case_fold_t::smartcase));
    do_test(test_fuzzy(L"lPh", L"ALPHA!", type_t::substr, case_fold_t::icase));
    do_test(test_fuzzy(L"AA", L"ALPHA!", type_t::subseq, case_fold_t::samecase));
    do_test(!string_fuzzy_match_string(L"lh", L"ALPHA!").has_value());  // no subseq icase
    do_test(!string_fuzzy_match_string(L"BB", L"ALPHA!").has_value());
}

static void test_ifind() {
    say(L"Testing ifind");
    do_test(ifind(std::string{"alpha"}, std::string{"alpha"}) == 0);
    do_test(ifind(wcstring{L"alphab"}, wcstring{L"alpha"}) == 0);
    do_test(ifind(std::string{"alpha"}, std::string{"balpha"}) == std::string::npos);
    do_test(ifind(std::string{"balpha"}, std::string{"alpha"}) == 1);
    do_test(ifind(std::string{"alphab"}, std::string{"balpha"}) == std::string::npos);
    do_test(ifind(std::string{"balpha"}, std::string{"lPh"}) == 2);
    do_test(ifind(std::string{"balpha"}, std::string{"Plh"}) == std::string::npos);
}

static void test_ifind_fuzzy() {
    say(L"Testing ifind with fuzzy logic");
    do_test(ifind(std::string{"alpha"}, std::string{"alpha"}, true) == 0);
    do_test(ifind(wcstring{L"alphab"}, wcstring{L"alpha"}, true) == 0);
    do_test(ifind(std::string{"alpha-b"}, std::string{"alpha_b"}, true) == 0);
    do_test(ifind(std::string{"alpha-_"}, std::string{"alpha_-"}, true) == 0);
    do_test(ifind(std::string{"alpha-b"}, std::string{"alpha b"}, true) == std::string::npos);
}

static void test_abbreviations() {
    say(L"Testing abbreviations");
    auto &vars = parser_t::principal_parser().vars();
    vars.push(true);

    const std::vector<std::pair<const wcstring, const wcstring>> abbreviations = {
        {L"gc", L"git checkout"},
        {L"foo", L"bar"},
        {L"gx", L"git checkout"},
    };
    for (const auto &kv : abbreviations) {
        int ret = vars.set_one(L"_fish_abbr_" + kv.first, ENV_LOCAL, kv.second);
        if (ret != 0) err(L"Unable to set abbreviation variable");
    }

    if (expand_abbreviation(L"", vars)) err(L"Unexpected success with empty abbreviation");
    if (expand_abbreviation(L"nothing", vars)) err(L"Unexpected success with missing abbreviation");

    auto mresult = expand_abbreviation(L"gc", vars);
    if (!mresult) err(L"Unexpected failure with gc abbreviation");
    if (*mresult != L"git checkout") err(L"Wrong abbreviation result for gc");

    mresult = expand_abbreviation(L"foo", vars);
    if (!mresult) err(L"Unexpected failure with foo abbreviation");
    if (*mresult != L"bar") err(L"Wrong abbreviation result for foo");

    maybe_t<wcstring> result;
    auto expand_abbreviation_in_command = [](const wcstring &cmdline, size_t cursor_pos,
                                             const environment_t &vars) -> maybe_t<wcstring> {
        if (auto edit = reader_expand_abbreviation_in_command(cmdline, cursor_pos, vars)) {
            wcstring cmdline_expanded = cmdline;
            apply_edit(&cmdline_expanded, *edit);
            return cmdline_expanded;
        }
        return none_t();
    };
    result = expand_abbreviation_in_command(L"just a command", 3, vars);
    if (result) err(L"Command wrongly expanded on line %ld", (long)__LINE__);
    result = expand_abbreviation_in_command(L"gc somebranch", 0, vars);
    if (!result) err(L"Command not expanded on line %ld", (long)__LINE__);

    result = expand_abbreviation_in_command(L"gc somebranch", const_strlen(L"gc"), vars);
    if (!result) err(L"gc not expanded");
    if (result != L"git checkout somebranch")
        err(L"gc incorrectly expanded on line %ld to '%ls'", (long)__LINE__, result->c_str());

    // Space separation.
    result = expand_abbreviation_in_command(L"gx somebranch", const_strlen(L"gc"), vars);
    if (!result) err(L"gx not expanded");
    if (result != L"git checkout somebranch")
        err(L"gc incorrectly expanded on line %ld to '%ls'", (long)__LINE__, result->c_str());

    result = expand_abbreviation_in_command(L"echo hi ; gc somebranch",
                                            const_strlen(L"echo hi ; g"), vars);
    if (!result) err(L"gc not expanded on line %ld", (long)__LINE__);
    if (result != L"echo hi ; git checkout somebranch")
        err(L"gc incorrectly expanded on line %ld", (long)__LINE__);

    result = expand_abbreviation_in_command(L"echo (echo (echo (echo (gc ",
                                            const_strlen(L"echo (echo (echo (echo (gc"), vars);
    if (!result) err(L"gc not expanded on line %ld", (long)__LINE__);
    if (result != L"echo (echo (echo (echo (git checkout ")
        err(L"gc incorrectly expanded on line %ld to '%ls'", (long)__LINE__, result->c_str());

    // If commands should be expanded.
    result = expand_abbreviation_in_command(L"if gc", const_strlen(L"if gc"), vars);
    if (!result) err(L"gc not expanded on line %ld", (long)__LINE__);
    if (result != L"if git checkout")
        err(L"gc incorrectly expanded on line %ld to '%ls'", (long)__LINE__, result->c_str());

    // Others should not be.
    result = expand_abbreviation_in_command(L"of gc", const_strlen(L"of gc"), vars);
    if (result) err(L"gc incorrectly expanded on line %ld", (long)__LINE__);

    // Others should not be.
    result = expand_abbreviation_in_command(L"command gc", const_strlen(L"command gc"), vars);
    if (result) err(L"gc incorrectly expanded on line %ld", (long)__LINE__);

    vars.pop();
}

/// Test path functions.
static void test_path() {
    say(L"Testing path functions");

    wcstring path = L"//foo//////bar/";
    path_make_canonical(path);
    if (path != L"/foo/bar") {
        err(L"Bug in canonical PATH code");
    }

    path = L"/";
    path_make_canonical(path);
    if (path != L"/") {
        err(L"Bug in canonical PATH code");
    }

    if (paths_are_equivalent(L"/foo/bar/baz", L"foo/bar/baz"))
        err(L"Bug in canonical PATH code on line %ld", (long)__LINE__);
    if (!paths_are_equivalent(L"///foo///bar/baz", L"/foo/bar////baz//"))
        err(L"Bug in canonical PATH code on line %ld", (long)__LINE__);
    if (!paths_are_equivalent(L"/foo/bar/baz", L"/foo/bar/baz"))
        err(L"Bug in canonical PATH code on line %ld", (long)__LINE__);
    if (!paths_are_equivalent(L"/", L"/"))
        err(L"Bug in canonical PATH code on line %ld", (long)__LINE__);

    do_test(path_apply_working_directory(L"abc", L"/def/") == L"/def/abc");
    do_test(path_apply_working_directory(L"abc/", L"/def/") == L"/def/abc/");
    do_test(path_apply_working_directory(L"/abc/", L"/def/") == L"/abc/");
    do_test(path_apply_working_directory(L"/abc", L"/def/") == L"/abc");
    do_test(path_apply_working_directory(L"", L"/def/").empty());
    do_test(path_apply_working_directory(L"abc", L"") == L"abc");
}

static void test_pager_navigation() {
    say(L"Testing pager navigation");

    // Generate 19 strings of width 10. There's 2 spaces between completions, and our term size is
    // 80; these can therefore fit into 6 columns (6 * 12 - 2 = 70) or 5 columns (58) but not 7
    // columns (7 * 12 - 2 = 82).
    //
    // You can simulate this test by creating 19 files named "file00.txt" through "file_18.txt".
    completion_list_t completions;
    for (size_t i = 0; i < 19; i++) {
        append_completion(&completions, L"abcdefghij");
    }

    pager_t pager;
    pager.set_completions(completions);
    pager.set_term_size(termsize_t::defaults());
    page_rendering_t render = pager.render();

    if (render.term_width != 80) err(L"Wrong term width");
    if (render.term_height != 24) err(L"Wrong term height");

    size_t rows = 4, cols = 5;

    // We have 19 completions. We can fit into 6 columns with 4 rows or 5 columns with 4 rows; the
    // second one is better and so is what we ought to have picked.
    if (render.rows != rows) err(L"Wrong row count");
    if (render.cols != cols) err(L"Wrong column count");

    // Initially expect to have no completion index.
    if (render.selected_completion_idx != (size_t)(-1)) {
        err(L"Wrong initial selection");
    }

    // Here are navigation directions and where we expect the selection to be.
    const struct {
        selection_motion_t dir;
        size_t sel;
    } cmds[] = {
        // Tab completion to get into the list.
        {selection_motion_t::next, 0},

        // Westward motion in upper left goes to the last filled column in the last row.
        {selection_motion_t::west, 15},
        // East goes back.
        {selection_motion_t::east, 0},

        // "Next" motion goes down the column.
        {selection_motion_t::next, 1},
        {selection_motion_t::next, 2},

        {selection_motion_t::west, 17},
        {selection_motion_t::east, 2},
        {selection_motion_t::east, 6},
        {selection_motion_t::east, 10},
        {selection_motion_t::east, 14},
        {selection_motion_t::east, 18},

        {selection_motion_t::west, 14},
        {selection_motion_t::east, 18},

        // Eastward motion wraps back to the upper left, westward goes to the prior column.
        {selection_motion_t::east, 3},
        {selection_motion_t::east, 7},
        {selection_motion_t::east, 11},
        {selection_motion_t::east, 15},

        // Pages.
        {selection_motion_t::page_north, 12},
        {selection_motion_t::page_south, 15},
        {selection_motion_t::page_north, 12},
        {selection_motion_t::east, 16},
        {selection_motion_t::page_south, 18},
        {selection_motion_t::east, 3},
        {selection_motion_t::north, 2},
        {selection_motion_t::page_north, 0},
        {selection_motion_t::page_south, 3},

    };
    for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++) {
        pager.select_next_completion_in_direction(cmds[i].dir, render);
        pager.update_rendering(&render);
        if (cmds[i].sel != render.selected_completion_idx) {
            err(L"For command %lu, expected selection %lu, but found instead %lu\n", i, cmds[i].sel,
                render.selected_completion_idx);
        }
    }
}

struct pager_layout_testcase_t {
    int width;
    const wchar_t *expected;

    // Run ourselves as a test case.
    // Set our data on the pager, and then check the rendering.
    // We should have one line, and it should have our expected text.
    void run(pager_t &pager) const {
        pager.set_term_size(termsize_t{this->width, 24});
        page_rendering_t rendering = pager.render();
        const screen_data_t &sd = rendering.screen_data;
        do_test(sd.line_count() == 1);
        if (sd.line_count() > 0) {
            wcstring expected = this->expected;

            // hack: handle the case where ellipsis is not L'\x2026'
            wchar_t ellipsis_char = get_ellipsis_char();
            if (ellipsis_char != L'\x2026') {
                std::replace(expected.begin(), expected.end(), L'\x2026', ellipsis_char);
            }

            wcstring text;
            for (const auto &p : sd.line(0).text) {
                text.push_back(p.character);
            }
            if (text != expected) {
                std::fwprintf(stderr, L"width %d got %zu<%ls>, expected %zu<%ls>\n", this->width,
                              text.length(), text.c_str(), expected.length(), expected.c_str());
                for (size_t i = 0; i < std::max(text.length(), expected.length()); i++) {
                    std::fwprintf(stderr, L"i %zu got <%lx> expected <%lx>\n", i,
                                  i >= text.length() ? 0xffff : text[i],
                                  i >= expected.length() ? 0xffff : expected[i]);
                }
            }
            do_test(text == expected);
        }
    }
};

static void test_pager_layout() {
    // These tests are woefully incomplete
    // They only test the truncation logic for a single completion
    say(L"Testing pager layout");
    pager_t pager;

    // These test cases have equal completions and descriptions
    const completion_t c1(L"abcdefghij", L"1234567890");
    pager.set_completions(completion_list_t(1, c1));
    const pager_layout_testcase_t testcases1[] = {
        {26, L"abcdefghij  (1234567890)"}, {25, L"abcdefghij  (1234567890)"},
        {24, L"abcdefghij  (1234567890)"}, {23, L"abcdefghij  (12345678…)"},
        {22, L"abcdefghij  (1234567…)"},   {21, L"abcdefghij  (123456…)"},
        {20, L"abcdefghij  (12345…)"},     {19, L"abcdefghij  (1234…)"},
        {18, L"abcdefgh…  (1234…)"},       {17, L"abcdefg…  (1234…)"},
        {16, L"abcdefg…  (123…)"},         {0, nullptr}  // sentinel terminator
    };
    for (size_t i = 0; testcases1[i].expected != nullptr; i++) {
        testcases1[i].run(pager);
    }

    // These test cases have heavyweight completions
    const completion_t c2(L"abcdefghijklmnopqrs", L"1");
    pager.set_completions(completion_list_t(1, c2));
    const pager_layout_testcase_t testcases2[] = {
        {26, L"abcdefghijklmnopqrs  (1)"}, {25, L"abcdefghijklmnopqrs  (1)"},
        {24, L"abcdefghijklmnopqrs  (1)"}, {23, L"abcdefghijklmnopq…  (1)"},
        {22, L"abcdefghijklmnop…  (1)"},   {21, L"abcdefghijklmno…  (1)"},
        {20, L"abcdefghijklmn…  (1)"},     {19, L"abcdefghijklm…  (1)"},
        {18, L"abcdefghijkl…  (1)"},       {17, L"abcdefghijk…  (1)"},
        {16, L"abcdefghij…  (1)"},         {0, nullptr}  // sentinel terminator
    };
    for (size_t i = 0; testcases2[i].expected != nullptr; i++) {
        testcases2[i].run(pager);
    }

    // These test cases have no descriptions
    const completion_t c3(L"abcdefghijklmnopqrst", L"");
    pager.set_completions(completion_list_t(1, c3));
    const pager_layout_testcase_t testcases3[] = {
        {26, L"abcdefghijklmnopqrst"}, {25, L"abcdefghijklmnopqrst"},
        {24, L"abcdefghijklmnopqrst"}, {23, L"abcdefghijklmnopqrst"},
        {22, L"abcdefghijklmnopqrst"}, {21, L"abcdefghijklmnopqrst"},
        {20, L"abcdefghijklmnopqrst"}, {19, L"abcdefghijklmnopqr…"},
        {18, L"abcdefghijklmnopq…"},   {17, L"abcdefghijklmnop…"},
        {16, L"abcdefghijklmno…"},     {0, nullptr}  // sentinel terminator
    };
    for (size_t i = 0; testcases3[i].expected != nullptr; i++) {
        testcases3[i].run(pager);
    }
}

enum word_motion_t { word_motion_left, word_motion_right };
static void test_1_word_motion(word_motion_t motion, move_word_style_t style,
                               const wcstring &test) {
    wcstring command;
    std::set<size_t> stops;

    // Carets represent stops and should be cut out of the command.
    for (wchar_t wc : test) {
        if (wc == L'^') {
            stops.insert(command.size());
        } else {
            command.push_back(wc);
        }
    }

    size_t idx, end;
    if (motion == word_motion_left) {
        idx = *std::max_element(stops.begin(), stops.end());
        end = 0;
    } else {
        idx = *std::min_element(stops.begin(), stops.end());
        end = command.size();
    }
    stops.erase(idx);

    move_word_state_machine_t sm(style);
    while (idx != end) {
        size_t char_idx = (motion == word_motion_left ? idx - 1 : idx);
        wchar_t wc = command.at(char_idx);
        bool will_stop = !sm.consume_char(wc);
        // std::fwprintf(stdout, L"idx %lu, looking at %lu (%c): %d\n", idx, char_idx, (char)wc,
        //          will_stop);
        bool expected_stop = (stops.count(idx) > 0);
        if (will_stop != expected_stop) {
            wcstring tmp = command;
            tmp.insert(idx, L"^");
            const char *dir = (motion == word_motion_left ? "left" : "right");
            if (will_stop) {
                err(L"Word motion: moving %s, unexpected stop at idx %lu: '%ls'", dir, idx,
                    tmp.c_str());
            } else if (!will_stop && expected_stop) {
                err(L"Word motion: moving %s, should have stopped at idx %lu: '%ls'", dir, idx,
                    tmp.c_str());
            }
        }
        // We don't expect to stop here next time.
        if (expected_stop) {
            stops.erase(idx);
        }
        if (will_stop) {
            sm.reset();
        } else {
            idx += (motion == word_motion_left ? -1 : 1);
        }
    }
}

/// Test word motion (forward-word, etc.). Carets represent cursor stops.
static void test_word_motion() {
    say(L"Testing word motion");
    test_1_word_motion(word_motion_left, move_word_style_punctuation, L"^echo ^hello_^world.^txt^");
    test_1_word_motion(word_motion_right, move_word_style_punctuation,
                       L"^echo^ hello^_world^.txt^");

    test_1_word_motion(word_motion_left, move_word_style_punctuation,
                       L"echo ^foo_^foo_^foo/^/^/^/^/^    ^");
    test_1_word_motion(word_motion_right, move_word_style_punctuation,
                       L"^echo^ foo^_foo^_foo^/^/^/^/^/    ^");

    test_1_word_motion(word_motion_left, move_word_style_path_components, L"^/^foo/^bar/^baz/^");
    test_1_word_motion(word_motion_left, move_word_style_path_components, L"^echo ^--foo ^--bar^");
    test_1_word_motion(word_motion_left, move_word_style_path_components,
                       L"^echo ^hi ^> ^/^dev/^null^");

    test_1_word_motion(word_motion_left, move_word_style_path_components,
                       L"^echo ^/^foo/^bar{^aaa,^bbb,^ccc}^bak/^");
    test_1_word_motion(word_motion_left, move_word_style_path_components, L"^echo ^bak ^///^");
    test_1_word_motion(word_motion_left, move_word_style_path_components, L"^aaa ^@ ^@^aaa^");
    test_1_word_motion(word_motion_left, move_word_style_path_components, L"^aaa ^a ^@^aaa^");
    test_1_word_motion(word_motion_left, move_word_style_path_components, L"^aaa ^@@@ ^@@^aa^");
    test_1_word_motion(word_motion_left, move_word_style_path_components, L"^aa^@@  ^aa@@^a^");

    test_1_word_motion(word_motion_right, move_word_style_punctuation, L"^a^ bcd^");
    test_1_word_motion(word_motion_right, move_word_style_punctuation, L"a^b^ cde^");
    test_1_word_motion(word_motion_right, move_word_style_punctuation, L"^ab^ cde^");
    test_1_word_motion(word_motion_right, move_word_style_punctuation, L"^ab^&cd^ ^& ^e^ f^&");

    test_1_word_motion(word_motion_right, move_word_style_whitespace, L"^^a-b-c^ d-e-f");
    test_1_word_motion(word_motion_right, move_word_style_whitespace, L"^a-b-c^\n d-e-f^ ");
    test_1_word_motion(word_motion_right, move_word_style_whitespace, L"^a-b-c^\n\nd-e-f^ ");
}

/// Test is_potential_path.
static void test_is_potential_path() {
    say(L"Testing is_potential_path");

    // Directories
    if (system("mkdir -p test/is_potential_path_test/alpha/")) err(L"mkdir failed");
    if (system("mkdir -p test/is_potential_path_test/beta/")) err(L"mkdir failed");

    // Files
    if (system("touch test/is_potential_path_test/aardvark")) err(L"touch failed");
    if (system("touch test/is_potential_path_test/gamma")) err(L"touch failed");

    const wcstring wd = L"test/is_potential_path_test/";
    const wcstring_list_t wds({L".", wd});

    operation_context_t ctx{env_stack_t::principal()};
    do_test(is_potential_path(L"al", wds, ctx, PATH_REQUIRE_DIR));
    do_test(is_potential_path(L"alpha/", wds, ctx, PATH_REQUIRE_DIR));
    do_test(is_potential_path(L"aard", wds, ctx, 0));

    do_test(!is_potential_path(L"balpha/", wds, ctx, PATH_REQUIRE_DIR));
    do_test(!is_potential_path(L"aard", wds, ctx, PATH_REQUIRE_DIR));
    do_test(!is_potential_path(L"aarde", wds, ctx, PATH_REQUIRE_DIR));
    do_test(!is_potential_path(L"aarde", wds, ctx, 0));

    do_test(is_potential_path(L"test/is_potential_path_test/aardvark", wds, ctx, 0));
    do_test(is_potential_path(L"test/is_potential_path_test/al", wds, ctx, PATH_REQUIRE_DIR));
    do_test(is_potential_path(L"test/is_potential_path_test/aardv", wds, ctx, 0));

    do_test(
        !is_potential_path(L"test/is_potential_path_test/aardvark", wds, ctx, PATH_REQUIRE_DIR));
    do_test(!is_potential_path(L"test/is_potential_path_test/al/", wds, ctx, 0));
    do_test(!is_potential_path(L"test/is_potential_path_test/ar", wds, ctx, 0));

    do_test(is_potential_path(L"/usr", wds, ctx, PATH_REQUIRE_DIR));
}

/// Test the 'test' builtin.
maybe_t<int> builtin_test(parser_t &parser, io_streams_t &streams, const wchar_t **argv);
static bool run_one_test_test(int expected, const wcstring_list_t &lst, bool bracket) {
    parser_t &parser = parser_t::principal_parser();
    wcstring_list_t argv;
    argv.push_back(bracket ? L"[" : L"test");
    argv.insert(argv.end(), lst.begin(), lst.end());
    if (bracket) argv.push_back(L"]");

    null_terminated_array_t<wchar_t> cargv(argv);

    null_output_stream_t null{};
    io_streams_t streams(null, null);
    maybe_t<int> result = builtin_test(parser, streams, cargv.get());

    if (result != expected) {
        std::wstring got = result ? std::to_wstring(result.value()) : L"nothing";
        err(L"expected builtin_test() to return %d, got %s", expected, got.c_str());
    }
    return result == expected;
}

static bool run_test_test(int expected, const wcstring &str) {
    // We need to tokenize the string in the same manner a normal shell would do. This is because we
    // need to test things like quoted strings that have leading and trailing whitespace.
    auto parser = parser_t::principal_parser().shared();
    null_environment_t nullenv{};
    operation_context_t ctx{parser, nullenv, no_cancel};
    completion_list_t comps = parser_t::expand_argument_list(str, expand_flags_t{}, ctx);

    wcstring_list_t argv;
    for (const auto &c : comps) {
        argv.push_back(c.completion);
    }

    bool bracket = run_one_test_test(expected, argv, true);
    bool nonbracket = run_one_test_test(expected, argv, false);
    do_test(bracket == nonbracket);
    return nonbracket;
}

static void test_test_brackets() {
    // Ensure [ knows it needs a ].
    parser_t &parser = parser_t::principal_parser();
    null_output_stream_t null{};
    io_streams_t streams(null, null);

    wcstring_list_t args;

    const wchar_t *args1[] = {L"[", L"foo", nullptr};
    do_test(builtin_test(parser, streams, args1) != 0);

    const wchar_t *args2[] = {L"[", L"foo", L"]", nullptr};
    do_test(builtin_test(parser, streams, args2) == 0);

    const wchar_t *args3[] = {L"[", L"foo", L"]", L"bar", nullptr};
    do_test(builtin_test(parser, streams, args3) != 0);
}

static void test_test() {
    say(L"Testing test builtin");
    test_test_brackets();

    do_test(run_test_test(0, L"5 -ne 6"));
    do_test(run_test_test(0, L"5 -eq 5"));
    do_test(run_test_test(0, L"0 -eq 0"));
    do_test(run_test_test(0, L"-1 -eq -1"));
    do_test(run_test_test(0, L"1 -ne -1"));
    do_test(run_test_test(1, L"' 2 ' -ne 2"));
    do_test(run_test_test(0, L"' 2' -eq 2"));
    do_test(run_test_test(0, L"'2 ' -eq 2"));
    do_test(run_test_test(0, L"' 2 ' -eq 2"));
    do_test(run_test_test(2, L"' 2x' -eq 2"));
    do_test(run_test_test(2, L"'' -eq 0"));
    do_test(run_test_test(2, L"'' -ne 0"));
    do_test(run_test_test(2, L"'  ' -eq 0"));
    do_test(run_test_test(2, L"'  ' -ne 0"));
    do_test(run_test_test(2, L"'x' -eq 0"));
    do_test(run_test_test(2, L"'x' -ne 0"));
    do_test(run_test_test(1, L"-1 -ne -1"));
    do_test(run_test_test(0, L"abc != def"));
    do_test(run_test_test(1, L"abc = def"));
    do_test(run_test_test(0, L"5 -le 10"));
    do_test(run_test_test(0, L"10 -le 10"));
    do_test(run_test_test(1, L"20 -le 10"));
    do_test(run_test_test(0, L"-1 -le 0"));
    do_test(run_test_test(1, L"0 -le -1"));
    do_test(run_test_test(0, L"15 -ge 10"));
    do_test(run_test_test(0, L"15 -ge 10"));
    do_test(run_test_test(1, L"! 15 -ge 10"));
    do_test(run_test_test(0, L"! ! 15 -ge 10"));

    do_test(run_test_test(0, L"0 -ne 1 -a 0 -eq 0"));
    do_test(run_test_test(0, L"0 -ne 1 -a -n 5"));
    do_test(run_test_test(0, L"-n 5 -a 10 -gt 5"));
    do_test(run_test_test(0, L"-n 3 -a -n 5"));

    // Test precedence:
    //      '0 == 0 || 0 == 1 && 0 == 2'
    //  should be evaluated as:
    //      '0 == 0 || (0 == 1 && 0 == 2)'
    //  and therefore true. If it were
    //      '(0 == 0 || 0 == 1) && 0 == 2'
    //  it would be false.
    do_test(run_test_test(0, L"0 = 0 -o 0 = 1 -a 0 = 2"));
    do_test(run_test_test(0, L"-n 5 -o 0 = 1 -a 0 = 2"));
    do_test(run_test_test(1, L"\\( 0 = 0 -o  0 = 1 \\) -a 0 = 2"));
    do_test(run_test_test(0, L"0 = 0 -o \\( 0 = 1 -a 0 = 2 \\)"));

    // A few lame tests for permissions; these need to be a lot more complete.
    do_test(run_test_test(0, L"-e /bin/ls"));
    do_test(run_test_test(1, L"-e /bin/ls_not_a_path"));
    do_test(run_test_test(0, L"-x /bin/ls"));
    do_test(run_test_test(1, L"-x /bin/ls_not_a_path"));
    do_test(run_test_test(0, L"-d /bin/"));
    do_test(run_test_test(1, L"-d /bin/ls"));

    // This failed at one point.
    do_test(run_test_test(1, L"-d /bin -a 5 -eq 3"));
    do_test(run_test_test(0, L"-d /bin -o 5 -eq 3"));
    do_test(run_test_test(0, L"-d /bin -a ! 5 -eq 3"));

    // We didn't properly handle multiple "just strings" either.
    do_test(run_test_test(0, L"foo"));
    do_test(run_test_test(0, L"foo -a bar"));

    // These should be errors.
    do_test(run_test_test(1, L"foo bar"));
    do_test(run_test_test(1, L"foo bar baz"));

    // This crashed.
    do_test(run_test_test(1, L"1 = 1 -a = 1"));

    // Make sure we can treat -S as a parameter instead of an operator.
    // https://github.com/fish-shell/fish-shell/issues/601
    do_test(run_test_test(0, L"-S = -S"));
    do_test(run_test_test(1, L"! ! ! A"));

    // Verify that 1. doubles are treated as doubles, and 2. integers that cannot be represented as
    // doubles are still treated as integers.
    do_test(run_test_test(0, L"4611686018427387904 -eq 4611686018427387904"));
    do_test(run_test_test(0, L"4611686018427387904.0 -eq 4611686018427387904.0"));
    do_test(run_test_test(0, L"4611686018427387904.00000000000000001 -eq 4611686018427387904.0"));
    do_test(run_test_test(1, L"4611686018427387904 -eq 4611686018427387905"));
    do_test(run_test_test(0, L"-4611686018427387904 -ne 4611686018427387904"));
    do_test(run_test_test(0, L"-4611686018427387904 -le 4611686018427387904"));
    do_test(run_test_test(1, L"-4611686018427387904 -ge 4611686018427387904"));
    do_test(run_test_test(1, L"4611686018427387904 -gt 4611686018427387904"));
    do_test(run_test_test(0, L"4611686018427387904 -ge 4611686018427387904"));

    // test out-of-range numbers
    do_test(run_test_test(2, L"99999999999999999999999999 -ge 1"));
    do_test(run_test_test(2, L"1 -eq -99999999999999999999999999.9"));
}

static void test_wcstod() {
    say(L"Testing fish_wcstod");
    auto tod_test = [](const wchar_t *a, const char *b) {
        char *narrow_end = nullptr;
        wchar_t *wide_end = nullptr;
        double val1 = std::wcstod(a, &wide_end);
        double val2 = strtod(b, &narrow_end);
        do_test((std::isnan(val1) && std::isnan(val2)) || fabs(val1 - val2) <= __DBL_EPSILON__);
        do_test(wide_end - a == narrow_end - b);
    };
    tod_test(L"", "");
    tod_test(L"1.2", "1.2");
    tod_test(L"1.5", "1.5");
    tod_test(L"-1000", "-1000");
    tod_test(L"0.12345", "0.12345");
    tod_test(L"nope", "nope");
}

static void test_dup2s() {
    using std::make_shared;
    io_chain_t chain;
    chain.push_back(make_shared<io_close_t>(17));
    chain.push_back(make_shared<io_fd_t>(3, 19));
    auto list = dup2_list_t::resolve_chain(chain);
    do_test(list.get_actions().size() == 2);

    auto act1 = list.get_actions().at(0);
    do_test(act1.src == 17);
    do_test(act1.target == -1);

    auto act2 = list.get_actions().at(1);
    do_test(act2.src == 19);
    do_test(act2.target == 3);
}

static void test_dup2s_fd_for_target_fd() {
    using std::make_shared;
    io_chain_t chain;
    // note io_fd_t params are backwards from dup2.
    chain.push_back(make_shared<io_close_t>(10));
    chain.push_back(make_shared<io_fd_t>(9, 10));
    chain.push_back(make_shared<io_fd_t>(5, 8));
    chain.push_back(make_shared<io_fd_t>(1, 4));
    chain.push_back(make_shared<io_fd_t>(3, 5));
    auto list = dup2_list_t::resolve_chain(chain);

    do_test(list.fd_for_target_fd(3) == 8);
    do_test(list.fd_for_target_fd(5) == 8);
    do_test(list.fd_for_target_fd(8) == 8);
    do_test(list.fd_for_target_fd(1) == 4);
    do_test(list.fd_for_target_fd(4) == 4);
    do_test(list.fd_for_target_fd(100) == 100);
    do_test(list.fd_for_target_fd(0) == 0);
    do_test(list.fd_for_target_fd(-1) == -1);
    do_test(list.fd_for_target_fd(9) == -1);
    do_test(list.fd_for_target_fd(10) == -1);
}

/// Testing colors.
static void test_colors() {
    say(L"Testing colors");
    do_test(rgb_color_t(L"#FF00A0").is_rgb());
    do_test(rgb_color_t(L"FF00A0").is_rgb());
    do_test(rgb_color_t(L"#F30").is_rgb());
    do_test(rgb_color_t(L"F30").is_rgb());
    do_test(rgb_color_t(L"f30").is_rgb());
    do_test(rgb_color_t(L"#FF30a5").is_rgb());
    do_test(rgb_color_t(L"3f30").is_none());
    do_test(rgb_color_t(L"##f30").is_none());
    do_test(rgb_color_t(L"magenta").is_named());
    do_test(rgb_color_t(L"MaGeNTa").is_named());
    do_test(rgb_color_t(L"mooganta").is_none());
}

// This class allows accessing private bits of autoload_t.
struct autoload_tester_t {
    static void run(const wchar_t *fmt, ...) {
        va_list va;
        va_start(va, fmt);
        wcstring cmd = vformat_string(fmt, va);
        va_end(va);

        int status = system(wcs2string(cmd).c_str());
        do_test(status == 0);
    }

    static void touch_file(const wcstring &path) {
        int fd = wopen_cloexec(path, O_RDWR | O_CREAT, 0666);
        do_test(fd >= 0);
        write_loop(fd, "Hello", 5);
        close(fd);
    }

    static void run_test() {
        char t1[] = "/tmp/fish_test_autoload.XXXXXX";
        wcstring p1 = str2wcstring(mkdtemp(t1));
        char t2[] = "/tmp/fish_test_autoload.XXXXXX";
        wcstring p2 = str2wcstring(mkdtemp(t2));

        const wcstring_list_t paths = {p1, p2};

        autoload_t autoload(L"test_var");
        do_test(!autoload.resolve_command(L"file1", paths));
        do_test(!autoload.resolve_command(L"nothing", paths));
        do_test(autoload.get_autoloaded_commands().empty());

        run(L"touch %ls/file1.fish", p1.c_str());
        run(L"touch %ls/file2.fish", p2.c_str());
        autoload.invalidate_cache();

        do_test(!autoload.autoload_in_progress(L"file1"));
        do_test(autoload.resolve_command(L"file1", paths));
        do_test(!autoload.resolve_command(L"file1", paths));
        do_test(autoload.autoload_in_progress(L"file1"));
        do_test(autoload.get_autoloaded_commands() == wcstring_list_t{L"file1"});
        autoload.mark_autoload_finished(L"file1");
        do_test(!autoload.autoload_in_progress(L"file1"));
        do_test(autoload.get_autoloaded_commands() == wcstring_list_t{L"file1"});

        do_test(!autoload.resolve_command(L"file1", paths));
        do_test(!autoload.resolve_command(L"nothing", paths));
        do_test(autoload.resolve_command(L"file2", paths));
        do_test(!autoload.resolve_command(L"file2", paths));
        autoload.mark_autoload_finished(L"file2");
        do_test(!autoload.resolve_command(L"file2", paths));
        do_test((autoload.get_autoloaded_commands() == wcstring_list_t{L"file1", L"file2"}));

        autoload.clear();
        do_test(autoload.resolve_command(L"file1", paths));
        autoload.mark_autoload_finished(L"file1");
        do_test(!autoload.resolve_command(L"file1", paths));
        do_test(!autoload.resolve_command(L"nothing", paths));
        do_test(autoload.resolve_command(L"file2", paths));
        do_test(!autoload.resolve_command(L"file2", paths));
        autoload.mark_autoload_finished(L"file2");

        do_test(!autoload.resolve_command(L"file1", paths));
        touch_file(format_string(L"%ls/file1.fish", p1.c_str()));
        autoload.invalidate_cache();
        do_test(autoload.resolve_command(L"file1", paths));
        autoload.mark_autoload_finished(L"file1");

        run(L"rm -Rf %ls", p1.c_str());
        run(L"rm -Rf %ls", p2.c_str());
    }
};

static void test_autoload() {
    say(L"Testing autoload");
    autoload_tester_t::run_test();
}

// Construct function properties for testing.
static std::shared_ptr<function_properties_t> make_test_func_props() {
    auto ret = std::make_shared<function_properties_t>();
    ret->parsed_source = parse_source(L"function stuff; end", parse_flag_none, nullptr);
    assert(ret->parsed_source && "Failed to parse");
    for (const auto &node : ret->parsed_source->ast) {
        if (const auto *s = node.try_as<ast::block_statement_t>()) {
            ret->func_node = s;
            break;
        }
    }
    assert(ret->func_node && "Unable to find block statement");
    return ret;
}

static void test_complete() {
    say(L"Testing complete");

    auto func_props = make_test_func_props();
    struct test_complete_vars_t : environment_t {
        wcstring_list_t get_names(int flags) const override {
            UNUSED(flags);
            return {L"Foo1", L"Foo2",  L"Foo3",   L"Bar1",   L"Bar2",
                    L"Bar3", L"alpha", L"ALPHA!", L"gamma1", L"GAMMA2"};
        }

        maybe_t<env_var_t> get(const wcstring &key,
                               env_mode_flags_t mode = ENV_DEFAULT) const override {
            UNUSED(mode);
            if (key == L"PWD") {
                return env_var_t{wgetcwd(), 0};
            }
            return {};
        }
    };
    test_complete_vars_t vars;

    auto parser = parser_t::principal_parser().shared();

    auto do_complete = [&](const wcstring &cmd, completion_request_flags_t flags) {
        return complete(cmd, flags, operation_context_t{parser, vars, no_cancel});
    };

    completion_list_t completions;

    completions = do_complete(L"$", {});
    completions_sort_and_prioritize(&completions);
    do_test(completions.size() == 10);
    do_test(completions.at(0).completion == L"alpha");
    do_test(completions.at(1).completion == L"ALPHA!");
    do_test(completions.at(2).completion == L"Bar1");
    do_test(completions.at(3).completion == L"Bar2");
    do_test(completions.at(4).completion == L"Bar3");
    do_test(completions.at(5).completion == L"Foo1");
    do_test(completions.at(6).completion == L"Foo2");
    do_test(completions.at(7).completion == L"Foo3");
    do_test(completions.at(8).completion == L"gamma1");
    do_test(completions.at(9).completion == L"GAMMA2");

    // Smartcase test. Lowercase inputs match both lowercase and uppercase.
    completions = do_complete(L"$a", {});
    completions_sort_and_prioritize(&completions);
    do_test(completions.size() == 2);
    do_test(completions.at(0).completion == L"$ALPHA!");
    do_test(completions.at(1).completion == L"lpha");

    completions = do_complete(L"$F", {});
    completions_sort_and_prioritize(&completions);
    do_test(completions.size() == 3);
    do_test(completions.at(0).completion == L"oo1");
    do_test(completions.at(1).completion == L"oo2");
    do_test(completions.at(2).completion == L"oo3");

    completions = do_complete(L"$1", {});
    completions_sort_and_prioritize(&completions);
    do_test(completions.empty());

    completions = do_complete(L"$1", completion_request_t::fuzzy_match);
    completions_sort_and_prioritize(&completions);
    do_test(completions.size() == 3);
    do_test(completions.at(0).completion == L"$Bar1");
    do_test(completions.at(1).completion == L"$Foo1");
    do_test(completions.at(2).completion == L"$gamma1");

    if (system("mkdir -p 'test/complete_test'")) err(L"mkdir failed");
    if (system("touch 'test/complete_test/has space'")) err(L"touch failed");
    if (system("touch 'test/complete_test/bracket[abc]'")) err(L"touch failed");
#ifndef __CYGWIN__  // Square brackets are not legal path characters on WIN32/CYGWIN
    if (system(R"(touch 'test/complete_test/gnarlybracket\[abc]')")) err(L"touch failed");
#endif
    if (system("touch 'test/complete_test/testfile'")) err(L"touch failed");
    if (system("chmod 700 'test/complete_test/testfile'")) err(L"chmod failed");
    if (system("mkdir -p 'test/complete_test/foo1'")) err(L"mkdir failed");
    if (system("mkdir -p 'test/complete_test/foo2'")) err(L"mkdir failed");
    if (system("mkdir -p 'test/complete_test/foo3'")) err(L"mkdir failed");

    completions = do_complete(L"echo (test/complete_test/testfil", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"e");

    completions = do_complete(L"echo (ls test/complete_test/testfil", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"e");

    completions = do_complete(L"echo (command ls test/complete_test/testfil", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"e");

    // Completing after spaces - see #2447
    completions = do_complete(L"echo (ls test/complete_test/has\\ ", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"space");

    // Brackets - see #5831
    completions = do_complete(L"echo (ls test/complete_test/bracket[", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"test/complete_test/bracket[abc]");

    wcstring cmdline = L"touch test/complete_test/bracket[";
    completions = do_complete(cmdline, {});
    do_test(completions.size() == 1);
    do_test(completions.front().completion == L"test/complete_test/bracket[abc]");
    size_t where = cmdline.size();
    wcstring newcmdline = completion_apply_to_command_line(
        completions.front().completion, completions.front().flags, cmdline, &where, false);
    do_test(newcmdline == L"touch test/complete_test/bracket\\[abc\\] ");

#ifndef __CYGWIN__  // Square brackets are not legal path characters on WIN32/CYGWIN
    cmdline = LR"(touch test/complete_test/gnarlybracket\\[)";
    completions = do_complete(cmdline, {});
    do_test(completions.size() == 1);
    do_test(completions.front().completion == LR"(test/complete_test/gnarlybracket\[abc])");
    where = cmdline.size();
    newcmdline = completion_apply_to_command_line(
        completions.front().completion, completions.front().flags, cmdline, &where, false);
    do_test(newcmdline == LR"(touch test/complete_test/gnarlybracket\\\[abc\] )");
#endif

    // Add a function and test completing it in various ways.
    function_add(L"scuttlebutt", func_props);

    // Complete a function name.
    completions = do_complete(L"echo (scuttlebut", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"t");

    // But not with the command prefix.
    completions = do_complete(L"echo (command scuttlebut", {});
    do_test(completions.empty());

    // Not with the builtin prefix.
    completions = do_complete(L"echo (builtin scuttlebut", {});
    do_test(completions.empty());

    // Not after a redirection.
    completions = do_complete(L"echo hi > scuttlebut", {});
    do_test(completions.empty());

    // Trailing spaces (#1261).
    completion_mode_t no_files{};
    no_files.no_files = true;
    complete_add(L"foobarbaz", false, wcstring(), option_type_args_only, no_files, nullptr, L"qux",
                 nullptr, COMPLETE_AUTO_SPACE);
    completions = do_complete(L"foobarbaz ", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"qux");

    // Don't complete variable names in single quotes (#1023).
    completions = do_complete(L"echo '$Foo", {});
    do_test(completions.empty());
    completions = do_complete(L"echo \\$Foo", {});
    do_test(completions.empty());

    // File completions.
    completions = do_complete(L"cat test/complete_test/te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");
    completions = do_complete(L"echo sup > test/complete_test/te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");
    completions = do_complete(L"echo sup > test/complete_test/te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");

    if (!pushd("test/complete_test")) return;
    completions = do_complete(L"cat te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");
    do_test(!(completions.at(0).flags & COMPLETE_REPLACES_TOKEN));
    do_test(!(completions.at(0).flags & COMPLETE_DUPLICATES_ARGUMENT));
    completions = do_complete(L"cat testfile te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");
    do_test(completions.at(0).flags & COMPLETE_DUPLICATES_ARGUMENT);
    completions = do_complete(L"cat testfile TE", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"testfile");
    do_test(completions.at(0).flags & COMPLETE_REPLACES_TOKEN);
    do_test(completions.at(0).flags & COMPLETE_DUPLICATES_ARGUMENT);
    completions = do_complete(L"something --abc=te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");
    completions = do_complete(L"something -abc=te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");
    completions = do_complete(L"something abc=te", {});
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"stfile");
    completions = do_complete(L"something abc=stfile", {});
    do_test(completions.empty());
    completions = do_complete(L"something abc=stfile", completion_request_t::fuzzy_match);
    do_test(completions.size() == 1);
    do_test(completions.at(0).completion == L"abc=testfile");

    // Zero escapes can cause problems. See issue #1631.
    completions = do_complete(L"cat foo\\0", {});
    do_test(completions.empty());
    completions = do_complete(L"cat foo\\0bar", {});
    do_test(completions.empty());
    completions = do_complete(L"cat \\0", {});
    do_test(completions.empty());
    completions = do_complete(L"cat te\\0", {});
    do_test(completions.empty());

    popd();
    completions.clear();

    // Test abbreviations.
    auto &pvars = parser_t::principal_parser().vars();
    function_add(L"testabbrsonetwothreefour", func_props);
    int ret = pvars.set_one(L"_fish_abbr_testabbrsonetwothreezero", ENV_LOCAL, L"expansion");
    completions = complete(L"testabbrsonetwothree", {}, parser->context());
    do_test(ret == 0);
    do_test(completions.size() == 2);
    do_test(completions.at(0).completion == L"four");
    do_test((completions.at(0).flags & COMPLETE_NO_SPACE) == 0);
    // Abbreviations should not have a space after them.
    do_test(completions.at(1).completion == L"zero");
    do_test((completions.at(1).flags & COMPLETE_NO_SPACE) != 0);

    // Test wraps.
    do_test(comma_join(complete_get_wrap_targets(L"wrapper1")).empty());
    complete_add_wrapper(L"wrapper1", L"wrapper2");
    do_test(comma_join(complete_get_wrap_targets(L"wrapper1")) == L"wrapper2");
    complete_add_wrapper(L"wrapper2", L"wrapper3");
    do_test(comma_join(complete_get_wrap_targets(L"wrapper1")) == L"wrapper2");
    do_test(comma_join(complete_get_wrap_targets(L"wrapper2")) == L"wrapper3");
    complete_add_wrapper(L"wrapper3", L"wrapper1");  // loop!
    do_test(comma_join(complete_get_wrap_targets(L"wrapper1")) == L"wrapper2");
    do_test(comma_join(complete_get_wrap_targets(L"wrapper2")) == L"wrapper3");
    do_test(comma_join(complete_get_wrap_targets(L"wrapper3")) == L"wrapper1");
    complete_remove_wrapper(L"wrapper1", L"wrapper2");
    do_test(comma_join(complete_get_wrap_targets(L"wrapper1")).empty());
    do_test(comma_join(complete_get_wrap_targets(L"wrapper2")) == L"wrapper3");
    do_test(comma_join(complete_get_wrap_targets(L"wrapper3")) == L"wrapper1");

    // Test cd wrapping chain
    if (!pushd("test/complete_test")) err(L"pushd(\"test/complete_test\") failed");

    complete_add_wrapper(L"cdwrap1", L"cd");
    complete_add_wrapper(L"cdwrap2", L"cdwrap1");

    completion_list_t cd_compl = do_complete(L"cd ", {});
    completions_sort_and_prioritize(&cd_compl);

    completion_list_t cdwrap1_compl = do_complete(L"cdwrap1 ", {});
    completions_sort_and_prioritize(&cdwrap1_compl);

    completion_list_t cdwrap2_compl = do_complete(L"cdwrap2 ", {});
    completions_sort_and_prioritize(&cdwrap2_compl);

    size_t min_compl_size =
        std::min(cd_compl.size(), std::min(cdwrap1_compl.size(), cdwrap2_compl.size()));

    do_test(cd_compl.size() == min_compl_size);
    do_test(cdwrap1_compl.size() == min_compl_size);
    do_test(cdwrap2_compl.size() == min_compl_size);
    for (size_t i = 0; i < min_compl_size; ++i) {
        do_test(cd_compl[i].completion == cdwrap1_compl[i].completion);
        do_test(cdwrap1_compl[i].completion == cdwrap2_compl[i].completion);
    }

    complete_remove_wrapper(L"cdwrap1", L"cd");
    complete_remove_wrapper(L"cdwrap2", L"cdwrap1");
    popd();
}

static void test_1_completion(wcstring line, const wcstring &completion, complete_flags_t flags,
                              bool append_only, wcstring expected, long source_line) {
    // str is given with a caret, which we use to represent the cursor position. Find it.
    const size_t in_cursor_pos = line.find(L'^');
    do_test(in_cursor_pos != wcstring::npos);
    line.erase(in_cursor_pos, 1);

    const size_t out_cursor_pos = expected.find(L'^');
    do_test(out_cursor_pos != wcstring::npos);
    expected.erase(out_cursor_pos, 1);

    size_t cursor_pos = in_cursor_pos;
    wcstring result =
        completion_apply_to_command_line(completion, flags, line, &cursor_pos, append_only);
    if (result != expected) {
        std::fwprintf(stderr, L"line %ld: %ls + %ls -> [%ls], expected [%ls]\n", source_line,
                      line.c_str(), completion.c_str(), result.c_str(), expected.c_str());
    }
    do_test(result == expected);
    do_test(cursor_pos == out_cursor_pos);
}

static void test_wait_handles() {
    say(L"Testing wait handles");
    constexpr size_t limit = 4;
    wait_handle_store_t whs(limit);
    do_test(whs.size() == 0);

    // Null handles ignored.
    whs.add(wait_handle_ref_t{});
    do_test(whs.size() == 0);
    do_test(whs.get_by_pid(5) == nullptr);

    // Duplicate pids drop oldest.
    whs.add(std::make_shared<wait_handle_t>(5, 0, L"first"));
    whs.add(std::make_shared<wait_handle_t>(5, 0, L"second"));
    do_test(whs.size() == 1);
    do_test(whs.get_by_pid(5)->base_name == L"second");

    whs.remove_by_pid(123);
    do_test(whs.size() == 1);
    whs.remove_by_pid(5);
    do_test(whs.size() == 0);

    // Test evicting oldest.
    whs.add(std::make_shared<wait_handle_t>(1, 0, L"1"));
    whs.add(std::make_shared<wait_handle_t>(2, 0, L"2"));
    whs.add(std::make_shared<wait_handle_t>(3, 0, L"3"));
    whs.add(std::make_shared<wait_handle_t>(4, 0, L"4"));
    whs.add(std::make_shared<wait_handle_t>(5, 0, L"5"));
    do_test(whs.size() == 4);
    auto start = whs.get_list().begin();
    do_test(std::next(start, 0)->get()->base_name == L"5");
    do_test(std::next(start, 1)->get()->base_name == L"4");
    do_test(std::next(start, 2)->get()->base_name == L"3");
    do_test(std::next(start, 3)->get()->base_name == L"2");
}

static void test_completion_insertions() {
#define TEST_1_COMPLETION(a, b, c, d, e) test_1_completion(a, b, c, d, e, __LINE__)
    say(L"Testing completion insertions");
    TEST_1_COMPLETION(L"foo^", L"bar", 0, false, L"foobar ^");
    // An unambiguous completion of a token that is already trailed by a space character.
    // After completing, the cursor moves on to the next token, suggesting to the user that the
    // current token is finished.
    TEST_1_COMPLETION(L"foo^ baz", L"bar", 0, false, L"foobar ^baz");
    TEST_1_COMPLETION(L"'foo^", L"bar", 0, false, L"'foobar' ^");
    TEST_1_COMPLETION(L"'foo'^", L"bar", 0, false, L"'foobar' ^");
    TEST_1_COMPLETION(L"'foo\\'^", L"bar", 0, false, L"'foo\\'bar' ^");
    TEST_1_COMPLETION(L"foo\\'^", L"bar", 0, false, L"foo\\'bar ^");

    // Test append only.
    TEST_1_COMPLETION(L"foo^", L"bar", 0, true, L"foobar ^");
    TEST_1_COMPLETION(L"foo^ baz", L"bar", 0, true, L"foobar ^baz");
    TEST_1_COMPLETION(L"'foo^", L"bar", 0, true, L"'foobar' ^");
    TEST_1_COMPLETION(L"'foo'^", L"bar", 0, true, L"'foo'bar ^");
    TEST_1_COMPLETION(L"'foo\\'^", L"bar", 0, true, L"'foo\\'bar' ^");
    TEST_1_COMPLETION(L"foo\\'^", L"bar", 0, true, L"foo\\'bar ^");

    TEST_1_COMPLETION(L"foo^", L"bar", COMPLETE_NO_SPACE, false, L"foobar^");
    TEST_1_COMPLETION(L"'foo^", L"bar", COMPLETE_NO_SPACE, false, L"'foobar^");
    TEST_1_COMPLETION(L"'foo'^", L"bar", COMPLETE_NO_SPACE, false, L"'foobar'^");
    TEST_1_COMPLETION(L"'foo\\'^", L"bar", COMPLETE_NO_SPACE, false, L"'foo\\'bar^");
    TEST_1_COMPLETION(L"foo\\'^", L"bar", COMPLETE_NO_SPACE, false, L"foo\\'bar^");

    TEST_1_COMPLETION(L"foo^", L"bar", COMPLETE_REPLACES_TOKEN, false, L"bar ^");
    TEST_1_COMPLETION(L"'foo^", L"bar", COMPLETE_REPLACES_TOKEN, false, L"bar ^");

    // See #6130
    TEST_1_COMPLETION(L": (:^ ''", L"", 0, false, L": (: ^''");
}

static void perform_one_autosuggestion_cd_test(const wcstring &command, const wcstring &expected,
                                               const environment_t &vars, long line) {
    completion_list_t comps =
        complete(command, completion_request_t::autosuggestion, operation_context_t{vars});

    bool expects_error = (expected == L"<error>");

    if (comps.empty() && !expects_error) {
        std::fwprintf(stderr, L"line %ld: autosuggest_suggest_special() failed for command %ls\n",
                      line, command.c_str());
        do_test_from(!comps.empty(), line);
        return;
    } else if (!comps.empty() && expects_error) {
        std::fwprintf(stderr,
                      L"line %ld: autosuggest_suggest_special() was expected to fail but did not, "
                      L"for command %ls\n",
                      line, command.c_str());
        do_test_from(comps.empty(), line);
    }

    if (!comps.empty()) {
        completions_sort_and_prioritize(&comps);
        const completion_t &suggestion = comps.at(0);

        if (suggestion.completion != expected) {
            std::fwprintf(
                stderr,
                L"line %ld: complete() for cd returned the wrong expected string for command %ls\n",
                line, command.c_str());
            std::fwprintf(stderr, L"  actual: %ls\n", suggestion.completion.c_str());
            std::fwprintf(stderr, L"expected: %ls\n", expected.c_str());
            do_test_from(suggestion.completion == expected, line);
        }
    }
}

static void perform_one_completion_cd_test(const wcstring &command, const wcstring &expected,
                                           const environment_t &vars, long line) {
    completion_list_t comps = complete(command, {}, operation_context_t{vars});

    bool expects_error = (expected == L"<error>");

    if (comps.empty() && !expects_error) {
        std::fwprintf(stderr, L"line %ld: autosuggest_suggest_special() failed for command %ls\n",
                      line, command.c_str());
        do_test_from(!comps.empty(), line);
        return;
    } else if (!comps.empty() && expects_error) {
        std::fwprintf(stderr,
                      L"line %ld: autosuggest_suggest_special() was expected to fail but did not, "
                      L"for command %ls\n",
                      line, command.c_str());
        do_test_from(comps.empty(), line);
    }

    if (!comps.empty()) {
        completions_sort_and_prioritize(&comps);
        const completion_t &suggestion = comps.at(0);

        if (suggestion.completion != expected) {
            std::fwprintf(stderr,
                          L"line %ld: complete() for cd tab completion returned the wrong expected "
                          L"string for command %ls\n",
                          line, command.c_str());
            std::fwprintf(stderr, L"  actual: %ls\n", suggestion.completion.c_str());
            std::fwprintf(stderr, L"expected: %ls\n", expected.c_str());
            do_test_from(suggestion.completion == expected, line);
        }
    }
}

// Testing test_autosuggest_suggest_special, in particular for properly handling quotes and
// backslashes.
static void test_autosuggest_suggest_special() {
    if (system("mkdir -p 'test/autosuggest_test/0foobar'")) err(L"mkdir failed");
    if (system("mkdir -p 'test/autosuggest_test/1foo bar'")) err(L"mkdir failed");
    if (system("mkdir -p 'test/autosuggest_test/2foo  bar'")) err(L"mkdir failed");
    if (system("mkdir -p 'test/autosuggest_test/3foo\\bar'")) err(L"mkdir failed");
    if (system("mkdir -p test/autosuggest_test/4foo\\'bar")) {
        err(L"mkdir failed");  // a path with a single quote
    }
    if (system("mkdir -p test/autosuggest_test/5foo\\\"bar")) {
        err(L"mkdir failed");  // a path with a double quote
    }
    // This is to ensure tilde expansion is handled. See the `cd ~/test_autosuggest_suggest_specia`
    // test below.
    // Fake out the home directory
    parser_t::principal_parser().vars().set_one(L"HOME", ENV_LOCAL | ENV_EXPORT, L"test/test-home");
    if (system("mkdir -p test/test-home/test_autosuggest_suggest_special/")) {
        err(L"mkdir failed");
    }
    if (system("mkdir -p test/autosuggest_test/start/unique2/unique3/multi4")) {
        err(L"mkdir failed");
    }
    if (system("mkdir -p test/autosuggest_test/start/unique2/unique3/multi42")) {
        err(L"mkdir failed");
    }
    if (system("mkdir -p test/autosuggest_test/start/unique2/.hiddenDir/moreStuff")) {
        err(L"mkdir failed");
    }

    const wcstring wd = L"test/autosuggest_test";

    pwd_environment_t vars{};
    vars.vars[L"HOME"] = parser_t::principal_parser().vars().get(L"HOME")->as_string();

    perform_one_autosuggestion_cd_test(L"cd test/autosuggest_test/0", L"foobar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"test/autosuggest_test/0", L"foobar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 'test/autosuggest_test/0", L"foobar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd test/autosuggest_test/1", L"foo bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"test/autosuggest_test/1", L"foo bar/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 'test/autosuggest_test/1", L"foo bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd test/autosuggest_test/2", L"foo  bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"test/autosuggest_test/2", L"foo  bar/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 'test/autosuggest_test/2", L"foo  bar/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd test/autosuggest_test/3", L"foo\\bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"test/autosuggest_test/3", L"foo\\bar/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 'test/autosuggest_test/3", L"foo\\bar/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd test/autosuggest_test/4", L"foo'bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"test/autosuggest_test/4", L"foo'bar/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 'test/autosuggest_test/4", L"foo'bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd test/autosuggest_test/5", L"foo\"bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"test/autosuggest_test/5", L"foo\"bar/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 'test/autosuggest_test/5", L"foo\"bar/", vars,
                                       __LINE__);

    vars.vars[L"AUTOSUGGEST_TEST_LOC"] = wd;
    perform_one_autosuggestion_cd_test(L"cd $AUTOSUGGEST_TEST_LOC/0", L"foobar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd ~/test_autosuggest_suggest_specia", L"l/", vars,
                                       __LINE__);

    perform_one_autosuggestion_cd_test(L"cd test/autosuggest_test/start/", L"unique2/unique3/",
                                       vars, __LINE__);

    if (!pushd(wcs2string(wd).c_str())) return;
    perform_one_autosuggestion_cd_test(L"cd 0", L"foobar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"0", L"foobar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd '0", L"foobar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 1", L"foo bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"1", L"foo bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd '1", L"foo bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 2", L"foo  bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"2", L"foo  bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd '2", L"foo  bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 3", L"foo\\bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"3", L"foo\\bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd '3", L"foo\\bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 4", L"foo'bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"4", L"foo'bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd '4", L"foo'bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd 5", L"foo\"bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd \"5", L"foo\"bar/", vars, __LINE__);
    perform_one_autosuggestion_cd_test(L"cd '5", L"foo\"bar/", vars, __LINE__);

    // A single quote should defeat tilde expansion.
    perform_one_autosuggestion_cd_test(L"cd '~/test_autosuggest_suggest_specia'", L"<error>", vars,
                                       __LINE__);

    // Don't crash on ~ (issue #2696). Note this is cwd dependent.
    if (system("mkdir -p '~absolutelynosuchuser/path1/path2/'")) err(L"mkdir failed");
    perform_one_autosuggestion_cd_test(L"cd ~absolutelynosuchus", L"er/path1/path2/", vars,
                                       __LINE__);
    perform_one_autosuggestion_cd_test(L"cd ~absolutelynosuchuser/", L"path1/path2/", vars,
                                       __LINE__);
    perform_one_completion_cd_test(L"cd ~absolutelynosuchus", L"er/", vars, __LINE__);
    perform_one_completion_cd_test(L"cd ~absolutelynosuchuser/", L"path1/", vars, __LINE__);

    parser_t::principal_parser().vars().remove(L"HOME", ENV_LOCAL | ENV_EXPORT);
    popd();
}

static void perform_one_autosuggestion_should_ignore_test(const wcstring &command, long line) {
    completion_list_t comps =
        complete(command, completion_request_t::autosuggestion, operation_context_t::empty());
    do_test(comps.empty());
    if (!comps.empty()) {
        const wcstring &suggestion = comps.front().completion;
        std::fwprintf(stderr, L"line %ld: complete() expected to return nothing for %ls\n", line,
                      command.c_str());
        std::fwprintf(stderr, L"  instead got: %ls\n", suggestion.c_str());
    }
}

static void test_autosuggestion_ignores() {
    say(L"Testing scenarios that should produce no autosuggestions");
    // Do not do file autosuggestions immediately after certain statement terminators - see #1631.
    perform_one_autosuggestion_should_ignore_test(L"echo PIPE_TEST|", __LINE__);
    perform_one_autosuggestion_should_ignore_test(L"echo PIPE_TEST&", __LINE__);
    perform_one_autosuggestion_should_ignore_test(L"echo PIPE_TEST#comment", __LINE__);
    perform_one_autosuggestion_should_ignore_test(L"echo PIPE_TEST;", __LINE__);
}

static void test_autosuggestion_combining() {
    say(L"Testing autosuggestion combining");
    do_test(combine_command_and_autosuggestion(L"alpha", L"alphabeta") == L"alphabeta");

    // When the last token contains no capital letters, we use the case of the autosuggestion.
    do_test(combine_command_and_autosuggestion(L"alpha", L"ALPHABETA") == L"ALPHABETA");

    // When the last token contains capital letters, we use its case.
    do_test(combine_command_and_autosuggestion(L"alPha", L"alphabeTa") == L"alPhabeTa");

    // If autosuggestion is not longer than input, use the input's case.
    do_test(combine_command_and_autosuggestion(L"alpha", L"ALPHAA") == L"ALPHAA");
    do_test(combine_command_and_autosuggestion(L"alpha", L"ALPHA") == L"alpha");
}

static void test_history_matches(history_search_t &search, const wcstring_list_t &expected,
                                 unsigned from_line) {
    wcstring_list_t found;
    while (search.go_backwards()) {
        found.push_back(search.current_string());
    }
    do_test_from(expected == found, from_line);
    if (expected != found) {
        fprintf(stderr, "Expected %ls, found %ls\n", comma_join(expected).c_str(),
                comma_join(found).c_str());
    }
}

static bool history_contains(history_t *history, const wcstring &txt) {
    bool result = false;
    size_t i;
    for (i = 1;; i++) {
        history_item_t item = history->item_at_index(i);
        if (item.empty()) break;

        if (item.str() == txt) {
            result = true;
            break;
        }
    }
    return result;
}

static bool history_contains(const std::shared_ptr<history_t> &history, const wcstring &txt) {
    return history_contains(history.get(), txt);
}

static void test_input() {
    say(L"Testing input");
    inputter_t input{parser_t::principal_parser()};
    // Ensure sequences are order independent. Here we add two bindings where the first is a prefix
    // of the second, and then emit the second key list. The second binding should be invoked, not
    // the first!
    wcstring prefix_binding = L"qqqqqqqa";
    wcstring desired_binding = prefix_binding + L'a';

    {
        auto input_mapping = input_mappings();
        input_mapping->add(prefix_binding, L"up-line");
        input_mapping->add(desired_binding, L"down-line");
    }

    // Push the desired binding to the queue.
    for (wchar_t c : desired_binding) {
        input.queue_char(c);
    }

    // Now test.
    auto evt = input.read_char();
    if (!evt.is_readline()) {
        err(L"Event is not a readline");
    } else if (evt.get_readline() != readline_cmd_t::down_line) {
        err(L"Expected to read char down_line");
    }
}

static void test_line_iterator() {
    say(L"Testing line iterator");

    std::string text1 = "Alpha\nBeta\nGamma\n\nDelta\n";
    std::vector<std::string> lines1;
    line_iterator_t<std::string> iter1(text1);
    while (iter1.next()) lines1.push_back(iter1.line());
    do_test((lines1 == std::vector<std::string>{"Alpha", "Beta", "Gamma", "", "Delta"}));

    wcstring text2 = L"\n\nAlpha\nBeta\nGamma\n\nDelta";
    wcstring_list_t lines2;
    line_iterator_t<wcstring> iter2(text2);
    while (iter2.next()) lines2.push_back(iter2.line());
    do_test((lines2 == wcstring_list_t{L"", L"", L"Alpha", L"Beta", L"Gamma", L"", L"Delta"}));
}

static void test_undo() {
    say(L"Testing undo/redo setting and restoring text and cursor position.");

    editable_line_t line;
    do_test(!line.undo());  // nothing to undo
    do_test(line.text().empty());
    do_test(line.position() == 0);
    line.push_edit(edit_t(0, 0, L"a b c"));
    do_test(line.text() == L"a b c");
    do_test(line.position() == 5);
    line.set_position(2);
    line.push_edit(edit_t(2, 1, L"B"));  // replacement right of cursor
    do_test(line.text() == L"a B c");
    line.undo();
    do_test(line.text() == L"a b c");
    do_test(line.position() == 2);
    line.redo();
    do_test(line.text() == L"a B c");
    do_test(line.position() == 3);

    do_test(!line.redo());  // nothing to redo

    line.push_edit(edit_t(0, 2, L""));  // deletion left of cursor
    do_test(line.text() == L"B c");
    do_test(line.position() == 1);
    line.undo();
    do_test(line.text() == L"a B c");
    do_test(line.position() == 3);
    line.redo();
    do_test(line.text() == L"B c");
    do_test(line.position() == 1);

    line.push_edit(edit_t(0, line.size(), L"a b c"));  // replacement left and right of cursor
    do_test(line.text() == L"a b c");
    do_test(line.position() == 5);

    say(L"Testing undoing coalesced edits.");
    line.clear();
    line.push_edit(edit_t(line.position(), 0, L"a"));
    line.insert_coalesce(L"b");
    line.insert_coalesce(L"c");
    do_test(line.undo_history.edits.size() == 1);
    line.push_edit(edit_t(line.position(), 0, L" "));
    do_test(line.undo_history.edits.size() == 2);
    line.undo();
    line.undo();
    line.redo();
    do_test(line.text() == L"abc");
    do_test(line.undo_history.edits.size() == 2);
    // This removes the space insertion from the history, but does not coalesce with the first edit.
    line.push_edit(edit_t(line.position(), 0, L"d"));
    do_test(line.undo_history.edits.size() == 2);
    line.insert_coalesce(L"e");
    do_test(line.text() == L"abcde");
    line.undo();
    do_test(line.text() == L"abc");
}

#define UVARS_PER_THREAD 8
#define UVARS_TEST_PATH L"test/fish_uvars_test/varsfile.txt"

static int test_universal_helper(int x) {
    callback_data_list_t callbacks;
    env_universal_t uvars;
    uvars.initialize_at_path(callbacks, UVARS_TEST_PATH);
    for (int j = 0; j < UVARS_PER_THREAD; j++) {
        const wcstring key = format_string(L"key_%d_%d", x, j);
        const wcstring val = format_string(L"val_%d_%d", x, j);
        uvars.set(key, env_var_t{val, 0});
        bool synced = uvars.sync(callbacks);
        if (!synced) {
            err(L"Failed to sync universal variables after modification");
        }
    }

    // Last step is to delete the first key.
    uvars.remove(format_string(L"key_%d_%d", x, 0));
    bool synced = uvars.sync(callbacks);
    if (!synced) {
        err(L"Failed to sync universal variables after deletion");
    }
    return 0;
}

static void test_universal() {
    say(L"Testing universal variables");
    if (system("mkdir -p test/fish_uvars_test/")) err(L"mkdir failed");

    const int threads = 1;
    for (int i = 0; i < threads; i++) {
        iothread_perform([=]() { test_universal_helper(i); });
    }
    iothread_drain_all();

    env_universal_t uvars;
    callback_data_list_t callbacks;
    uvars.initialize_at_path(callbacks, UVARS_TEST_PATH);
    for (int i = 0; i < threads; i++) {
        for (int j = 0; j < UVARS_PER_THREAD; j++) {
            const wcstring key = format_string(L"key_%d_%d", i, j);
            maybe_t<env_var_t> expected_val;
            if (j == 0) {
                expected_val = none();
            } else {
                expected_val = env_var_t(format_string(L"val_%d_%d", i, j), 0);
            }
            const maybe_t<env_var_t> var = uvars.get(key);
            if (j == 0) assert(!expected_val);
            if (var != expected_val) {
                const wchar_t *missing_desc = L"<missing>";
                err(L"Wrong value for key %ls: expected %ls, got %ls\n", key.c_str(),
                    (expected_val ? expected_val->as_string().c_str() : missing_desc),
                    (var ? var->as_string().c_str() : missing_desc));
            }
        }
    }
    system_assert("rm -Rf test/fish_uvars_test/");
}

static void test_universal_output() {
    say(L"Testing universal variable output");

    const env_var_t::env_var_flags_t flag_export = env_var_t::flag_export;
    const env_var_t::env_var_flags_t flag_pathvar = env_var_t::flag_pathvar;

    var_table_t vars;
    vars[L"varA"] = env_var_t(wcstring_list_t{L"ValA1", L"ValA2"}, 0);
    vars[L"varB"] = env_var_t(wcstring_list_t{L"ValB1"}, flag_export);
    vars[L"varC"] = env_var_t(wcstring_list_t{L"ValC1"}, 0);
    vars[L"varD"] = env_var_t(wcstring_list_t{L"ValD1"}, flag_export | flag_pathvar);
    vars[L"varE"] = env_var_t(wcstring_list_t{L"ValE1", L"ValE2"}, flag_pathvar);

    std::string text = env_universal_t::serialize_with_vars(vars);
    const char *expected =
        "# This file contains fish universal variable definitions.\n"
        "# VERSION: 3.0\n"
        "SETUVAR varA:ValA1\\x1eValA2\n"
        "SETUVAR --export varB:ValB1\n"
        "SETUVAR varC:ValC1\n"
        "SETUVAR --export --path varD:ValD1\n"
        "SETUVAR --path varE:ValE1\\x1eValE2\n";
    do_test(text == expected);
}

static void test_universal_parsing() {
    say(L"Testing universal variable parsing");
    const char *input =
        "# This file contains fish universal variable definitions.\n"
        "# VERSION: 3.0\n"
        "SETUVAR varA:ValA1\\x1eValA2\n"
        "SETUVAR --export varB:ValB1\n"
        "SETUVAR --nonsenseflag varC:ValC1\n"
        "SETUVAR --export --path varD:ValD1\n"
        "SETUVAR --path --path varE:ValE1\\x1eValE2\n";

    const env_var_t::env_var_flags_t flag_export = env_var_t::flag_export;
    const env_var_t::env_var_flags_t flag_pathvar = env_var_t::flag_pathvar;

    var_table_t vars;
    vars[L"varA"] = env_var_t(wcstring_list_t{L"ValA1", L"ValA2"}, 0);
    vars[L"varB"] = env_var_t(wcstring_list_t{L"ValB1"}, flag_export);
    vars[L"varC"] = env_var_t(wcstring_list_t{L"ValC1"}, 0);
    vars[L"varD"] = env_var_t(wcstring_list_t{L"ValD1"}, flag_export | flag_pathvar);
    vars[L"varE"] = env_var_t(wcstring_list_t{L"ValE1", L"ValE2"}, flag_pathvar);

    var_table_t parsed_vars;
    env_universal_t::populate_variables(input, &parsed_vars);
    do_test(vars == parsed_vars);
}

static void test_universal_parsing_legacy() {
    say(L"Testing universal variable legacy parsing");
    const char *input =
        "# This file contains fish universal variable definitions.\n"
        "SET varA:ValA1\\x1eValA2\n"
        "SET_EXPORT varB:ValB1\n";

    var_table_t vars;
    vars[L"varA"] = env_var_t(wcstring_list_t{L"ValA1", L"ValA2"}, 0);
    vars[L"varB"] = env_var_t(wcstring_list_t{L"ValB1"}, env_var_t::flag_export);

    var_table_t parsed_vars;
    env_universal_t::populate_variables(input, &parsed_vars);
    do_test(vars == parsed_vars);
}

static bool callback_data_less_than(const callback_data_t &a, const callback_data_t &b) {
    return a.key < b.key;
}

static void test_universal_callbacks() {
    say(L"Testing universal callbacks");
    if (system("mkdir -p test/fish_uvars_test/")) err(L"mkdir failed");
    callback_data_list_t callbacks;
    env_universal_t uvars1;
    env_universal_t uvars2;
    uvars1.initialize_at_path(callbacks, UVARS_TEST_PATH);
    uvars2.initialize_at_path(callbacks, UVARS_TEST_PATH);

    env_var_t::env_var_flags_t noflags = 0;

    // Put some variables into both.
    uvars1.set(L"alpha", env_var_t{L"1", noflags});
    uvars1.set(L"beta", env_var_t{L"1", noflags});
    uvars1.set(L"delta", env_var_t{L"1", noflags});
    uvars1.set(L"epsilon", env_var_t{L"1", noflags});
    uvars1.set(L"lambda", env_var_t{L"1", noflags});
    uvars1.set(L"kappa", env_var_t{L"1", noflags});
    uvars1.set(L"omicron", env_var_t{L"1", noflags});

    uvars1.sync(callbacks);
    uvars2.sync(callbacks);

    // Change uvars1.
    uvars1.set(L"alpha", env_var_t{L"2", noflags});                // changes value
    uvars1.set(L"beta", env_var_t{L"1", env_var_t::flag_export});  // changes export
    uvars1.remove(L"delta");                                       // erases value
    uvars1.set(L"epsilon", env_var_t{L"1", noflags});              // changes nothing
    uvars1.sync(callbacks);

    // Change uvars2. It should treat its value as correct and ignore changes from uvars1.
    uvars2.set(L"lambda", {L"1", noflags});  // same value
    uvars2.set(L"kappa", {L"2", noflags});   // different value

    // Now see what uvars2 sees.
    callbacks.clear();
    uvars2.sync(callbacks);

    // Sort them to get them in a predictable order.
    std::sort(callbacks.begin(), callbacks.end(), callback_data_less_than);

    // Should see exactly three changes.
    do_test(callbacks.size() == 3);
    do_test(callbacks.at(0).key == L"alpha");
    do_test(callbacks.at(0).val == wcstring{L"2"});
    do_test(callbacks.at(1).key == L"beta");
    do_test(callbacks.at(1).val == wcstring{L"1"});
    do_test(callbacks.at(2).key == L"delta");
    do_test(callbacks.at(2).val == none());
    system_assert("rm -Rf test/fish_uvars_test/");
}

static void test_universal_formats() {
    say(L"Testing universal format detection");
    const struct {
        const char *str;
        uvar_format_t format;
    } tests[] = {
        {"# VERSION: 3.0", uvar_format_t::fish_3_0},
        {"# version: 3.0", uvar_format_t::fish_2_x},
        {"# blah blahVERSION: 3.0", uvar_format_t::fish_2_x},
        {"stuff\n# blah blahVERSION: 3.0", uvar_format_t::fish_2_x},
        {"# blah\n# VERSION: 3.0", uvar_format_t::fish_3_0},
        {"# blah\n#VERSION: 3.0", uvar_format_t::fish_3_0},
        {"# blah\n#VERSION:3.0", uvar_format_t::fish_3_0},
        {"# blah\n#VERSION:3.1", uvar_format_t::future},
    };
    for (const auto &test : tests) {
        uvar_format_t format = env_universal_t::format_for_contents(test.str);
        do_test(format == test.format);
    }
}

static void test_universal_ok_to_save() {
    // Ensure we don't try to save after reading from a newer fish.
    say(L"Testing universal Ok to save");
    if (system("mkdir -p test/fish_uvars_test/")) err(L"mkdir failed");
    constexpr const char contents[] = "# VERSION: 99999.99\n";
    FILE *fp = fopen(wcs2string(UVARS_TEST_PATH).c_str(), "w");
    assert(fp && "Failed to open UVARS_TEST_PATH for writing");
    fwrite(contents, const_strlen(contents), 1, fp);
    fclose(fp);

    file_id_t before_id = file_id_for_path(UVARS_TEST_PATH);
    do_test(before_id != kInvalidFileID && "UVARS_TEST_PATH should be readable");

    callback_data_list_t cbs;
    env_universal_t uvars;
    uvars.initialize_at_path(cbs, UVARS_TEST_PATH);
    do_test(!uvars.is_ok_to_save() && "Should not be OK to save");
    uvars.sync(cbs);
    cbs.clear();
    do_test(!uvars.is_ok_to_save() && "Should still not be OK to save");
    uvars.set(L"SOMEVAR", env_var_t{wcstring{L"SOMEVALUE"}, 0});
    uvars.sync(cbs);

    // Ensure file is same.
    file_id_t after_id = file_id_for_path(UVARS_TEST_PATH);
    do_test(before_id == after_id && "UVARS_TEST_PATH should not have changed");
    system_assert("rm -Rf test/fish_uvars_test/");
}

bool poll_notifier(const std::unique_ptr<universal_notifier_t> &note) {
    if (note->poll()) return true;

    bool result = false;
    int fd = note->notification_fd();
    if (fd >= 0 && select_wrapper_t::poll_fd_readable(fd)) {
        result = note->notification_fd_became_readable(fd);
    }
    return result;
}

static void test_notifiers_with_strategy(universal_notifier_t::notifier_strategy_t strategy) {
    say(L"Testing universal notifiers with strategy %d", (int)strategy);
    constexpr size_t notifier_count = 16;
    std::unique_ptr<universal_notifier_t> notifiers[notifier_count];

    // Populate array of notifiers.
    for (auto &notifier : notifiers) {
        notifier = universal_notifier_t::new_notifier_for_strategy(strategy, UVARS_TEST_PATH);
    }

    // Nobody should poll yet.
    for (const auto &notifier : notifiers) {
        if (poll_notifier(notifier)) {
            err(L"Universal variable notifier polled true before any changes, with strategy %d",
                (int)strategy);
        }
    }

    // Tweak each notifier. Verify that others see it.
    for (size_t post_idx = 0; post_idx < notifier_count; post_idx++) {
        notifiers[post_idx]->post_notification();

        if (strategy == universal_notifier_t::strategy_notifyd) {
            // notifyd requires a round trip to the notifyd server, which means we have to wait a
            // little bit to receive it. In practice 40 ms seems to be enough.
            usleep(40000);
        }

        for (size_t i = 0; i < notifier_count; i++) {
            bool polled = poll_notifier(notifiers[i]);

            // We aren't concerned with the one who posted. Poll from it (to drain it), and then
            // skip it.
            if (i == post_idx) {
                continue;
            }

            if (!polled) {
                err(L"Universal variable notifier (%lu) %p polled failed to notice changes, with "
                    L"strategy %d",
                    i, notifiers[i].get(), (int)strategy);
                continue;
            }
            // It should not poll again immediately.
            if (poll_notifier(notifiers[i])) {
                err(L"Universal variable notifier (%lu) %p polled twice in a row with strategy %d",
                    i, notifiers[i].get(), (int)strategy);
            }
        }

        // Named pipes have special cleanup requirements.
        if (strategy == universal_notifier_t::strategy_named_pipe) {
            usleep(1000000 / 10);  // corresponds to NAMED_PIPE_FLASH_DURATION_USEC
            // Have to clean up the posted one first, so that the others see the pipe become no
            // longer readable.
            poll_notifier(notifiers[post_idx]);
            for (const auto &notifier : notifiers) {
                poll_notifier(notifier);
            }
        }
    }

    // Nobody should poll now.
    for (const auto &notifier : notifiers) {
        if (poll_notifier(notifier)) {
            err(L"Universal variable notifier polled true after all changes, with strategy %d",
                (int)strategy);
        }
    }
}

static void test_universal_notifiers() {
    if (system("mkdir -p test/fish_uvars_test/ && touch test/fish_uvars_test/varsfile.txt")) {
        err(L"mkdir failed");
    }

    auto strategy = universal_notifier_t::resolve_default_strategy();
    test_notifiers_with_strategy(strategy);
}

class history_tests_t {
   public:
    static void test_history();
    static void test_history_merge();
    static void test_history_path_detection();
    static void test_history_formats();
    // static void test_history_speed(void);
    static void test_history_races();
    static void test_history_races_pound_on_history(size_t item_count, size_t idx);
};

static wcstring random_string() {
    wcstring result;
    size_t max = 1 + random() % 32;
    while (max--) {
        wchar_t c = 1 + random() % ESCAPE_TEST_CHAR;
        result.push_back(c);
    }
    return result;
}

void history_tests_t::test_history() {
    history_search_t searcher;
    say(L"Testing history");

    const wcstring_list_t items = {L"Gamma", L"beta",  L"BetA", L"Beta", L"alpha",
                                   L"AlphA", L"Alpha", L"alph", L"ALPH", L"ZZZ"};
    const history_search_flags_t nocase = history_search_ignore_case;

    // Populate a history.
    std::shared_ptr<history_t> history = history_t::with_name(L"test_history");
    history->clear();
    for (const wcstring &s : items) {
        history->add(s);
    }

    // Helper to set expected items to those matching a predicate, in reverse order.
    wcstring_list_t expected;
    auto set_expected = [&](const std::function<bool(const wcstring &)> &filt) {
        expected.clear();
        for (const auto &s : items) {
            if (filt(s)) expected.push_back(s);
        }
        std::reverse(expected.begin(), expected.end());
    };

    // Items matching "a", case-sensitive.
    searcher = history_search_t(history, L"a");
    set_expected([](const wcstring &s) { return s.find(L'a') != wcstring::npos; });
    test_history_matches(searcher, expected, __LINE__);

    // Items matching "alpha", case-insensitive.
    searcher = history_search_t(history, L"AlPhA", history_search_type_t::contains, nocase);
    set_expected([](const wcstring &s) { return wcstolower(s).find(L"alpha") != wcstring::npos; });
    test_history_matches(searcher, expected, __LINE__);

    // Items matching "et", case-sensitive.
    searcher = history_search_t(history, L"et");
    set_expected([](const wcstring &s) { return s.find(L"et") != wcstring::npos; });
    test_history_matches(searcher, expected, __LINE__);

    // Items starting with "be", case-sensitive.
    searcher = history_search_t(history, L"be", history_search_type_t::prefix, 0);
    set_expected([](const wcstring &s) { return string_prefixes_string(L"be", s); });
    test_history_matches(searcher, expected, __LINE__);

    // Items starting with "be", case-insensitive.
    searcher = history_search_t(history, L"be", history_search_type_t::prefix, nocase);
    set_expected(
        [](const wcstring &s) { return string_prefixes_string_case_insensitive(L"be", s); });
    test_history_matches(searcher, expected, __LINE__);

    // Items exactly matching "alph", case-sensitive.
    searcher = history_search_t(history, L"alph", history_search_type_t::exact, 0);
    set_expected([](const wcstring &s) { return s == L"alph"; });
    test_history_matches(searcher, expected, __LINE__);

    // Items exactly matching "alph", case-insensitive.
    searcher = history_search_t(history, L"alph", history_search_type_t::exact, nocase);
    set_expected([](const wcstring &s) { return wcstolower(s) == L"alph"; });
    test_history_matches(searcher, expected, __LINE__);

    // Test item removal case-sensitive.
    searcher = history_search_t(history, L"Alpha");
    test_history_matches(searcher, {L"Alpha"}, __LINE__);
    history->remove(L"Alpha");
    searcher = history_search_t(history, L"Alpha");
    test_history_matches(searcher, {}, __LINE__);

    // Test history escaping and unescaping, yaml, etc.
    history_item_list_t before, after;
    history->clear();
    size_t i, max = 100;
    for (i = 1; i <= max; i++) {
        // Generate a value.
        wcstring value = wcstring(L"test item ") + to_string(i);

        // Maybe add some backslashes.
        if (i % 3 == 0) value.append(L"(slashies \\\\\\ slashies)");

        // Generate some paths.
        path_list_t paths;
        size_t count = random() % 6;
        while (count--) {
            paths.push_back(random_string());
        }

        // Record this item.
        history_item_t item(value, time(nullptr));
        item.required_paths = paths;
        before.push_back(item);
        history->add(std::move(item));
    }
    history->save();

    // Empty items should just be dropped (#6032).
    history->add(L"");
    do_test(!history->item_at_index(1).contents.empty());

    // Read items back in reverse order and ensure they're the same.
    for (i = 100; i >= 1; i--) {
        history_item_t item = history->item_at_index(i);
        do_test(!item.empty());
        after.push_back(item);
    }
    do_test(before.size() == after.size());
    for (size_t i = 0; i < before.size(); i++) {
        const history_item_t &bef = before.at(i), &aft = after.at(i);
        do_test(bef.contents == aft.contents);
        do_test(bef.creation_timestamp == aft.creation_timestamp);
        do_test(bef.required_paths == aft.required_paths);
    }

    // Clean up after our tests.
    history->clear();
}
// Wait until the next second.
static void time_barrier() {
    time_t start = time(nullptr);
    do {
        usleep(1000);
    } while (time(nullptr) == start);
}

static wcstring_list_t generate_history_lines(size_t item_count, size_t idx) {
    wcstring_list_t result;
    result.reserve(item_count);
    for (unsigned long i = 0; i < item_count; i++) {
        result.push_back(format_string(L"%ld %lu", (unsigned long)idx, (unsigned long)i));
    }
    return result;
}

void history_tests_t::test_history_races_pound_on_history(size_t item_count, size_t idx) {
    // Called in child thread to modify history.
    history_t hist(L"race_test");
    const wcstring_list_t hist_lines = generate_history_lines(item_count, idx);
    for (const wcstring &line : hist_lines) {
        hist.add(line);
        hist.save();
    }
}

void history_tests_t::test_history_races() {
    // This always fails under WSL
    if (is_windows_subsystem_for_linux()) {
        return;
    }

    say(L"Testing history race conditions");

    // It appears TSAN and ASAN's allocators do not release their locks properly in atfork, so
    // allocating with multiple threads risks deadlock. Drain threads before running under ASAN.
    // TODO: stop forking with these tests.
    bool needs_thread_drain = false;
#if __SANITIZE_ADDRESS__
    needs_thread_drain |= true;
#endif
#if defined(__has_feature)
    needs_thread_drain |= __has_feature(thread_sanitizer) || __has_feature(address_sanitizer);
#endif

    if (needs_thread_drain) {
        iothread_drain_all();
    }

    // Test concurrent history writing.
    // How many concurrent writers we have
    constexpr size_t RACE_COUNT = 4;

    // How many items each writer makes
    constexpr size_t ITEM_COUNT = 256;

    // Ensure history is clear.
    history_t(L"race_test").clear();

    // hist.chaos_mode = true;

    std::thread children[RACE_COUNT];
    for (size_t i = 0; i < RACE_COUNT; i++) {
        children[i] = std::thread([=] { test_history_races_pound_on_history(ITEM_COUNT, i); });
    }

    // Wait for all children.
    for (std::thread &child : children) {
        child.join();
    }

    // Compute the expected lines.
    std::array<wcstring_list_t, RACE_COUNT> expected_lines;
    for (size_t i = 0; i < RACE_COUNT; i++) {
        expected_lines[i] = generate_history_lines(ITEM_COUNT, i);
    }

    // Ensure we consider the lines that have been outputted as part of our history.
    time_barrier();

    // Ensure that we got sane, sorted results.
    history_t hist(L"race_test");
    hist.chaos_mode = !true;

    // History is enumerated from most recent to least
    // Every item should be the last item in some array
    size_t hist_idx;
    for (hist_idx = 1;; hist_idx++) {
        history_item_t item = hist.item_at_index(hist_idx);
        if (item.empty()) break;

        bool found = false;
        for (wcstring_list_t &list : expected_lines) {
            auto iter = std::find(list.begin(), list.end(), item.contents);
            if (iter != list.end()) {
                found = true;

                // Remove everything from this item on
                auto cursor = list.end();
                if (cursor + 1 != list.end()) {
                    while (--cursor != iter) {
                        err(L"Item dropped from history: %ls", cursor->c_str());
                    }
                }
                list.erase(iter, list.end());
                break;
            }
        }
        if (!found) {
            err(L"Line '%ls' found in history, but not found in some array", item.str().c_str());
            for (wcstring_list_t &list : expected_lines) {
                if (!list.empty()) {
                    fprintf(stderr, "\tRemaining: %ls\n", list.back().c_str());
                }
            }
        }
    }

    // +1 to account for history's 1-based offset
    size_t expected_idx = RACE_COUNT * ITEM_COUNT + 1;
    if (hist_idx != expected_idx) {
        err(L"Expected %lu items, but instead got %lu items", expected_idx, hist_idx);
    }

    // See if anything is left in the arrays
    for (const wcstring_list_t &list : expected_lines) {
        for (const wcstring &str : list) {
            err(L"Line '%ls' still left in the array", str.c_str());
        }
    }
    hist.clear();
}

void history_tests_t::test_history_merge() {
    // In a single fish process, only one history is allowed to exist with the given name But it's
    // common to have multiple history instances with the same name active in different processes,
    // e.g. when you have multiple shells open. We try to get that right and merge all their history
    // together. Test that case.
    say(L"Testing history merge");
    const size_t count = 3;
    const wcstring name = L"merge_test";
    std::shared_ptr<history_t> hists[count] = {std::make_shared<history_t>(name),
                                               std::make_shared<history_t>(name),
                                               std::make_shared<history_t>(name)};
    const wcstring texts[count] = {L"History 1", L"History 2", L"History 3"};
    const wcstring alt_texts[count] = {L"History Alt 1", L"History Alt 2", L"History Alt 3"};

    // Make sure history is clear.
    for (auto &hist : hists) {
        hist->clear();
    }

    // Make sure we don't add an item in the same second as we created the history.
    time_barrier();

    // Add a different item to each.
    for (size_t i = 0; i < count; i++) {
        hists[i]->add(texts[i]);
    }

    // Save them.
    for (auto &hist : hists) {
        hist->save();
    }

    // Make sure each history contains what it ought to, but they have not leaked into each other.
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) {
            bool does_contain = history_contains(hists[i], texts[j]);
            bool should_contain = (i == j);
            do_test(should_contain == does_contain);
        }
    }

    // Make a new history. It should contain everything. The time_barrier() is so that the timestamp
    // is newer, since we only pick up items whose timestamp is before the birth stamp.
    time_barrier();
    std::shared_ptr<history_t> everything = std::make_shared<history_t>(name);
    for (const auto &text : texts) {
        do_test(history_contains(everything, text));
    }

    // Tell all histories to merge. Now everybody should have everything.
    for (auto &hist : hists) {
        hist->incorporate_external_changes();
    }

    // Everyone should also have items in the same order (#2312)
    wcstring_list_t hist_vals1;
    hists[0]->get_history(hist_vals1);
    for (const auto &hist : hists) {
        wcstring_list_t hist_vals2;
        hist->get_history(hist_vals2);
        do_test(hist_vals1 == hist_vals2);
    }

    // Add some more per-history items.
    for (size_t i = 0; i < count; i++) {
        hists[i]->add(alt_texts[i]);
    }
    // Everybody should have old items, but only one history should have each new item.
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) {
            // Old item.
            do_test(history_contains(hists[i], texts[j]));

            // New item.
            bool does_contain = history_contains(hists[i], alt_texts[j]);
            bool should_contain = (i == j);
            do_test(should_contain == does_contain);
        }
    }

    // Make sure incorporate_external_changes doesn't drop items! (#3496)
    history_t *const writer = hists[0].get();
    history_t *const reader = hists[1].get();
    const wcstring more_texts[] = {L"Item_#3496_1", L"Item_#3496_2", L"Item_#3496_3",
                                   L"Item_#3496_4", L"Item_#3496_5", L"Item_#3496_6"};
    for (size_t i = 0; i < sizeof more_texts / sizeof *more_texts; i++) {
        // time_barrier because merging will ignore items that may be newer
        if (i > 0) time_barrier();
        writer->add(more_texts[i]);
        writer->incorporate_external_changes();
        reader->incorporate_external_changes();
        for (size_t j = 0; j < i; j++) {
            do_test(history_contains(reader, more_texts[j]));
        }
    }
    everything->clear();
}

void history_tests_t::test_history_path_detection() {
    // Regression test for #7582.
    say(L"Testing history path detection");
    char tmpdirbuff[] = "/tmp/fish_test_history.XXXXXX";
    wcstring tmpdir = str2wcstring(mkdtemp(tmpdirbuff));
    if (!string_suffixes_string(L"/", tmpdir)) {
        tmpdir.push_back(L'/');
    }

    // Place one valid file in the directory.
    wcstring filename = L"testfile";
    std::string path = wcs2string(tmpdir + filename);
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        err(L"Failed to open test file from history path detection");
        return;
    }
    fclose(f);

    std::shared_ptr<test_environment_t> vars = std::make_shared<test_environment_t>();
    vars->vars[L"PWD"] = tmpdir;
    vars->vars[L"HOME"] = tmpdir;

    std::shared_ptr<history_t> history = history_t::with_name(L"path_detection");
    history_t::add_pending_with_file_detection(history, L"cmd0 not/a/valid/path", vars);
    history_t::add_pending_with_file_detection(history, L"cmd1 " + filename, vars);
    history_t::add_pending_with_file_detection(history, L"cmd2 " + tmpdir + L"/" + filename, vars);
    history_t::add_pending_with_file_detection(history, L"cmd3  $HOME/" + filename, vars);
    history_t::add_pending_with_file_detection(history, L"cmd4  $HOME/notafile", vars);
    history_t::add_pending_with_file_detection(history, L"cmd5  ~/" + filename, vars);
    history_t::add_pending_with_file_detection(history, L"cmd6  ~/notafile", vars);
    history_t::add_pending_with_file_detection(history, L"cmd7  ~/*f*", vars);
    history_t::add_pending_with_file_detection(history, L"cmd8  ~/*zzz*", vars);
    history->resolve_pending();

    constexpr size_t hist_size = 9;
    if (history->size() != hist_size) {
        err(L"history has wrong size: %lu but expected %lu", (unsigned long)history->size(),
            (unsigned long)hist_size);
        history->clear();
        return;
    }

    // Expected sets of paths.
    wcstring_list_t expected[hist_size] = {
        {},                          // cmd0
        {filename},                  // cmd1
        {tmpdir + L"/" + filename},  // cmd2
        {L"$HOME/" + filename},      // cmd3
        {},                          // cmd4
        {L"~/" + filename},          // cmd5
        {},                          // cmd6
        {},                          // cmd7 - we do not expand globs
        {},                          // cmd8
    };

    size_t lap;
    const size_t maxlap = 128;
    for (lap = 0; lap < maxlap; lap++) {
        int failures = 0;
        bool last = (lap + 1 == maxlap);
        for (size_t i = 1; i <= hist_size; i++) {
            if (history->item_at_index(i).required_paths != expected[hist_size - i]) {
                failures += 1;
                if (last) {
                    err(L"Wrong detected paths for item %lu", (unsigned long)i);
                }
            }
        }
        if (failures == 0) {
            break;
        }
        // The file detection takes a little time since it occurs in the background.
        // Loop until the test passes.
        usleep(1E6 / 500);  // 1 msec
    }
    // fprintf(stderr, "History saving took %lu laps\n", (unsigned long)lap);
    history->clear();
}

static bool install_sample_history(const wchar_t *name) {
    wcstring path;
    if (!path_get_data(path)) {
        err(L"Failed to get data directory");
        return false;
    }
    char command[512];
    snprintf(command, sizeof command, "cp tests/%ls %ls/%ls_history", name, path.c_str(), name);
    if (system(command)) {
        err(L"Failed to copy sample history");
        return false;
    }
    return true;
}

/// Indicates whether the history is equal to the given null-terminated array of strings.
static bool history_equals(const shared_ptr<history_t> &hist, const wchar_t *const *strings) {
    // Count our expected items.
    size_t expected_count = 0;
    while (strings[expected_count]) {
        expected_count++;
    }

    // Ensure the contents are the same.
    size_t history_idx = 1;
    size_t array_idx = 0;
    for (;;) {
        const wchar_t *expected = strings[array_idx];
        history_item_t item = hist->item_at_index(history_idx);
        if (expected == nullptr) {
            if (!item.empty()) {
                err(L"Expected empty item at history index %lu, instead found: %ls", history_idx,
                    item.str().c_str());
            }
            break;
        } else {
            if (item.str() != expected) {
                err(L"Expected '%ls', found '%ls' at index %lu", expected, item.str().c_str(),
                    history_idx);
            }
        }
        history_idx++;
        array_idx++;
    }

    return true;
}

void history_tests_t::test_history_formats() {
    const wchar_t *name;

    // Test inferring and reading legacy and bash history formats.
    name = L"history_sample_fish_1_x";
    say(L"Testing %ls", name);
    if (!install_sample_history(name)) {
        err(L"Couldn't open file tests/%ls", name);
    } else {
        // Note: This is backwards from what appears in the file.
        const wchar_t *const expected[] = {
            L"#def", L"echo #abc", L"function yay\necho hi\nend", L"cd foobar", L"ls /", nullptr};

        auto test_history = history_t::with_name(name);
        if (!history_equals(test_history, expected)) {
            err(L"test_history_formats failed for %ls\n", name);
        }
        test_history->clear();
    }

    name = L"history_sample_fish_2_0";
    say(L"Testing %ls", name);
    if (!install_sample_history(name)) {
        err(L"Couldn't open file tests/%ls", name);
    } else {
        const wchar_t *const expected[] = {L"echo this has\\\nbackslashes",
                                           L"function foo\necho bar\nend", L"echo alpha", nullptr};

        auto test_history = history_t::with_name(name);
        if (!history_equals(test_history, expected)) {
            err(L"test_history_formats failed for %ls\n", name);
        }
        test_history->clear();
    }

    say(L"Testing bash import");
    FILE *f = fopen("tests/history_sample_bash", "r");
    if (!f) {
        err(L"Couldn't open file tests/history_sample_bash");
    } else {
        // The results are in the reverse order that they appear in the bash history file.
        // We don't expect whitespace to be elided (#4908: except for leading/trailing whitespace)
        const wchar_t *expected[] = {L"EOF",
                                     L"sleep 123",
                                     L"posix_cmd_sub $(is supported but only splits on newlines)",
                                     L"posix_cmd_sub \"$(is supported)\"",
                                     L"a && echo valid construct",
                                     L"final line",
                                     L"echo supsup",
                                     L"export XVAR='exported'",
                                     L"history --help",
                                     L"echo foo",
                                     nullptr};
        auto test_history = history_t::with_name(L"bash_import");
        test_history->populate_from_bash(f);
        if (!history_equals(test_history, expected)) {
            err(L"test_history_formats failed for bash import\n");
        }
        test_history->clear();
        fclose(f);
    }

    name = L"history_sample_corrupt1";
    say(L"Testing %ls", name);
    if (!install_sample_history(name)) {
        err(L"Couldn't open file tests/%ls", name);
    } else {
        // We simply invoke get_string_representation. If we don't die, the test is a success.
        auto test_history = history_t::with_name(name);
        const wchar_t *expected[] = {L"no_newline_at_end_of_file", L"corrupt_prefix",
                                     L"this_command_is_ok", nullptr};
        if (!history_equals(test_history, expected)) {
            err(L"test_history_formats failed for %ls\n", name);
        }
        test_history->clear();
    }
}

#if 0
// This test isn't run at this time. It was added by commit b9283d48 but not actually enabled.
void history_tests_t::test_history_speed(void)
{
    say(L"Testing history speed (pid is %d)", getpid());
    std::unique_ptr<history_t> hist = make_unique<history_t>(L"speed_test");
    wcstring item = L"History Speed Test - X";

    // Test for 10 seconds.
    double start = timef();
    double end = start + 10;
    double stop = 0;
    size_t count = 0;
    for (;;)
    {
        item[item.size() - 1] = L'0' + (count % 10);
        hist->add(item);
        count++;

        stop = timef();
        if (stop >= end)
            break;
    }
    std::fwprintf(stdout, L"%lu items - %.2f msec per item\n", (unsigned long)count,
             (stop - start) * 1E6 / count);
    hist->clear();
}
#endif

static void test_new_parser_correctness() {
    say(L"Testing parser correctness");
    const struct parser_test_t {
        const wchar_t *src;
        bool ok;
    } parser_tests[] = {
        {L"; ; ; ", true},
        {L"if ; end", false},
        {L"if true ; end", true},
        {L"if true; end ; end", false},
        {L"if end; end ; end", false},
        {L"if end", false},
        {L"end", false},
        {L"for i i", false},
        {L"for i in a b c ; end", true},
        {L"begin end", true},
        {L"begin; end", true},
        {L"begin if true; end; end;", true},
        {L"begin if true ; echo hi ; end; end", true},
        {L"true && false || false", true},
        {L"true || false; and true", true},
        {L"true || ||", false},
        {L"|| true", false},
        {L"true || \n\n false", true},
    };

    for (const auto &test : parser_tests) {
        auto ast = ast::ast_t::parse(test.src);
        bool success = !ast.errored();
        if (success && !test.ok) {
            err(L"\"%ls\" should NOT have parsed, but did", test.src);
        } else if (!success && test.ok) {
            err(L"\"%ls\" should have parsed, but failed", test.src);
        }
    }
    say(L"Parse tests complete");
}

// Given that we have an array of 'fuzz_count' strings, we wish to enumerate all permutations of
// 'len' values. We do this by incrementing an integer, interpreting it as "base fuzz_count".
static inline bool string_for_permutation(const wcstring *fuzzes, size_t fuzz_count, size_t len,
                                          size_t permutation, wcstring *out_str) {
    out_str->clear();

    size_t remaining_permutation = permutation;
    for (size_t i = 0; i < len; i++) {
        size_t idx = remaining_permutation % fuzz_count;
        remaining_permutation /= fuzz_count;

        out_str->append(fuzzes[idx]);
        out_str->push_back(L' ');
    }
    // Return false if we wrapped.
    return remaining_permutation == 0;
}

static void test_new_parser_fuzzing() {
    say(L"Fuzzing parser");
    const wcstring fuzzes[] = {
        L"if",      L"else", L"for", L"in",  L"while", L"begin", L"function",
        L"switch",  L"case", L"end", L"and", L"or",    L"not",   L"command",
        L"builtin", L"foo",  L"|",   L"^",   L"&",     L";",
    };

    // Generate a list of strings of all keyword / token combinations.
    wcstring src;
    src.reserve(128);

    parse_error_list_t errors;

    double start = timef();
    bool log_it = true;
    unsigned long max_len = 5;
    for (unsigned long len = 0; len < max_len; len++) {
        if (log_it) std::fwprintf(stderr, L"%lu / %lu...", len, max_len);

        // We wish to look at all permutations of 4 elements of 'fuzzes' (with replacement).
        // Construct an int and keep incrementing it.
        unsigned long permutation = 0;
        while (string_for_permutation(fuzzes, sizeof fuzzes / sizeof *fuzzes, len, permutation++,
                                      &src)) {
            ast::ast_t::parse(src);
        }
        if (log_it) std::fwprintf(stderr, L"done (%lu)\n", permutation);
    }
    double end = timef();
    if (log_it) say(L"All fuzzed in %f seconds!", end - start);
}

// Parse a statement, returning the command, args (joined by spaces), and the decoration. Returns
// true if successful.
static bool test_1_parse_ll2(const wcstring &src, wcstring *out_cmd, wcstring *out_joined_args,
                             enum statement_decoration_t *out_deco) {
    using namespace ast;
    out_cmd->clear();
    out_joined_args->clear();
    *out_deco = statement_decoration_t::none;

    auto ast = ast_t::parse(src);
    if (ast.errored()) return false;

    // Get the statement. Should only have one.
    const decorated_statement_t *statement = nullptr;
    for (const auto &n : ast) {
        if (const auto *tmp = n.try_as<decorated_statement_t>()) {
            if (statement) {
                say(L"More than one decorated statement found in '%ls'", src.c_str());
                return false;
            }
            statement = tmp;
        }
    }
    if (!statement) {
        say(L"No decorated statement found in '%ls'", src.c_str());
        return false;
    }

    // Return its decoration and command.
    *out_deco = statement->decoration();
    *out_cmd = statement->command.source(src);

    // Return arguments separated by spaces.
    bool first = true;
    for (const ast::argument_or_redirection_t &arg : statement->args_or_redirs) {
        if (!arg.is_argument()) continue;
        if (!first) out_joined_args->push_back(L' ');
        out_joined_args->append(arg.source(src));
        first = false;
    }

    return true;
}

// Verify that 'function -h' and 'function --help' are plain statements but 'function --foo' is
// not (issue #1240).
template <ast::type_t Type>
static void check_function_help(const wchar_t *src) {
    using namespace ast;
    auto ast = ast_t::parse(src);
    if (ast.errored()) {
        err(L"Failed to parse '%ls'", src);
    }

    int count = 0;
    for (const node_t &node : ast) {
        count += (node.type == Type);
    }
    if (count == 0) {
        err(L"Failed to find node of type '%ls'", ast_type_to_string(Type));
    } else if (count > 1) {
        err(L"Found too many nodes of type '%ls'", ast_type_to_string(Type));
    }
}

// Test the LL2 (two token lookahead) nature of the parser by exercising the special builtin and
// command handling. In particular, 'command foo' should be a decorated statement 'foo' but 'command
// -help' should be an undecorated statement 'command' with argument '--help', and NOT attempt to
// run a command called '--help'.
static void test_new_parser_ll2() {
    say(L"Testing parser two-token lookahead");

    const struct {
        wcstring src;
        wcstring cmd;
        wcstring args;
        enum statement_decoration_t deco;
    } tests[] = {{L"echo hello", L"echo", L"hello", statement_decoration_t::none},
                 {L"command echo hello", L"echo", L"hello", statement_decoration_t::command},
                 {L"exec echo hello", L"echo", L"hello", statement_decoration_t::exec},
                 {L"command command hello", L"command", L"hello", statement_decoration_t::command},
                 {L"builtin command hello", L"command", L"hello", statement_decoration_t::builtin},
                 {L"command --help", L"command", L"--help", statement_decoration_t::none},
                 {L"command -h", L"command", L"-h", statement_decoration_t::none},
                 {L"command", L"command", L"", statement_decoration_t::none},
                 {L"command -", L"command", L"-", statement_decoration_t::none},
                 {L"command --", L"command", L"--", statement_decoration_t::none},
                 {L"builtin --names", L"builtin", L"--names", statement_decoration_t::none},
                 {L"function", L"function", L"", statement_decoration_t::none},
                 {L"function --help", L"function", L"--help", statement_decoration_t::none}};

    for (const auto &test : tests) {
        wcstring cmd, args;
        enum statement_decoration_t deco = statement_decoration_t::none;
        bool success = test_1_parse_ll2(test.src, &cmd, &args, &deco);
        if (!success) err(L"Parse of '%ls' failed on line %ld", test.cmd.c_str(), (long)__LINE__);
        if (cmd != test.cmd)
            err(L"When parsing '%ls', expected command '%ls' but got '%ls' on line %ld",
                test.src.c_str(), test.cmd.c_str(), cmd.c_str(), (long)__LINE__);
        if (args != test.args)
            err(L"When parsing '%ls', expected args '%ls' but got '%ls' on line %ld",
                test.src.c_str(), test.args.c_str(), args.c_str(), (long)__LINE__);
        if (deco != test.deco)
            err(L"When parsing '%ls', expected decoration %d but got %d on line %ld",
                test.src.c_str(), (int)test.deco, (int)deco, (long)__LINE__);
    }

    check_function_help<ast::type_t::decorated_statement>(L"function -h");
    check_function_help<ast::type_t::decorated_statement>(L"function --help");
    check_function_help<ast::type_t::function_header>(L"function --foo; end");
    check_function_help<ast::type_t::function_header>(L"function foo; end");
}

static void test_new_parser_ad_hoc() {
    using namespace ast;
    // Very ad-hoc tests for issues encountered.
    say(L"Testing new parser ad hoc tests");

    // Ensure that 'case' terminates a job list.
    const wcstring src = L"switch foo ; case bar; case baz; end";
    auto ast = ast_t::parse(src);
    if (ast.errored()) {
        err(L"Parsing failed");
    }

    // Expect two case_item_lists. The bug was that we'd
    // try to run a command 'case'.
    int count = 0;
    for (const auto &n : ast) {
        count += (n.type == type_t::case_item);
    }
    if (count != 2) {
        err(L"Expected 2 case item nodes, found %d", count);
    }

    // Ensure that naked variable assignments don't hang.
    // The bug was that "a=" would produce an error but not be consumed,
    // leading to an infinite loop.

    // By itself it should produce an error.
    ast = ast_t::parse(L"a=");
    do_test(ast.errored());

    // If we are leaving things unterminated, this should not produce an error.
    // i.e. when typing "a=" at the command line, it should be treated as valid
    // because we don't want to color it as an error.
    ast = ast_t::parse(L"a=", parse_flag_leave_unterminated);
    do_test(!ast.errored());

    parse_error_list_t errors;
    ast = ast_t::parse(L"begin; echo (", parse_flag_leave_unterminated, &errors);
    do_test(errors.size() == 1 && errors.at(0).code == parse_error_tokenizer_unterminated_subshell);

    errors.clear();
    ast = ast_t::parse(L"for x in (", parse_flag_leave_unterminated, &errors);
    do_test(errors.size() == 1 && errors.at(0).code == parse_error_tokenizer_unterminated_subshell);

    errors.clear();
    ast = ast_t::parse(L"begin; echo '", parse_flag_leave_unterminated, &errors);
    do_test(errors.size() == 1 && errors.at(0).code == parse_error_tokenizer_unterminated_quote);
}

static void test_new_parser_errors() {
    say(L"Testing new parser error reporting");
    const struct {
        const wchar_t *src;
        parse_error_code_t code;
    } tests[] = {
        {L"echo 'abc", parse_error_tokenizer_unterminated_quote},
        {L"'", parse_error_tokenizer_unterminated_quote},
        {L"echo (abc", parse_error_tokenizer_unterminated_subshell},

        {L"end", parse_error_unbalancing_end},
        {L"echo hi ; end", parse_error_unbalancing_end},

        {L"else", parse_error_unbalancing_else},
        {L"if true ; end ; else", parse_error_unbalancing_else},

        {L"case", parse_error_unbalancing_case},
        {L"if true ; case ; end", parse_error_generic},

        {L"true | and", parse_error_andor_in_pipeline},

        {L"a=", parse_error_bare_variable_assignment},
    };

    for (const auto &test : tests) {
        const wcstring src = test.src;
        parse_error_code_t expected_code = test.code;

        parse_error_list_t errors;
        auto ast = ast::ast_t::parse(src, parse_flag_none, &errors);
        if (!ast.errored()) {
            err(L"Source '%ls' was expected to fail to parse, but succeeded", src.c_str());
        }

        if (errors.size() != 1) {
            err(L"Source '%ls' was expected to produce 1 error, but instead produced %lu errors",
                src.c_str(), errors.size());
            for (const auto &err : errors) {
                fprintf(stderr, "%ls\n", err.describe(src, false).c_str());
            }
        } else if (errors.at(0).code != expected_code) {
            err(L"Source '%ls' was expected to produce error code %lu, but instead produced error "
                L"code %lu",
                src.c_str(), expected_code, (unsigned long)errors.at(0).code);
            for (const auto &error : errors) {
                err(L"\t\t%ls", error.describe(src, true).c_str());
            }
        }
    }
}

// Given a format string, returns a list of non-empty strings separated by format specifiers. The
// format specifiers themselves are omitted.
static wcstring_list_t separate_by_format_specifiers(const wchar_t *format) {
    wcstring_list_t result;
    const wchar_t *cursor = format;
    const wchar_t *end = format + std::wcslen(format);
    while (cursor < end) {
        const wchar_t *next_specifier = std::wcschr(cursor, '%');
        if (next_specifier == nullptr) {
            next_specifier = end;
        }
        assert(next_specifier != nullptr);

        // Don't return empty strings.
        if (next_specifier > cursor) {
            result.emplace_back(cursor, next_specifier - cursor);
        }

        // Walk over the format specifier (if any).
        cursor = next_specifier;
        if (*cursor != '%') {
            continue;
        }

        cursor++;
        // Flag
        if (std::wcschr(L"#0- +'", *cursor)) cursor++;
        // Minimum field width
        while (iswdigit(*cursor)) cursor++;
        // Precision
        if (*cursor == L'.') {
            cursor++;
            while (iswdigit(*cursor)) cursor++;
        }
        // Length modifier
        if (!std::wcsncmp(cursor, L"ll", 2) || !std::wcsncmp(cursor, L"hh", 2)) {
            cursor += 2;
        } else if (std::wcschr(L"hljtzqL", *cursor)) {
            cursor++;
        }
        // The format specifier itself. We allow any character except NUL.
        if (*cursor != L'\0') {
            cursor += 1;
        }
        assert(cursor <= end);
    }
    return result;
}

// Given a format string 'format', return true if the string may have been produced by that format
// string. We do this by splitting the format string around the format specifiers, and then ensuring
// that each of the remaining chunks is found (in order) in the string.
static bool string_matches_format(const wcstring &string, const wchar_t *format) {
    bool result = true;
    wcstring_list_t components = separate_by_format_specifiers(format);
    size_t idx = 0;
    for (const auto &component : components) {
        size_t where = string.find(component, idx);
        if (where == wcstring::npos) {
            result = false;
            break;
        }
        idx = where + component.size();
        assert(idx <= string.size());
    }
    return result;
}

static void test_error_messages() {
    say(L"Testing error messages");
    const struct error_test_t {
        const wchar_t *src;
        const wchar_t *error_text_format;
    } error_tests[] = {{L"echo $^", ERROR_BAD_VAR_CHAR1},
                       {L"echo foo${a}bar", ERROR_BRACKETED_VARIABLE1},
                       {L"echo foo\"${a}\"bar", ERROR_BRACKETED_VARIABLE_QUOTED1},
                       {L"echo foo\"${\"bar", ERROR_BAD_VAR_CHAR1},
                       {L"echo $?", ERROR_NOT_STATUS},
                       {L"echo $$", ERROR_NOT_PID},
                       {L"echo $#", ERROR_NOT_ARGV_COUNT},
                       {L"echo $@", ERROR_NOT_ARGV_AT},
                       {L"echo $*", ERROR_NOT_ARGV_STAR},
                       {L"echo $", ERROR_NO_VAR_NAME},
                       {L"echo foo\"$\"bar", ERROR_NO_VAR_NAME},
                       {L"echo \"foo\"$\"bar\"", ERROR_NO_VAR_NAME},
                       {L"echo foo $ bar", ERROR_NO_VAR_NAME}};

    parse_error_list_t errors;
    for (const auto &test : error_tests) {
        errors.clear();
        parse_util_detect_errors(test.src, &errors);
        do_test(!errors.empty());
        if (!errors.empty()) {
            do_test1(string_matches_format(errors.at(0).text, test.error_text_format), test.src);
        }
    }
}

static void test_highlighting() {
    say(L"Testing syntax highlighting");
    if (!pushd("test/fish_highlight_test/")) return;
    cleanup_t pop{[] { popd(); }};
    if (system("mkdir -p dir")) err(L"mkdir failed");
    if (system("touch foo")) err(L"touch failed");
    if (system("touch bar")) err(L"touch failed");

    // Here are the components of our source and the colors we expect those to be.
    struct highlight_component_t {
        const wchar_t *txt;
        highlight_spec_t color;
        bool nospace;
        highlight_component_t(const wchar_t *txt, highlight_spec_t color, bool nospace = false)
            : txt(txt), color(color), nospace(nospace) {}
    };
    const bool ns = true;

    using highlight_component_list_t = std::vector<highlight_component_t>;
    std::vector<highlight_component_list_t> highlight_tests;

    highlight_spec_t param_valid_path{highlight_role_t::param};
    param_valid_path.valid_path = true;

    highlight_tests.push_back({{L"echo", highlight_role_t::command},
                               {L"./foo", param_valid_path},
                               {L"&", highlight_role_t::statement_terminator}});

    highlight_tests.push_back({
        {L"command", highlight_role_t::keyword},
        {L"echo", highlight_role_t::command},
        {L"abc", highlight_role_t::param},
        {L"foo", param_valid_path},
        {L"&", highlight_role_t::statement_terminator},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"foo&bar", highlight_role_t::param},
        {L"foo", highlight_role_t::param, /*nospace=*/true},
        {L"&", highlight_role_t::statement_terminator},
        {L"echo", highlight_role_t::command},
        {L"&>", highlight_role_t::redirection},
    });

    highlight_tests.push_back({
        {L"if command", highlight_role_t::keyword},
        {L"ls", highlight_role_t::command},
        {L"; ", highlight_role_t::statement_terminator},
        {L"echo", highlight_role_t::command},
        {L"abc", highlight_role_t::param},
        {L"; ", highlight_role_t::statement_terminator},
        {L"/bin/definitely_not_a_command", highlight_role_t::error},
        {L"; ", highlight_role_t::statement_terminator},
        {L"end", highlight_role_t::keyword},
    });

    // Verify that cd shows errors for non-directories.
    highlight_tests.push_back({
        {L"cd", highlight_role_t::command},
        {L"dir", param_valid_path},
    });

    highlight_tests.push_back({
        {L"cd", highlight_role_t::command},
        {L"foo", highlight_role_t::error},
    });

    highlight_tests.push_back({
        {L"cd", highlight_role_t::command},
        {L"--help", highlight_role_t::option},
        {L"-h", highlight_role_t::option},
        {L"definitely_not_a_directory", highlight_role_t::error},
    });

    // Command substitutions.
    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"param1", highlight_role_t::param},
        {L"-l", highlight_role_t::option},
        {L"--", highlight_role_t::option},
        {L"-l", highlight_role_t::param},
        {L"(", highlight_role_t::operat},
        {L"ls", highlight_role_t::command},
        {L"-l", highlight_role_t::option},
        {L"--", highlight_role_t::option},
        {L"-l", highlight_role_t::param},
        {L"param2", highlight_role_t::param},
        {L")", highlight_role_t::operat},
        {L"|", highlight_role_t::statement_terminator},
        {L"cat", highlight_role_t::command},
    });
    highlight_tests.push_back({
        {L"true", highlight_role_t::command},
        {L"$(", highlight_role_t::operat},
        {L"true", highlight_role_t::command},
        {L")", highlight_role_t::operat},
    });
    highlight_tests.push_back({
        {L"true", highlight_role_t::command},
        {L"\"before", highlight_role_t::quote},
        {L"$(", highlight_role_t::operat},
        {L"true", highlight_role_t::command},
        {L"param1", highlight_role_t::param},
        {L")", highlight_role_t::operat},
        {L"after\"", highlight_role_t::quote},
        {L"param2", highlight_role_t::param},
    });
    highlight_tests.push_back({
        {L"true", highlight_role_t::command},
        {L"\"", highlight_role_t::error},
        {L"unclosed quote", highlight_role_t::quote},
        {L"$(", highlight_role_t::operat},
        {L"true", highlight_role_t::command},
        {L")", highlight_role_t::operat},
    });

    // Redirections substitutions.
    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"param1", highlight_role_t::param},

        // Input redirection.
        {L"<", highlight_role_t::redirection},
        {L"/bin/echo", highlight_role_t::redirection},

        // Output redirection to a valid fd.
        {L"1>&2", highlight_role_t::redirection},

        // Output redirection to an invalid fd.
        {L"2>&", highlight_role_t::redirection},
        {L"LOL", highlight_role_t::error},

        // Just a param, not a redirection.
        {L"test/blah", highlight_role_t::param},

        // Input redirection from directory.
        {L"<", highlight_role_t::redirection},
        {L"test/", highlight_role_t::error},

        // Output redirection to an invalid path.
        {L"3>", highlight_role_t::redirection},
        {L"/not/a/valid/path/nope", highlight_role_t::error},

        // Output redirection to directory.
        {L"3>", highlight_role_t::redirection},
        {L"test/nope/", highlight_role_t::error},

        // Redirections to overflow fd.
        {L"99999999999999999999>&2", highlight_role_t::error},
        {L"2>&", highlight_role_t::redirection},
        {L"99999999999999999999", highlight_role_t::error},

        // Output redirection containing a command substitution.
        {L"4>", highlight_role_t::redirection},
        {L"(", highlight_role_t::operat},
        {L"echo", highlight_role_t::command},
        {L"test/somewhere", highlight_role_t::param},
        {L")", highlight_role_t::operat},

        // Just another param.
        {L"param2", highlight_role_t::param},
    });

    highlight_tests.push_back({
        {L"for", highlight_role_t::keyword},
        {L"x", highlight_role_t::param},
        {L"in", highlight_role_t::keyword},
        {L"set-by-for-1", highlight_role_t::param},
        {L"set-by-for-2", highlight_role_t::param},
        {L";", highlight_role_t::statement_terminator},
        {L"echo", highlight_role_t::command},
        {L">", highlight_role_t::redirection},
        {L"$x", highlight_role_t::redirection},
        {L";", highlight_role_t::statement_terminator},
        {L"end", highlight_role_t::keyword},
    });

    highlight_tests.push_back({
        {L"set", highlight_role_t::command},
        {L"x", highlight_role_t::param},
        {L"set-by-set", highlight_role_t::param},
        {L";", highlight_role_t::statement_terminator},
        {L"echo", highlight_role_t::command},
        {L">", highlight_role_t::redirection},
        {L"$x", highlight_role_t::redirection},
        {L"2>", highlight_role_t::redirection},
        {L"$totally_not_x", highlight_role_t::error},
        {L"<", highlight_role_t::redirection},
        {L"$x_but_its_an_impostor", highlight_role_t::error},
    });

    highlight_tests.push_back({
        {L"x", highlight_role_t::param, ns},
        {L"=", highlight_role_t::operat, ns},
        {L"set-by-variable-override", highlight_role_t::param, ns},
        {L"echo", highlight_role_t::command},
        {L">", highlight_role_t::redirection},
        {L"$x", highlight_role_t::redirection},
    });

    highlight_tests.push_back({
        {L"end", highlight_role_t::error},
        {L";", highlight_role_t::statement_terminator},
        {L"if", highlight_role_t::keyword},
        {L"end", highlight_role_t::error},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"'", highlight_role_t::error},
        {L"single_quote", highlight_role_t::quote},
        {L"$stuff", highlight_role_t::quote},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"\"", highlight_role_t::error},
        {L"double_quote", highlight_role_t::quote},
        {L"$stuff", highlight_role_t::operat},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"$foo", highlight_role_t::operat},
        {L"\"", highlight_role_t::quote},
        {L"$bar", highlight_role_t::operat},
        {L"\"", highlight_role_t::quote},
        {L"$baz[", highlight_role_t::operat},
        {L"1 2..3", highlight_role_t::param},
        {L"]", highlight_role_t::operat},
    });

    highlight_tests.push_back({
        {L"for", highlight_role_t::keyword},
        {L"i", highlight_role_t::param},
        {L"in", highlight_role_t::keyword},
        {L"1 2 3", highlight_role_t::param},
        {L";", highlight_role_t::statement_terminator},
        {L"end", highlight_role_t::keyword},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"$$foo[", highlight_role_t::operat},
        {L"1", highlight_role_t::param},
        {L"][", highlight_role_t::operat},
        {L"2", highlight_role_t::param},
        {L"]", highlight_role_t::operat},
        {L"[3]", highlight_role_t::param},  // two dollar signs, so last one is not an expansion
    });

    highlight_tests.push_back({
        {L"cat", highlight_role_t::command},
        {L"/dev/null", param_valid_path},
        {L"|", highlight_role_t::statement_terminator},
        // This is bogus, but we used to use "less" here and that doesn't have to be installed.
        {L"cat", highlight_role_t::command},
        {L"2>", highlight_role_t::redirection},
    });

    highlight_tests.push_back({
        {L"if", highlight_role_t::keyword},
        {L"true", highlight_role_t::command},
        {L"&&", highlight_role_t::operat},
        {L"false", highlight_role_t::command},
        {L";", highlight_role_t::statement_terminator},
        {L"or", highlight_role_t::operat},
        {L"false", highlight_role_t::command},
        {L"||", highlight_role_t::operat},
        {L"true", highlight_role_t::command},
        {L";", highlight_role_t::statement_terminator},
        {L"and", highlight_role_t::operat},
        {L"not", highlight_role_t::operat},
        {L"!", highlight_role_t::operat},
        {L"true", highlight_role_t::command},
        {L";", highlight_role_t::statement_terminator},
        {L"end", highlight_role_t::keyword},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"%self", highlight_role_t::operat},
        {L"not%self", highlight_role_t::param},
        {L"self%not", highlight_role_t::param},
    });

    highlight_tests.push_back({
        {L"false", highlight_role_t::command},
        {L"&|", highlight_role_t::statement_terminator},
        {L"true", highlight_role_t::command},
    });

    highlight_tests.push_back({
        {L"HOME", highlight_role_t::param},
        {L"=", highlight_role_t::operat, ns},
        {L".", highlight_role_t::param, ns},
        {L"VAR1", highlight_role_t::param},
        {L"=", highlight_role_t::operat, ns},
        {L"VAL1", highlight_role_t::param, ns},
        {L"VAR", highlight_role_t::param},
        {L"=", highlight_role_t::operat, ns},
        {L"false", highlight_role_t::command},
        {L"|&", highlight_role_t::error},
        {L"true", highlight_role_t::command},
        {L"stuff", highlight_role_t::param},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L")", highlight_role_t::error},
    });

    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"stuff", highlight_role_t::param},
        {L"# comment", highlight_role_t::comment},
    });


    highlight_tests.push_back({
        {L"echo", highlight_role_t::command},
        {L"--", highlight_role_t::option},
        {L"-s", highlight_role_t::param},
    });

    // Overlong paths don't crash (#7837).
    const wcstring overlong = get_overlong_path();
    highlight_tests.push_back({
        {L"touch", highlight_role_t::command},
        {overlong.c_str(), highlight_role_t::param},
    });

    highlight_tests.push_back({
        {L"a", highlight_role_t::param},
        {L"=", highlight_role_t::operat, ns},
    });

    auto &vars = parser_t::principal_parser().vars();
    // Verify variables and wildcards in commands using /bin/cat.
    vars.set(L"VARIABLE_IN_COMMAND", ENV_LOCAL, {L"a"});
    vars.set(L"VARIABLE_IN_COMMAND2", ENV_LOCAL, {L"at"});
    highlight_tests.push_back(
        {{L"/bin/ca", highlight_role_t::command, ns}, {L"*", highlight_role_t::operat, ns}});

    highlight_tests.push_back({{L"/bin/c", highlight_role_t::command, ns},
                               {L"{$VARIABLE_IN_COMMAND}", highlight_role_t::operat, ns},
                               {L"*", highlight_role_t::operat, ns}});

    highlight_tests.push_back({{L"/bin/c", highlight_role_t::command, ns},
                               {L"{$VARIABLE_IN_COMMAND}", highlight_role_t::operat, ns},
                               {L"*", highlight_role_t::operat, ns}});

    highlight_tests.push_back({{L"/bin/c", highlight_role_t::command, ns},
                               {L"$VARIABLE_IN_COMMAND2", highlight_role_t::operat, ns}});

    highlight_tests.push_back({{L"$EMPTY_VARIABLE", highlight_role_t::error}});
    highlight_tests.push_back({{L"\"$EMPTY_VARIABLE\"", highlight_role_t::error}});

    const auto saved_flags = fish_features();
    mutable_fish_features().set(features_t::ampersand_nobg_in_token, true);
    for (const highlight_component_list_t &components : highlight_tests) {
        // Generate the text.
        wcstring text;
        std::vector<highlight_spec_t> expected_colors;
        for (const highlight_component_t &comp : components) {
            if (!text.empty() && !comp.nospace) {
                text.push_back(L' ');
                expected_colors.emplace_back();
            }
            text.append(comp.txt);
            expected_colors.resize(text.size(), comp.color);
        }
        do_test(expected_colors.size() == text.size());

        std::vector<highlight_spec_t> colors(text.size());
        highlight_shell(text, colors, operation_context_t{vars}, true /* io_ok */);

        if (expected_colors.size() != colors.size()) {
            err(L"Color vector has wrong size! Expected %lu, actual %lu", expected_colors.size(),
                colors.size());
        }
        do_test(expected_colors.size() == colors.size());
        for (size_t i = 0; i < text.size(); i++) {
            // Hackish space handling. We don't care about the colors in spaces.
            if (text.at(i) == L' ') continue;

            if (expected_colors.at(i) != colors.at(i)) {
                // Make a fancy caret under the token
                auto e_col = expected_colors.at(i);
                auto a_col = colors.at(i);
                auto j = i + 1;
                while (j < colors.size() && expected_colors.at(j) == e_col && colors.at(j) == a_col) j++;
                if (j == colors.size() - 1) j++;
                const wcstring spaces(i, L' ');
                const wcstring carets(j - i, L'^');
                err(L"Wrong color in test at index %lu-%lu in text (expected %#x, actual "
                    L"%#x):\n%ls\n%ls%ls",
                    i, j - 1, expected_colors.at(i), colors.at(i), text.c_str(), spaces.c_str(), carets.c_str());
                i = j;
            }
        }
    }
    mutable_fish_features() = saved_flags;
    vars.remove(L"VARIABLE_IN_COMMAND", ENV_DEFAULT);
    vars.remove(L"VARIABLE_IN_COMMAND2", ENV_DEFAULT);
}

static void test_split_string_tok() {
    say(L"Testing split_string_tok");
    wcstring_list_t splits;
    splits = split_string_tok(L" hello \t   world", L" \t\n");
    do_test((splits == wcstring_list_t{L"hello", L"world"}));

    splits = split_string_tok(L" stuff ", wcstring(L" "), 0);
    do_test((splits.empty()));

    splits = split_string_tok(L" stuff ", wcstring(L" "), 1);
    do_test((splits == wcstring_list_t{L" stuff "}));

    splits = split_string_tok(L" hello \t   world  andstuff ", L" \t\n", 3);
    do_test((splits == wcstring_list_t{L"hello", L"world", L" andstuff "}));

    // NUL chars are OK.
    wcstring nullstr = L" hello X  world";
    nullstr.at(nullstr.find(L'X')) = L'\0';
    splits = split_string_tok(nullstr, wcstring(L" \0", 2));
    do_test((splits == wcstring_list_t{L"hello", L"world"}));
}

static void test_wwrite_to_fd() {
    say(L"Testing wwrite_to_fd");
    char t[] = "/tmp/fish_test_wwrite.XXXXXX";
    autoclose_fd_t tmpfd{mkstemp(t)};
    if (!tmpfd.valid()) {
        err(L"Unable to create temporary file");
        return;
    }
    tmpfd.close();

    size_t sizes[] = {0, 1, 2, 3, 5, 13, 23, 64, 128, 255, 4096, 4096 * 2};
    for (size_t size : sizes) {
        autoclose_fd_t fd{open(t, O_RDWR | O_TRUNC | O_CREAT, 0666)};
        if (!fd.valid()) {
            wperror(L"open");
            err(L"Unable to open temporary file");
            return;
        }
        wcstring input{};
        for (size_t i = 0; i < size; i++) {
            input.push_back(wchar_t(random()));
        }

        ssize_t amt = wwrite_to_fd(input, fd.fd());
        if (amt < 0) {
            wperror(L"write");
            err(L"Unable to write to temporary file");
            return;
        }
        std::string narrow = wcs2string(input);
        size_t expected_size = narrow.size();
        do_test(static_cast<size_t>(amt) == expected_size);

        if (lseek(fd.fd(), 0, SEEK_SET) < 0) {
            wperror(L"seek");
            err(L"Unable to seek temporary file");
            return;
        }

        std::string contents(expected_size, '\0');
        ssize_t read_amt = read(fd.fd(), &contents[0], expected_size);
        do_test(read_amt >= 0 && static_cast<size_t>(read_amt) == expected_size);
    }
    (void)remove(t);
}

static void test_pcre2_escape() {
    say(L"Testing escaping strings as pcre2 literals");
    // plain text should not be needlessly escaped
    auto input = L"hello world!";
    auto escaped = escape_string(input, 0, STRING_STYLE_REGEX);
    if (escaped != input) {
        err(L"Input string %ls unnecessarily PCRE2 escaped as %ls", input, escaped.c_str());
    }

    // all the following are intended to be ultimately matched literally - even if they don't look
    // like that's the intent - so we escape them.
    const wchar_t *const tests[][2] = {
        {L".ext", L"\\.ext"},
        {L"{word}", L"\\{word\\}"},
        {L"hola-mundo", L"hola\\-mundo"},
        {L"$17.42 is your total?", L"\\$17\\.42 is your total\\?"},
        {L"not really escaped\\?", L"not really escaped\\\\\\?"},
    };

    for (const auto &test : tests) {
        auto escaped = escape_string(test[0], 0, STRING_STYLE_REGEX);
        if (escaped != test[1]) {
            err(L"pcre2_escape error: pcre2_escape(%ls) -> %ls, expected %ls", test[0],
                escaped.c_str(), test[1]);
        }
    }
}

maybe_t<int> builtin_string(parser_t &parser, io_streams_t &streams, const wchar_t **argv);
static void run_one_string_test(const wchar_t *const *argv_raw, int expected_rc,
                                const wchar_t *expected_out) {
    // Copy to a null terminated array, as builtin_string may wish to rearrange our pointers.
    wcstring_list_t argv_list(argv_raw, argv_raw + null_terminated_array_length(argv_raw));
    null_terminated_array_t<wchar_t> argv(argv_list);

    parser_t &parser = parser_t::principal_parser();
    string_output_stream_t outs{};
    null_output_stream_t errs{};
    io_streams_t streams(outs, errs);
    streams.stdin_is_directly_redirected = false;  // read from argv instead of stdin
    maybe_t<int> rc = builtin_string(parser, streams, argv.get());

    wcstring args;
    for (const wcstring &arg : argv_list) {
        args += escape_string(arg, ESCAPE_ALL) + L' ';
    }
    args.resize(args.size() - 1);

    if (rc != expected_rc) {
        std::wstring got = rc ? std::to_wstring(rc.value()) : L"nothing";
        err(L"Test failed on line %lu: [%ls]: expected return code %d but got %s", __LINE__,
            args.c_str(), expected_rc, got.c_str());
    } else if (outs.contents() != expected_out) {
        err(L"Test failed on line %lu: [%ls]: expected [%ls] but got [%ls]", __LINE__, args.c_str(),
            escape_string(expected_out, ESCAPE_ALL).c_str(),
            escape_string(outs.contents(), ESCAPE_ALL).c_str());
    }
}

static void test_string() {
    say(L"Testing builtin_string");
    const struct string_test {
        const wchar_t *argv[15];
        int expected_rc;
        const wchar_t *expected_out;
    } string_tests[] = {
        {{L"string", L"escape", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"escape", L"", nullptr}, STATUS_CMD_OK, L"''\n"},
        {{L"string", L"escape", L"-n", L"", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"escape", L"a", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"escape", L"\x07", nullptr}, STATUS_CMD_OK, L"\\cg\n"},
        {{L"string", L"escape", L"\"x\"", nullptr}, STATUS_CMD_OK, L"'\"x\"'\n"},
        {{L"string", L"escape", L"hello world", nullptr}, STATUS_CMD_OK, L"'hello world'\n"},
        {{L"string", L"escape", L"-n", L"hello world", nullptr}, STATUS_CMD_OK, L"hello\\ world\n"},
        {{L"string", L"escape", L"hello", L"world", nullptr}, STATUS_CMD_OK, L"hello\nworld\n"},
        {{L"string", L"escape", L"-n", L"~", nullptr}, STATUS_CMD_OK, L"\\~\n"},

        {{L"string", L"join", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"join", L"", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"join", L"", L"", L"", L"", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"join", L"", L"a", L"b", L"c", nullptr}, STATUS_CMD_OK, L"abc\n"},
        {{L"string", L"join", L".", L"fishshell", L"com", nullptr},
         STATUS_CMD_OK,
         L"fishshell.com\n"},
        {{L"string", L"join", L"/", L"usr", nullptr}, STATUS_CMD_ERROR, L"usr\n"},
        {{L"string", L"join", L"/", L"usr", L"local", L"bin", nullptr},
         STATUS_CMD_OK,
         L"usr/local/bin\n"},
        {{L"string", L"join", L"...", L"3", L"2", L"1", nullptr}, STATUS_CMD_OK, L"3...2...1\n"},
        {{L"string", L"join", L"-q", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"join", L"-q", L".", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"join", L"-q", L".", L".", nullptr}, STATUS_CMD_ERROR, L""},

        {{L"string", L"length", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"length", L"", nullptr}, STATUS_CMD_ERROR, L"0\n"},
        {{L"string", L"length", L"", L"", L"", nullptr}, STATUS_CMD_ERROR, L"0\n0\n0\n"},
        {{L"string", L"length", L"a", nullptr}, STATUS_CMD_OK, L"1\n"},
        {{L"string", L"length", L"\U0002008A", nullptr}, STATUS_CMD_OK, L"1\n"},
        {{L"string", L"length", L"um", L"dois", L"três", nullptr}, STATUS_CMD_OK, L"2\n4\n4\n"},
        {{L"string", L"length", L"um", L"dois", L"três", nullptr}, STATUS_CMD_OK, L"2\n4\n4\n"},
        {{L"string", L"length", L"-q", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"length", L"-q", L"", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"length", L"-q", L"a", nullptr}, STATUS_CMD_OK, L""},

        {{L"string", L"match", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"match", L"", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"", L"", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"match", L"?", L"a", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"match", L"*", L"", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"match", L"**", L"", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"match", L"*", L"xyzzy", nullptr}, STATUS_CMD_OK, L"xyzzy\n"},
        {{L"string", L"match", L"**", L"plugh", nullptr}, STATUS_CMD_OK, L"plugh\n"},
        {{L"string", L"match", L"a*b", L"axxb", nullptr}, STATUS_CMD_OK, L"axxb\n"},
        {{L"string", L"match", L"a??b", L"axxb", nullptr}, STATUS_CMD_OK, L"axxb\n"},
        {{L"string", L"match", L"-i", L"a??B", L"axxb", nullptr}, STATUS_CMD_OK, L"axxb\n"},
        {{L"string", L"match", L"-i", L"a??b", L"Axxb", nullptr}, STATUS_CMD_OK, L"Axxb\n"},
        {{L"string", L"match", L"a*", L"axxb", nullptr}, STATUS_CMD_OK, L"axxb\n"},
        {{L"string", L"match", L"*a", L"xxa", nullptr}, STATUS_CMD_OK, L"xxa\n"},
        {{L"string", L"match", L"*a*", L"axa", nullptr}, STATUS_CMD_OK, L"axa\n"},
        {{L"string", L"match", L"*a*", L"xax", nullptr}, STATUS_CMD_OK, L"xax\n"},
        {{L"string", L"match", L"*a*", L"bxa", nullptr}, STATUS_CMD_OK, L"bxa\n"},
        {{L"string", L"match", L"*a", L"a", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"match", L"a*", L"a", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"match", L"a*b*c", L"axxbyyc", nullptr}, STATUS_CMD_OK, L"axxbyyc\n"},
        {{L"string", L"match", L"\\*", L"*", nullptr}, STATUS_CMD_OK, L"*\n"},
        {{L"string", L"match", L"a*\\", L"abc\\", nullptr}, STATUS_CMD_OK, L"abc\\\n"},
        {{L"string", L"match", L"a*\\?", L"abc?", nullptr}, STATUS_CMD_OK, L"abc?\n"},

        {{L"string", L"match", L"?", L"", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"?", L"ab", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"??", L"a", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"?a", L"a", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"a?", L"a", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"a??B", L"axxb", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"a*b", L"axxbc", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"*b", L"bbba", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"0x[0-9a-fA-F][0-9a-fA-F]", L"0xbad", nullptr},
         STATUS_CMD_ERROR,
         L""},

        {{L"string", L"match", L"-a", L"*", L"ab", L"cde", nullptr}, STATUS_CMD_OK, L"ab\ncde\n"},
        {{L"string", L"match", L"*", L"ab", L"cde", nullptr}, STATUS_CMD_OK, L"ab\ncde\n"},
        {{L"string", L"match", L"-n", L"*d*", L"cde", nullptr}, STATUS_CMD_OK, L"1 3\n"},
        {{L"string", L"match", L"-n", L"*x*", L"cde", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"-q", L"a*", L"b", L"c", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"-q", L"a*", L"b", L"a", nullptr}, STATUS_CMD_OK, L""},

        {{L"string", L"match", L"-r", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"match", L"-r", L"", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"-r", L"", L"", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"match", L"-r", L".", L"a", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"match", L"-r", L".*", L"", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"match", L"-r", L"a*b", L"b", nullptr}, STATUS_CMD_OK, L"b\n"},
        {{L"string", L"match", L"-r", L"a*b", L"aab", nullptr}, STATUS_CMD_OK, L"aab\n"},
        {{L"string", L"match", L"-r", L"-i", L"a*b", L"Aab", nullptr}, STATUS_CMD_OK, L"Aab\n"},
        {{L"string", L"match", L"-r", L"-a", L"a[bc]", L"abadac", nullptr},
         STATUS_CMD_OK,
         L"ab\nac\n"},
        {{L"string", L"match", L"-r", L"a", L"xaxa", L"axax", nullptr}, STATUS_CMD_OK, L"a\na\n"},
        {{L"string", L"match", L"-r", L"-a", L"a", L"xaxa", L"axax", nullptr},
         STATUS_CMD_OK,
         L"a\na\na\na\n"},
        {{L"string", L"match", L"-r", L"a[bc]", L"abadac", nullptr}, STATUS_CMD_OK, L"ab\n"},
        {{L"string", L"match", L"-r", L"-q", L"a[bc]", L"abadac", nullptr}, STATUS_CMD_OK, L""},
        {{L"string", L"match", L"-r", L"-q", L"a[bc]", L"ad", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"-r", L"(a+)b(c)", L"aabc", nullptr},
         STATUS_CMD_OK,
         L"aabc\naa\nc\n"},
        {{L"string", L"match", L"-r", L"-a", L"(a)b(c)", L"abcabc", nullptr},
         STATUS_CMD_OK,
         L"abc\na\nc\nabc\na\nc\n"},
        {{L"string", L"match", L"-r", L"(a)b(c)", L"abcabc", nullptr},
         STATUS_CMD_OK,
         L"abc\na\nc\n"},
        {{L"string", L"match", L"-r", L"(a|(z))(bc)", L"abc", nullptr},
         STATUS_CMD_OK,
         L"abc\na\nbc\n"},
        {{L"string", L"match", L"-r", L"-n", L"a", L"ada", L"dad", nullptr},
         STATUS_CMD_OK,
         L"1 1\n2 1\n"},
        {{L"string", L"match", L"-r", L"-n", L"-a", L"a", L"bacadae", nullptr},
         STATUS_CMD_OK,
         L"2 1\n4 1\n6 1\n"},
        {{L"string", L"match", L"-r", L"-n", L"(a).*(b)", L"a---b", nullptr},
         STATUS_CMD_OK,
         L"1 5\n1 1\n5 1\n"},
        {{L"string", L"match", L"-r", L"-n", L"(a)(b)", L"ab", nullptr},
         STATUS_CMD_OK,
         L"1 2\n1 1\n2 1\n"},
        {{L"string", L"match", L"-r", L"-n", L"(a)(b)", L"abab", nullptr},
         STATUS_CMD_OK,
         L"1 2\n1 1\n2 1\n"},
        {{L"string", L"match", L"-r", L"-n", L"-a", L"(a)(b)", L"abab", nullptr},
         STATUS_CMD_OK,
         L"1 2\n1 1\n2 1\n3 2\n3 1\n4 1\n"},
        {{L"string", L"match", L"-r", L"*", L"", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"match", L"-r", L"-a", L"a*", L"b", nullptr}, STATUS_CMD_OK, L"\n\n"},
        {{L"string", L"match", L"-r", L"foo\\Kbar", L"foobar", nullptr}, STATUS_CMD_OK, L"bar\n"},
        {{L"string", L"match", L"-r", L"(foo)\\Kbar", L"foobar", nullptr},
         STATUS_CMD_OK,
         L"bar\nfoo\n"},
        {{L"string", L"replace", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"replace", L"", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"replace", L"", L"", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"replace", L"", L"", L"", nullptr}, STATUS_CMD_ERROR, L"\n"},
        {{L"string", L"replace", L"", L"", L" ", nullptr}, STATUS_CMD_ERROR, L" \n"},
        {{L"string", L"replace", L"a", L"b", L"", nullptr}, STATUS_CMD_ERROR, L"\n"},
        {{L"string", L"replace", L"a", L"b", L"a", nullptr}, STATUS_CMD_OK, L"b\n"},
        {{L"string", L"replace", L"a", L"b", L"xax", nullptr}, STATUS_CMD_OK, L"xbx\n"},
        {{L"string", L"replace", L"a", L"b", L"xax", L"axa", nullptr},
         STATUS_CMD_OK,
         L"xbx\nbxa\n"},
        {{L"string", L"replace", L"bar", L"x", L"red barn", nullptr}, STATUS_CMD_OK, L"red xn\n"},
        {{L"string", L"replace", L"x", L"bar", L"red xn", nullptr}, STATUS_CMD_OK, L"red barn\n"},
        {{L"string", L"replace", L"--", L"x", L"-", L"xyz", nullptr}, STATUS_CMD_OK, L"-yz\n"},
        {{L"string", L"replace", L"--", L"y", L"-", L"xyz", nullptr}, STATUS_CMD_OK, L"x-z\n"},
        {{L"string", L"replace", L"--", L"z", L"-", L"xyz", nullptr}, STATUS_CMD_OK, L"xy-\n"},
        {{L"string", L"replace", L"-i", L"z", L"X", L"_Z_", nullptr}, STATUS_CMD_OK, L"_X_\n"},
        {{L"string", L"replace", L"-a", L"a", L"A", L"aaa", nullptr}, STATUS_CMD_OK, L"AAA\n"},
        {{L"string", L"replace", L"-i", L"a", L"z", L"AAA", nullptr}, STATUS_CMD_OK, L"zAA\n"},
        {{L"string", L"replace", L"-q", L"x", L">x<", L"x", nullptr}, STATUS_CMD_OK, L""},
        {{L"string", L"replace", L"-a", L"x", L"", L"xxx", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"replace", L"-a", L"***", L"_", L"*****", nullptr}, STATUS_CMD_OK, L"_**\n"},
        {{L"string", L"replace", L"-a", L"***", L"***", L"******", nullptr},
         STATUS_CMD_OK,
         L"******\n"},
        {{L"string", L"replace", L"-a", L"a", L"b", L"xax", L"axa", nullptr},
         STATUS_CMD_OK,
         L"xbx\nbxb\n"},

        {{L"string", L"replace", L"-r", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"replace", L"-r", L"", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"replace", L"-r", L"", L"", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"replace", L"-r", L"", L"", L"", nullptr},
         STATUS_CMD_OK,
         L"\n"},  // pcre2 behavior
        {{L"string", L"replace", L"-r", L"", L"", L" ", nullptr},
         STATUS_CMD_OK,
         L" \n"},  // pcre2 behavior
        {{L"string", L"replace", L"-r", L"a", L"b", L"", nullptr}, STATUS_CMD_ERROR, L"\n"},
        {{L"string", L"replace", L"-r", L"a", L"b", L"a", nullptr}, STATUS_CMD_OK, L"b\n"},
        {{L"string", L"replace", L"-r", L".", L"x", L"abc", nullptr}, STATUS_CMD_OK, L"xbc\n"},
        {{L"string", L"replace", L"-r", L".", L"", L"abc", nullptr}, STATUS_CMD_OK, L"bc\n"},
        {{L"string", L"replace", L"-r", L"(\\w)(\\w)", L"$2$1", L"ab", nullptr},
         STATUS_CMD_OK,
         L"ba\n"},
        {{L"string", L"replace", L"-r", L"(\\w)", L"$1$1", L"ab", nullptr},
         STATUS_CMD_OK,
         L"aab\n"},
        {{L"string", L"replace", L"-r", L"-a", L".", L"x", L"abc", nullptr},
         STATUS_CMD_OK,
         L"xxx\n"},
        {{L"string", L"replace", L"-r", L"-a", L"(\\w)", L"$1$1", L"ab", nullptr},
         STATUS_CMD_OK,
         L"aabb\n"},
        {{L"string", L"replace", L"-r", L"-a", L".", L"", L"abc", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"replace", L"-r", L"a", L"x", L"bc", L"cd", L"de", nullptr},
         STATUS_CMD_ERROR,
         L"bc\ncd\nde\n"},
        {{L"string", L"replace", L"-r", L"a", L"x", L"aba", L"caa", nullptr},
         STATUS_CMD_OK,
         L"xba\ncxa\n"},
        {{L"string", L"replace", L"-r", L"-a", L"a", L"x", L"aba", L"caa", nullptr},
         STATUS_CMD_OK,
         L"xbx\ncxx\n"},
        {{L"string", L"replace", L"-r", L"-i", L"A", L"b", L"xax", nullptr},
         STATUS_CMD_OK,
         L"xbx\n"},
        {{L"string", L"replace", L"-r", L"-i", L"[a-z]", L".", L"1A2B", nullptr},
         STATUS_CMD_OK,
         L"1.2B\n"},
        {{L"string", L"replace", L"-r", L"A", L"b", L"xax", nullptr}, STATUS_CMD_ERROR, L"xax\n"},
        {{L"string", L"replace", L"-r", L"a", L"$1", L"a", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"replace", L"-r", L"(a)", L"$2", L"a", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"replace", L"-r", L"*", L".", L"a", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"replace", L"-ra", L"x", L"\\c", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"replace", L"-r", L"^(.)", L"\t$1", L"abc", L"x", nullptr},
         STATUS_CMD_OK,
         L"\tabc\n\tx\n"},

        {{L"string", L"split", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"split", L":", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"split", L".", L"www.ch.ic.ac.uk", nullptr},
         STATUS_CMD_OK,
         L"www\nch\nic\nac\nuk\n"},
        {{L"string", L"split", L"..", L"....", nullptr}, STATUS_CMD_OK, L"\n\n\n"},
        {{L"string", L"split", L"-m", L"x", L"..", L"....", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"split", L"-m1", L"..", L"....", nullptr}, STATUS_CMD_OK, L"\n..\n"},
        {{L"string", L"split", L"-m0", L"/", L"/usr/local/bin/fish", nullptr},
         STATUS_CMD_ERROR,
         L"/usr/local/bin/fish\n"},
        {{L"string", L"split", L"-m2", L":", L"a:b:c:d", L"e:f:g:h", nullptr},
         STATUS_CMD_OK,
         L"a\nb\nc:d\ne\nf\ng:h\n"},
        {{L"string", L"split", L"-m1", L"-r", L"/", L"/usr/local/bin/fish", nullptr},
         STATUS_CMD_OK,
         L"/usr/local/bin\nfish\n"},
        {{L"string", L"split", L"-r", L".", L"www.ch.ic.ac.uk", nullptr},
         STATUS_CMD_OK,
         L"www\nch\nic\nac\nuk\n"},
        {{L"string", L"split", L"--", L"--", L"a--b---c----d", nullptr},
         STATUS_CMD_OK,
         L"a\nb\n-c\n\nd\n"},
        {{L"string", L"split", L"-r", L"..", L"....", nullptr}, STATUS_CMD_OK, L"\n\n\n"},
        {{L"string", L"split", L"-r", L"--", L"--", L"a--b---c----d", nullptr},
         STATUS_CMD_OK,
         L"a\nb-\nc\n\nd\n"},
        {{L"string", L"split", L"", L"", nullptr}, STATUS_CMD_ERROR, L"\n"},
        {{L"string", L"split", L"", L"a", nullptr}, STATUS_CMD_ERROR, L"a\n"},
        {{L"string", L"split", L"", L"ab", nullptr}, STATUS_CMD_OK, L"a\nb\n"},
        {{L"string", L"split", L"", L"abc", nullptr}, STATUS_CMD_OK, L"a\nb\nc\n"},
        {{L"string", L"split", L"-m1", L"", L"abc", nullptr}, STATUS_CMD_OK, L"a\nbc\n"},
        {{L"string", L"split", L"-r", L"", L"", nullptr}, STATUS_CMD_ERROR, L"\n"},
        {{L"string", L"split", L"-r", L"", L"a", nullptr}, STATUS_CMD_ERROR, L"a\n"},
        {{L"string", L"split", L"-r", L"", L"ab", nullptr}, STATUS_CMD_OK, L"a\nb\n"},
        {{L"string", L"split", L"-r", L"", L"abc", nullptr}, STATUS_CMD_OK, L"a\nb\nc\n"},
        {{L"string", L"split", L"-r", L"-m1", L"", L"abc", nullptr}, STATUS_CMD_OK, L"ab\nc\n"},
        {{L"string", L"split", L"-q", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"split", L"-q", L":", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"split", L"-q", L"x", L"axbxc", nullptr}, STATUS_CMD_OK, L""},

        {{L"string", L"sub", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"sub", L"abcde", nullptr}, STATUS_CMD_OK, L"abcde\n"},
        {{L"string", L"sub", L"-l", L"x", L"abcde", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"sub", L"-s", L"x", L"abcde", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"sub", L"-l0", L"abcde", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"sub", L"-l2", L"abcde", nullptr}, STATUS_CMD_OK, L"ab\n"},
        {{L"string", L"sub", L"-l5", L"abcde", nullptr}, STATUS_CMD_OK, L"abcde\n"},
        {{L"string", L"sub", L"-l6", L"abcde", nullptr}, STATUS_CMD_OK, L"abcde\n"},
        {{L"string", L"sub", L"-l-1", L"abcde", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"sub", L"-s0", L"abcde", nullptr}, STATUS_INVALID_ARGS, L""},
        {{L"string", L"sub", L"-s1", L"abcde", nullptr}, STATUS_CMD_OK, L"abcde\n"},
        {{L"string", L"sub", L"-s5", L"abcde", nullptr}, STATUS_CMD_OK, L"e\n"},
        {{L"string", L"sub", L"-s6", L"abcde", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"sub", L"-s-1", L"abcde", nullptr}, STATUS_CMD_OK, L"e\n"},
        {{L"string", L"sub", L"-s-5", L"abcde", nullptr}, STATUS_CMD_OK, L"abcde\n"},
        {{L"string", L"sub", L"-s-6", L"abcde", nullptr}, STATUS_CMD_OK, L"abcde\n"},
        {{L"string", L"sub", L"-s1", L"-l0", L"abcde", nullptr}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"sub", L"-s1", L"-l1", L"abcde", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"sub", L"-s2", L"-l2", L"abcde", nullptr}, STATUS_CMD_OK, L"bc\n"},
        {{L"string", L"sub", L"-s-1", L"-l1", L"abcde", nullptr}, STATUS_CMD_OK, L"e\n"},
        {{L"string", L"sub", L"-s-1", L"-l2", L"abcde", nullptr}, STATUS_CMD_OK, L"e\n"},
        {{L"string", L"sub", L"-s-3", L"-l2", L"abcde", nullptr}, STATUS_CMD_OK, L"cd\n"},
        {{L"string", L"sub", L"-s-3", L"-l4", L"abcde", nullptr}, STATUS_CMD_OK, L"cde\n"},
        {{L"string", L"sub", L"-q", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"sub", L"-q", L"abcde", nullptr}, STATUS_CMD_OK, L""},

        {{L"string", L"trim", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"trim", L""}, STATUS_CMD_ERROR, L"\n"},
        {{L"string", L"trim", L" "}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"trim", L"  \f\n\r\t"}, STATUS_CMD_OK, L"\n"},
        {{L"string", L"trim", L" a"}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"a "}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L" a "}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-l", L" a"}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-l", L"a "}, STATUS_CMD_ERROR, L"a \n"},
        {{L"string", L"trim", L"-l", L" a "}, STATUS_CMD_OK, L"a \n"},
        {{L"string", L"trim", L"-r", L" a"}, STATUS_CMD_ERROR, L" a\n"},
        {{L"string", L"trim", L"-r", L"a "}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-r", L" a "}, STATUS_CMD_OK, L" a\n"},
        {{L"string", L"trim", L"-c", L".", L" a"}, STATUS_CMD_ERROR, L" a\n"},
        {{L"string", L"trim", L"-c", L".", L"a "}, STATUS_CMD_ERROR, L"a \n"},
        {{L"string", L"trim", L"-c", L".", L" a "}, STATUS_CMD_ERROR, L" a \n"},
        {{L"string", L"trim", L"-c", L".", L".a"}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-c", L".", L"a."}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-c", L".", L".a."}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-c", L"\\/", L"/a\\"}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-c", L"\\/", L"a/"}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-c", L"\\/", L"\\a/"}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"trim", L"-c", L"", L".a."}, STATUS_CMD_ERROR, L".a.\n"}};

    for (const auto &t : string_tests) {
        run_one_string_test(t.argv, t.expected_rc, t.expected_out);
    }

    const auto saved_flags = fish_features();
    const struct string_test qmark_noglob_tests[] = {
        {{L"string", L"match", L"a*b?c", L"axxb?c", nullptr}, STATUS_CMD_OK, L"axxb?c\n"},
        {{L"string", L"match", L"*?", L"a", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"*?", L"ab", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"?*", L"a", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"?*", L"ab", nullptr}, STATUS_CMD_ERROR, L""},
        {{L"string", L"match", L"a*\\?", L"abc?", nullptr}, STATUS_CMD_ERROR, L""}};
    mutable_fish_features().set(features_t::qmark_noglob, true);
    for (const auto &t : qmark_noglob_tests) {
        run_one_string_test(t.argv, t.expected_rc, t.expected_out);
    }

    const struct string_test qmark_glob_tests[] = {
        {{L"string", L"match", L"a*b?c", L"axxbyc", nullptr}, STATUS_CMD_OK, L"axxbyc\n"},
        {{L"string", L"match", L"*?", L"a", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"match", L"*?", L"ab", nullptr}, STATUS_CMD_OK, L"ab\n"},
        {{L"string", L"match", L"?*", L"a", nullptr}, STATUS_CMD_OK, L"a\n"},
        {{L"string", L"match", L"?*", L"ab", nullptr}, STATUS_CMD_OK, L"ab\n"},
        {{L"string", L"match", L"a*\\?", L"abc?", nullptr}, STATUS_CMD_OK, L"abc?\n"}};
    mutable_fish_features().set(features_t::qmark_noglob, false);
    for (const auto &t : qmark_glob_tests) {
        run_one_string_test(t.argv, t.expected_rc, t.expected_out);
    }
    mutable_fish_features() = saved_flags;
}

/// Helper for test_timezone_env_vars().
long return_timezone_hour(time_t tstamp, const wchar_t *timezone) {
    auto &vars = parser_t::principal_parser().vars();
    struct tm ltime;
    char ltime_str[3];
    char *str_ptr;
    size_t n;

    vars.set_one(L"TZ", ENV_EXPORT, timezone);

    const auto var = vars.get(L"TZ", ENV_DEFAULT);
    (void)var;

    localtime_r(&tstamp, &ltime);
    n = strftime(ltime_str, 3, "%H", &ltime);
    if (n != 2) {
        err(L"strftime() returned %d, expected 2", n);
        return 0;
    }
    return strtol(ltime_str, &str_ptr, 10);
}

/// Verify that setting special env vars have the expected effect on the current shell process.
static void test_timezone_env_vars() {
    // Confirm changing the timezone affects fish's idea of the local time.
    time_t tstamp = time(nullptr);

    long first_tstamp = return_timezone_hour(tstamp, L"UTC-1");
    long second_tstamp = return_timezone_hour(tstamp, L"UTC-2");
    long delta = second_tstamp - first_tstamp;
    if (delta != 1 && delta != -23) {
        err(L"expected a one hour timezone delta got %ld", delta);
    }
}

/// Verify that setting special env vars have the expected effect on the current shell process.
static void test_env_vars() {
    test_timezone_env_vars();
    // TODO: Add tests for the locale and ncurses vars.

    env_var_t v1 = {L"abc", env_var_t::flag_export};
    env_var_t v2 = {wcstring_list_t{L"abc"}, env_var_t::flag_export};
    env_var_t v3 = {wcstring_list_t{L"abc"}, 0};
    env_var_t v4 = {wcstring_list_t{L"abc", L"def"}, env_var_t::flag_export};
    do_test(v1 == v2 && !(v1 != v2));
    do_test(v1 != v3 && !(v1 == v3));
    do_test(v1 != v4 && !(v1 == v4));
}

static void test_env_snapshot() {
    if (system("mkdir -p test/fish_env_snapshot_test/")) err(L"mkdir failed");
    bool pushed = pushd("test/fish_env_snapshot_test");
    do_test(pushed);
    auto &vars = parser_t::principal_parser().vars();
    vars.push(true);
    wcstring before_pwd = vars.get(L"PWD")->as_string();
    vars.set(L"test_env_snapshot_var", 0, {L"before"});
    const auto snapshot = vars.snapshot();
    vars.set(L"PWD", 0, {L"/newdir"});
    vars.set(L"test_env_snapshot_var", 0, {L"after"});
    vars.set(L"test_env_snapshot_var_2", 0, {L"after"});

    // vars should be unaffected by the snapshot
    do_test(vars.get(L"PWD")->as_string() == L"/newdir");
    do_test(vars.get(L"test_env_snapshot_var")->as_string() == L"after");
    do_test(vars.get(L"test_env_snapshot_var_2")->as_string() == L"after");

    // snapshot should have old values of vars
    do_test(snapshot->get(L"PWD")->as_string() == before_pwd);
    do_test(snapshot->get(L"test_env_snapshot_var")->as_string() == L"before");
    do_test(snapshot->get(L"test_env_snapshot_var_2") == none());

    // snapshots see global var changes except for perproc like PWD
    vars.set(L"test_env_snapshot_var_3", ENV_GLOBAL, {L"reallyglobal"});
    do_test(vars.get(L"test_env_snapshot_var_3")->as_string() == L"reallyglobal");
    do_test(snapshot->get(L"test_env_snapshot_var_3")->as_string() == L"reallyglobal");

    vars.pop();
    popd();
}

static void test_illegal_command_exit_code() {
    say(L"Testing illegal command exit code");

    // We need to be in an empty directory so that none of the wildcards match a file that might be
    // in the fish source tree. In particular we need to ensure that "?" doesn't match a file
    // named by a single character. See issue #3852.
    if (!pushd("test/temp")) return;

    struct command_result_tuple_t {
        const wchar_t *txt;
        int result;
    };

    const command_result_tuple_t tests[] = {
        {L"echo -n", STATUS_CMD_OK},
        {L"pwd", STATUS_CMD_OK},
        {L"UNMATCHABLE_WILDCARD*", STATUS_UNMATCHED_WILDCARD},
        {L"UNMATCHABLE_WILDCARD**", STATUS_UNMATCHED_WILDCARD},
        {L"?", STATUS_UNMATCHED_WILDCARD},
        {L"abc?def", STATUS_UNMATCHED_WILDCARD},
    };

    const io_chain_t empty_ios;
    parser_t &parser = parser_t::principal_parser();

    for (const auto &test : tests) {
        parser.eval(test.txt, empty_ios);

        int exit_status = parser.get_last_status();
        if (exit_status != test.result) {
            err(L"command '%ls': expected exit code %d, got %d", test.txt, test.result,
                exit_status);
        }
    }

    popd();
}

void test_maybe() {
    say(L"Testing maybe_t");
    do_test(!bool(maybe_t<int>()));
    maybe_t<int> m(3);
    do_test(m.has_value());
    do_test(m.value() == 3);
    m.reset();
    do_test(!m.has_value());
    m = 123;
    do_test(m.has_value());
    do_test(m.has_value() && m.value() == 123);
    m = maybe_t<int>();
    do_test(!m.has_value());
    m = maybe_t<int>(64);
    do_test(m.has_value() && m.value() == 64);
    m = 123;
    do_test(m == maybe_t<int>(123));
    do_test(m != maybe_t<int>());
    do_test(maybe_t<int>() == none());
    do_test(!maybe_t<int>(none()).has_value());
    m = none();
    do_test(!bool(m));

    maybe_t<std::string> m2("abc");
    do_test(!m2.missing_or_empty());
    m2 = "";
    do_test(m2.missing_or_empty());
    m2 = none();
    do_test(m2.missing_or_empty());

    maybe_t<std::string> m0 = none();
    maybe_t<std::string> m3("hi");
    maybe_t<std::string> m4 = m3;
    do_test(m4 && *m4 == "hi");
    maybe_t<std::string> m5 = m0;
    do_test(!m5);

    maybe_t<std::string> acquire_test("def");
    do_test(acquire_test);
    std::string res = acquire_test.acquire();
    do_test(!acquire_test);
    do_test(res == "def");

    // maybe_t<T> should be copyable iff T is copyable.
    using copyable = std::shared_ptr<int>;
    using noncopyable = std::unique_ptr<int>;
    do_test(std::is_copy_assignable<maybe_t<copyable>>::value == true);
    do_test(std::is_copy_constructible<maybe_t<copyable>>::value == true);
    do_test(std::is_copy_assignable<maybe_t<noncopyable>>::value == false);
    do_test(std::is_copy_constructible<maybe_t<noncopyable>>::value == false);

    maybe_t<std::string> c1{"abc"};
    maybe_t<std::string> c2 = c1;
    do_test(c1.value() == "abc");
    do_test(c2.value() == "abc");
    c2 = c1;
    do_test(c1.value() == "abc");
    do_test(c2.value() == "abc");
}

void test_layout_cache() {
    layout_cache_t seqs;

    // Verify escape code cache.
    do_test(seqs.find_escape_code(L"abc") == 0);
    seqs.add_escape_code(L"abc");
    seqs.add_escape_code(L"abc");
    do_test(seqs.esc_cache_size() == 1);
    do_test(seqs.find_escape_code(L"abc") == 3);
    do_test(seqs.find_escape_code(L"abcd") == 3);
    do_test(seqs.find_escape_code(L"abcde") == 3);
    do_test(seqs.find_escape_code(L"xabcde") == 0);
    seqs.add_escape_code(L"ac");
    do_test(seqs.find_escape_code(L"abcd") == 3);
    do_test(seqs.find_escape_code(L"acbd") == 2);
    seqs.add_escape_code(L"wxyz");
    do_test(seqs.find_escape_code(L"abc") == 3);
    do_test(seqs.find_escape_code(L"abcd") == 3);
    do_test(seqs.find_escape_code(L"wxyz123") == 4);
    do_test(seqs.find_escape_code(L"qwxyz123") == 0);
    do_test(seqs.esc_cache_size() == 3);
    seqs.clear();
    do_test(seqs.esc_cache_size() == 0);
    do_test(seqs.find_escape_code(L"abcd") == 0);

    auto huge = std::numeric_limits<size_t>::max();

    // Verify prompt layout cache.
    for (size_t i = 0; i < layout_cache_t::prompt_cache_max_size; i++) {
        wcstring input = std::to_wstring(i);
        do_test(!seqs.find_prompt_layout(input));
        seqs.add_prompt_layout({input, huge, input, {{}, i, 0}});
        do_test(seqs.find_prompt_layout(input)->layout.max_line_width == i);
    }

    size_t expected_evictee = 3;
    for (size_t i = 0; i < layout_cache_t::prompt_cache_max_size; i++) {
        if (i != expected_evictee)
            do_test(seqs.find_prompt_layout(std::to_wstring(i))->layout.max_line_width == i);
    }

    seqs.add_prompt_layout({L"whatever", huge, L"whatever", {{}, 100, 0}});
    do_test(!seqs.find_prompt_layout(std::to_wstring(expected_evictee)));
    do_test(seqs.find_prompt_layout(L"whatever", huge)->layout.max_line_width == 100);
}

void test_prompt_truncation() {
    layout_cache_t cache;
    wcstring trunc;
    prompt_layout_t layout;

    /// Helper to return 'layout' formatted as a string for easy comparison.
    auto format_layout = [&] {
        wcstring line_breaks;
        bool first = true;
        for (const size_t line_break : layout.line_breaks) {
            if (!first) {
                line_breaks.push_back(L',');
            }
            line_breaks.append(format_string(L"%lu", (unsigned long)line_break));
            first = false;
        }
        return format_string(L"[%ls],%lu,%lu", line_breaks.c_str(),
                             (unsigned long)layout.max_line_width,
                             (unsigned long)layout.last_line_width);
    };

    /// Join some strings with newline.
    auto join = [](std::initializer_list<wcstring> vals) { return join_strings(vals, L'\n'); };

    wcstring ellipsis = {get_ellipsis_char()};

    // No truncation.
    layout = cache.calc_prompt_layout(L"abcd", &trunc);
    do_test(format_layout() == L"[],4,4");
    do_test(trunc == L"abcd");

    // Line break calculation.
    layout = cache.calc_prompt_layout(join({
                                          L"0123456789ABCDEF",  //
                                          L"012345",            //
                                          L"0123456789abcdef",  //
                                          L"xyz"                //
                                      }),
                                      &trunc, 80);
    do_test(format_layout() == L"[16,23,40],16,3");

    // Basic truncation.
    layout = cache.calc_prompt_layout(L"0123456789ABCDEF", &trunc, 8);
    do_test(format_layout() == L"[],8,8");
    do_test(trunc == ellipsis + L"9ABCDEF");

    // Multiline truncation.
    layout = cache.calc_prompt_layout(join({
                                          L"0123456789ABCDEF",  //
                                          L"012345",            //
                                          L"0123456789abcdef",  //
                                          L"xyz"                //
                                      }),
                                      &trunc, 8);
    do_test(format_layout() == L"[8,15,24],8,3");
    do_test(trunc == join({ellipsis + L"9ABCDEF", L"012345", ellipsis + L"9abcdef", L"xyz"}));

    // Escape sequences are not truncated.
    layout =
        cache.calc_prompt_layout(L"\x1B]50;CurrentDir=test/foo\x07NOT_PART_OF_SEQUENCE", &trunc, 4);
    do_test(format_layout() == L"[],4,4");
    do_test(trunc == ellipsis + L"\x1B]50;CurrentDir=test/foo\x07NCE");

    // Newlines in escape sequences are skipped.
    layout = cache.calc_prompt_layout(L"\x1B]50;CurrentDir=\ntest/foo\x07NOT_PART_OF_SEQUENCE",
                                      &trunc, 4);
    do_test(format_layout() == L"[],4,4");
    do_test(trunc == ellipsis + L"\x1B]50;CurrentDir=\ntest/foo\x07NCE");

    // We will truncate down to one character if we have to.
    layout = cache.calc_prompt_layout(L"Yay", &trunc, 1);
    do_test(format_layout() == L"[],1,1");
    do_test(trunc == ellipsis);
}

void test_normalize_path() {
    say(L"Testing path normalization");
    do_test(normalize_path(L"") == L".");
    do_test(normalize_path(L"..") == L"..");
    do_test(normalize_path(L"./") == L".");
    do_test(normalize_path(L"./.") == L".");
    do_test(normalize_path(L"/") == L"/");
    do_test(normalize_path(L"//") == L"//");
    do_test(normalize_path(L"///") == L"/");
    do_test(normalize_path(L"////") == L"/");
    do_test(normalize_path(L"/.///") == L"/");
    do_test(normalize_path(L".//") == L".");
    do_test(normalize_path(L"/.//../") == L"/");
    do_test(normalize_path(L"////abc") == L"/abc");
    do_test(normalize_path(L"/abc") == L"/abc");
    do_test(normalize_path(L"/abc/") == L"/abc");
    do_test(normalize_path(L"/abc/..def/") == L"/abc/..def");
    do_test(normalize_path(L"//abc/../def/") == L"//def");
    do_test(normalize_path(L"abc/../abc/../abc/../abc") == L"abc");
    do_test(normalize_path(L"../../") == L"../..");
    do_test(normalize_path(L"foo/./bar") == L"foo/bar");
    do_test(normalize_path(L"foo/../") == L".");
    do_test(normalize_path(L"foo/../foo") == L"foo");
    do_test(normalize_path(L"foo/../foo/") == L"foo");
    do_test(normalize_path(L"foo/././bar/.././baz") == L"foo/baz");

    do_test(path_normalize_for_cd(L"/", L"..") == L"/..");
    do_test(path_normalize_for_cd(L"/abc/", L"..") == L"/");
    do_test(path_normalize_for_cd(L"/abc/def/", L"..") == L"/abc");
    do_test(path_normalize_for_cd(L"/abc/def/", L"../..") == L"/");
    do_test(path_normalize_for_cd(L"/abc///def/", L"../..") == L"/");
    do_test(path_normalize_for_cd(L"/abc///def/", L"../..") == L"/");
    do_test(path_normalize_for_cd(L"/abc///def///", L"../..") == L"/");
    do_test(path_normalize_for_cd(L"/abc///def///", L"..") == L"/abc");
    do_test(path_normalize_for_cd(L"/abc///def///", L"..") == L"/abc");
    do_test(path_normalize_for_cd(L"/abc/def/", L"./././/..") == L"/abc");
    do_test(path_normalize_for_cd(L"/abc/def/", L"../../../") == L"/../");
    do_test(path_normalize_for_cd(L"/abc/def/", L"../ghi/..") == L"/abc/ghi/..");
}

void test_dirname_basename() {
    say(L"Testing wdirname and wbasename");
    const struct testcase_t {
        const wchar_t *path;
        const wchar_t *dir;
        const wchar_t *base;
    } testcases[] = {
        {L"", L".", L"."},
        {L"foo//", L".", L"foo"},
        {L"foo//////", L".", L"foo"},
        {L"/////foo", L"/", L"foo"},
        {L"/////foo", L"/", L"foo"},
        {L"//foo/////bar", L"//foo", L"bar"},
        {L"foo/////bar", L"foo", L"bar"},

        // Examples given in XPG4.2.
        {L"/usr/lib", L"/usr", L"lib"},
        {L"usr", L".", L"usr"},
        {L"/", L"/", L"/"},
        {L".", L".", L"."},
        {L"..", L".", L".."},
    };
    for (const auto &tc : testcases) {
        wcstring dir = wdirname(tc.path);
        if (dir != tc.dir) {
            err(L"Wrong dirname for \"%ls\": expected \"%ls\", got \"%ls\"", tc.path, tc.dir,
                dir.c_str());
        }
        wcstring base = wbasename(tc.path);
        if (base != tc.base) {
            err(L"Wrong basename for \"%ls\": expected \"%ls\", got \"%ls\"", tc.path, tc.base,
                base.c_str());
        }
    }
    // Ensures strings which greatly exceed PATH_MAX still work (#7837).
    wcstring longpath = get_overlong_path();
    wcstring longpath_dir = longpath.substr(0, longpath.rfind(L'/'));
    do_test(wdirname(longpath) == longpath_dir);
    do_test(wbasename(longpath) == L"overlong");
}

static void test_topic_monitor() {
    say(L"Testing topic monitor");
    topic_monitor_t monitor;
    generation_list_t gens{};
    constexpr auto t = topic_t::sigchld;
    gens.sigchld = 0;
    do_test(monitor.generation_for_topic(t) == 0);
    auto changed = monitor.check(&gens, false /* wait */);
    do_test(!changed);
    do_test(gens.sigchld == 0);

    monitor.post(t);
    changed = monitor.check(&gens, true /* wait */);
    do_test(changed);
    do_test(gens.at(t) == 1);
    do_test(monitor.generation_for_topic(t) == 1);

    monitor.post(t);
    do_test(monitor.generation_for_topic(t) == 2);
    changed = monitor.check(&gens, true /* wait */);
    do_test(changed);
    do_test(gens.sigchld == 2);
}

static void test_topic_monitor_torture() {
    say(L"Torture-testing topic monitor");
    topic_monitor_t monitor;
    const size_t thread_count = 64;
    constexpr auto t1 = topic_t::sigchld;
    constexpr auto t2 = topic_t::sighupint;
    std::vector<generation_list_t> gens;
    gens.resize(thread_count, generation_list_t::invalids());
    std::atomic<uint32_t> post_count{};
    for (auto &gen : gens) {
        gen = monitor.current_generations();
        post_count += 1;
        monitor.post(t1);
    }

    std::atomic<uint32_t> completed{};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < thread_count; i++) {
        threads.emplace_back(
            [&](size_t i) {
                for (size_t j = 0; j < (1 << 11); j++) {
                    auto before = gens[i];
                    auto changed = monitor.check(&gens[i], true /* wait */);
                    (void)changed;
                    do_test(before.at(t1) < gens[i].at(t1));
                    do_test(gens[i].at(t1) <= post_count);
                    do_test(gens[i].at(t2) == 0);
                }
                auto amt = completed.fetch_add(1, std::memory_order_relaxed);
                (void)amt;
            },
            i);
    }

    while (completed.load(std::memory_order_relaxed) < thread_count) {
        post_count += 1;
        monitor.post(t1);
        std::this_thread::yield();
    }
    for (auto &t : threads) t.join();
}

static void test_pipes() {
    say(L"Testing pipes");
    // Here we just test that each pipe has CLOEXEC set and is in the high range.
    // Note pipe creation may fail due to fd exhaustion; don't fail in that case.
    std::vector<autoclose_pipes_t> pipes;
    for (int i = 0; i < 10; i++) {
        if (auto pipe = make_autoclose_pipes()) {
            pipes.push_back(pipe.acquire());
        }
    }
    for (const auto &pipe : pipes) {
        for (int fd : {pipe.read.fd(), pipe.write.fd()}) {
            do_test(fd >= k_first_high_fd);
            int flags = fcntl(fd, F_GETFD, 0);
            do_test(flags >= 0);
            do_test(bool(flags & FD_CLOEXEC));
        }
    }
}

static void test_fd_event_signaller() {
    say(L"Testing fd event signaller");
    fd_event_signaller_t sema;
    do_test(!sema.try_consume());
    do_test(!sema.poll());

    // Post once.
    sema.post();
    do_test(sema.poll());
    do_test(sema.poll());
    do_test(sema.try_consume());
    do_test(!sema.poll());
    do_test(!sema.try_consume());

    // Posts are coalesced.
    sema.post();
    sema.post();
    sema.post();
    do_test(sema.poll());
    do_test(sema.poll());
    do_test(sema.try_consume());
    do_test(!sema.poll());
    do_test(!sema.try_consume());
}

static void test_timer_format() {
    say(L"Testing timer format");
    // This test uses numeric output, so we need to set the locale.
    char *saved_locale = strdup(setlocale(LC_NUMERIC, nullptr));
    setlocale(LC_NUMERIC, "C");
    auto t1 = timer_snapshot_t::take();
    t1.cpu_fish.ru_utime.tv_usec = 0;
    t1.cpu_fish.ru_stime.tv_usec = 0;
    t1.cpu_children.ru_utime.tv_usec = 0;
    t1.cpu_children.ru_stime.tv_usec = 0;
    auto t2 = t1;
    t2.cpu_fish.ru_utime.tv_usec = 999995;
    t2.cpu_fish.ru_stime.tv_usec = 999994;
    t2.cpu_children.ru_utime.tv_usec = 1000;
    t2.cpu_children.ru_stime.tv_usec = 500;
    t2.wall += std::chrono::microseconds(500);
    auto expected =
        LR"(
________________________________________________________
Executed in  500.00 micros    fish         external
   usr time    1.00 secs      1.00 secs    1.00 millis
   sys time    1.00 secs      1.00 secs    0.50 millis
)";  //        (a)            (b)            (c)
     // (a) remaining columns should align even if there are different units
     // (b) carry to the next unit when it would overflow %6.2F
     // (c) carry to the next unit when the larger one exceeds 1000
    std::wstring actual = timer_snapshot_t::print_delta(t1, t2, true);
    if (actual != expected) {
        err(L"Failed to format timer snapshot\nExpected: %ls\nActual:%ls\n", expected,
            actual.c_str());
    }
    setlocale(LC_NUMERIC, saved_locale);
    free(saved_locale);
}

static void test_killring() {
    say(L"Testing killring");

    do_test(kill_entries().empty());

    kill_add(L"a");
    kill_add(L"b");
    kill_add(L"c");

    do_test((kill_entries() == wcstring_list_t{L"c", L"b", L"a"}));

    do_test(kill_yank_rotate() == L"b");
    do_test((kill_entries() == wcstring_list_t{L"b", L"a", L"c"}));

    do_test(kill_yank_rotate() == L"a");
    do_test((kill_entries() == wcstring_list_t{L"a", L"c", L"b"}));

    kill_add(L"d");

    do_test((kill_entries() == wcstring_list_t{L"d", L"a", L"c", L"b"}));

    do_test(kill_yank_rotate() == L"a");
    do_test((kill_entries() == wcstring_list_t{L"a", L"c", L"b", L"d"}));
}

struct termsize_tester_t {
    static void test();
};

void termsize_tester_t::test() {
    say(L"Testing termsize");

    parser_t &parser = parser_t::principal_parser();
    env_stack_t &vars = parser.vars();

    // Use a static variable so we can pretend we're the kernel exposing a terminal size.
    static maybe_t<termsize_t> stubby_termsize{};
    termsize_container_t ts([] { return stubby_termsize; });

    // Initially default value.
    do_test(ts.last() == termsize_t::defaults());

    // Haha we change the value, it doesn't even know.
    stubby_termsize = termsize_t{42, 84};
    do_test(ts.last() == termsize_t::defaults());

    // Ok let's tell it. But it still doesn't update right away.
    ts.handle_winch();
    do_test(ts.last() == termsize_t::defaults());

    // Ok now we tell it to update.
    ts.updating(parser);
    do_test(ts.last() == *stubby_termsize);
    do_test(vars.get(L"COLUMNS")->as_string() == L"42");
    do_test(vars.get(L"LINES")->as_string() == L"84");

    // Wow someone set COLUMNS and LINES to a weird value.
    // Now the tty's termsize doesn't matter.
    vars.set(L"COLUMNS", ENV_GLOBAL, {L"75"});
    vars.set(L"LINES", ENV_GLOBAL, {L"150"});
    ts.handle_columns_lines_var_change(vars);
    do_test(ts.last() == termsize_t(75, 150));
    do_test(vars.get(L"COLUMNS")->as_string() == L"75");
    do_test(vars.get(L"LINES")->as_string() == L"150");

    vars.set(L"COLUMNS", ENV_GLOBAL, {L"33"});
    ts.handle_columns_lines_var_change(vars);
    do_test(ts.last() == termsize_t(33, 150));

    // Oh it got SIGWINCH, now the tty matters again.
    ts.handle_winch();
    do_test(ts.last() == termsize_t(33, 150));
    do_test(ts.updating(parser) == *stubby_termsize);
    do_test(vars.get(L"COLUMNS")->as_string() == L"42");
    do_test(vars.get(L"LINES")->as_string() == L"84");

    // Test initialize().
    vars.set(L"COLUMNS", ENV_GLOBAL, {L"83"});
    vars.set(L"LINES", ENV_GLOBAL, {L"38"});
    ts.initialize(vars);
    do_test(ts.last() == termsize_t(83, 38));

    // initialize() even beats the tty reader until a sigwinch.
    termsize_container_t ts2([] { return stubby_termsize; });
    ts.initialize(vars);
    ts2.updating(parser);
    do_test(ts.last() == termsize_t(83, 38));
    ts2.handle_winch();
    do_test(ts2.updating(parser) == *stubby_termsize);
}

// typedef void (test_entry_point_t)();
using test_entry_point_t = void (*)();
struct test_t {
    const char *group;
    std::function<void()> test;
    bool opt_in = false;

    test_t(const char *group, test_entry_point_t test, bool opt_in = false)
        : group(group), test(test), opt_in(opt_in) {}
};

struct test_comparator_t {
    // template<typename T=test_t>
    int operator()(const test_t &lhs, const test_t &rhs) { return strcmp(lhs.group, rhs.group); }
};

// This magic string is required for CMake to pick up the list of tests
#define TEST_GROUP(x) x
static const test_t s_tests[]{
    {TEST_GROUP("utility_functions"), test_utility_functions},
    {TEST_GROUP("string_split"), test_split_string_tok},
    {TEST_GROUP("wwrite_to_fd"), test_wwrite_to_fd},
    {TEST_GROUP("env_vars"), test_env_vars},
    {TEST_GROUP("env"), test_env_snapshot},
    {TEST_GROUP("str_to_num"), test_str_to_num},
    {TEST_GROUP("enum"), test_enum_set},
    {TEST_GROUP("enum"), test_enum_array},
    {TEST_GROUP("highlighting"), test_highlighting},
    {TEST_GROUP("new_parser_ll2"), test_new_parser_ll2},
    {TEST_GROUP("new_parser_fuzzing"), test_new_parser_fuzzing},
    {TEST_GROUP("new_parser_correctness"), test_new_parser_correctness},
    {TEST_GROUP("new_parser_ad_hoc"), test_new_parser_ad_hoc},
    {TEST_GROUP("new_parser_errors"), test_new_parser_errors},
    {TEST_GROUP("error_messages"), test_error_messages},
    {TEST_GROUP("escape"), test_unescape_sane},
    {TEST_GROUP("escape"), test_escape_crazy},
    {TEST_GROUP("escape"), test_escape_quotes},
    {TEST_GROUP("format"), test_format},
    {TEST_GROUP("convert"), test_convert},
    {TEST_GROUP("convert"), test_convert_private_use},
    {TEST_GROUP("convert_ascii"), test_convert_ascii},
    {TEST_GROUP("perf_convert_ascii"), perf_convert_ascii, true},
    {TEST_GROUP("convert_nulls"), test_convert_nulls},
    {TEST_GROUP("tokenizer"), test_tokenizer},
    {TEST_GROUP("fd_monitor"), test_fd_monitor},
    {TEST_GROUP("iothread"), test_iothread},
    {TEST_GROUP("pthread"), test_pthread},
    {TEST_GROUP("debounce"), test_debounce},
    {TEST_GROUP("debounce"), test_debounce_timeout},
    {TEST_GROUP("parser"), test_parser},
    {TEST_GROUP("cancellation"), test_cancellation},
    {TEST_GROUP("indents"), test_indents},
    {TEST_GROUP("utf8"), test_utf8},
    {TEST_GROUP("feature_flags"), test_feature_flags},
    {TEST_GROUP("escape_sequences"), test_escape_sequences},
    {TEST_GROUP("pcre2_escape"), test_pcre2_escape},
    {TEST_GROUP("lru"), test_lru},
    {TEST_GROUP("expand"), test_expand},
    {TEST_GROUP("expand"), test_expand_overflow},
    {TEST_GROUP("fuzzy_match"), test_fuzzy_match},
    {TEST_GROUP("ifind"), test_ifind},
    {TEST_GROUP("ifind_fuzzy"), test_ifind_fuzzy},
    {TEST_GROUP("abbreviations"), test_abbreviations},
    {TEST_GROUP("builtin_test"), test_test},
    {TEST_GROUP("wcstod"), test_wcstod},
    {TEST_GROUP("dup2s"), test_dup2s},
    {TEST_GROUP("dup2s"), test_dup2s_fd_for_target_fd},
    {TEST_GROUP("path"), test_path},
    {TEST_GROUP("pager_navigation"), test_pager_navigation},
    {TEST_GROUP("pager_layout"), test_pager_layout},
    {TEST_GROUP("word_motion"), test_word_motion},
    {TEST_GROUP("is_potential_path"), test_is_potential_path},
    {TEST_GROUP("colors"), test_colors},
    {TEST_GROUP("complete"), test_complete},
    {TEST_GROUP("autoload"), test_autoload},
    {TEST_GROUP("input"), test_input},
    {TEST_GROUP("line_iterator"), test_line_iterator},
    {TEST_GROUP("undo"), test_undo},
    {TEST_GROUP("universal"), test_universal},
    {TEST_GROUP("universal"), test_universal_output},
    {TEST_GROUP("universal"), test_universal_parsing},
    {TEST_GROUP("universal"), test_universal_parsing_legacy},
    {TEST_GROUP("universal"), test_universal_callbacks},
    {TEST_GROUP("universal"), test_universal_formats},
    {TEST_GROUP("universal"), test_universal_ok_to_save},
    {TEST_GROUP("notifiers"), test_universal_notifiers},
    {TEST_GROUP("wait_handles"), test_wait_handles},
    {TEST_GROUP("completion_insertions"), test_completion_insertions},
    {TEST_GROUP("autosuggestion_ignores"), test_autosuggestion_ignores},
    {TEST_GROUP("autosuggestion_combining"), test_autosuggestion_combining},
    {TEST_GROUP("autosuggest_suggest_special"), test_autosuggest_suggest_special},
    {TEST_GROUP("history"), history_tests_t::test_history},
    {TEST_GROUP("history_merge"), history_tests_t::test_history_merge},
    {TEST_GROUP("history_paths"), history_tests_t::test_history_path_detection},
    {TEST_GROUP("history_races"), history_tests_t::test_history_races},
    {TEST_GROUP("history_formats"), history_tests_t::test_history_formats},
    {TEST_GROUP("string"), test_string},
    {TEST_GROUP("illegal_command_exit_code"), test_illegal_command_exit_code},
    {TEST_GROUP("maybe"), test_maybe},
    {TEST_GROUP("layout_cache"), test_layout_cache},
    {TEST_GROUP("prompt"), test_prompt_truncation},
    {TEST_GROUP("normalize"), test_normalize_path},
    {TEST_GROUP("dirname"), test_dirname_basename},
    {TEST_GROUP("topics"), test_topic_monitor},
    {TEST_GROUP("topics"), test_topic_monitor_torture},
    {TEST_GROUP("pipes"), test_pipes},
    {TEST_GROUP("fd_event"), test_fd_event_signaller},
    {TEST_GROUP("timer_format"), test_timer_format},
    {TEST_GROUP("termsize"), termsize_tester_t::test},
    {TEST_GROUP("killring"), test_killring},
};

void list_tests() {
    std::set<std::string> groups;
    for (const auto &test : s_tests) {
        groups.insert(test.group);
    }

    for (const auto &group : groups) {
        std::fprintf(stdout, "%s\n", group.c_str());
    }
}

/// Main test.
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");

    if (argc >= 2 && std::strcmp(argv[1], "--list") == 0) {
        list_tests();
        return 0;
    }

    // Look for the file tests/test.fish. We expect to run in a directory containing that file.
    // If we don't find it, walk up the directory hierarchy until we do, or error.
    while (access("./tests/test.fish", F_OK) != 0) {
        char wd[PATH_MAX + 1] = {};
        if (!getcwd(wd, sizeof wd)) {
            perror("getcwd");
            exit(-1);
        }
        if (!std::strcmp(wd, "/")) {
            std::fwprintf(
                stderr, L"Unable to find 'tests' directory, which should contain file test.fish\n");
            exit(EXIT_FAILURE);
        }
        if (chdir(dirname(wd)) < 0) {
            perror("chdir");
        }
    }

    srandom((unsigned int)time(nullptr));
    configure_thread_assertions_for_testing();

    // Set the program name to this sentinel value
    // This will prevent some misleading stderr output during the tests
    program_name = TESTS_PROGRAM_NAME;
    s_arguments = argv + 1;

    struct utsname uname_info;
    uname(&uname_info);

    say(L"Testing low-level functionality");
    set_main_thread();
    setup_fork_guards();
    proc_init();
    builtin_init();
    env_init();
    misc_init();
    reader_init();

    // Set default signal handlers, so we can ctrl-C out of this.
    signal_reset_handlers();

    // Set PWD from getcwd - fixes #5599
    env_stack_t::principal().set_pwd_from_getcwd();

    for (const auto &test : s_tests) {
        if (should_test_function(test.group)) {
            test.test();
        }
    }

    say(L"Encountered %d errors in low-level tests", err_count);
    if (s_test_run_count == 0) say(L"*** No Tests Were Actually Run! ***");

    if (err_count != 0) {
        return 1;
    }
}
