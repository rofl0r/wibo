#ifndef WIBO_TEST_ASSERT_H
#define WIBO_TEST_ASSERT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FAIL(fmt, ...) \
    do { \
        fprintf(stderr, "FAIL:%s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        exit(EXIT_FAILURE); \
    } while (0)

#define TEST_CHECK(cond) \
    do { \
        if (!(cond)) { \
            TEST_FAIL("Assertion '%s' failed", #cond); \
        } \
    } while (0)

#define TEST_CHECK_MSG(cond, fmt, ...) \
    do { \
        if (!(cond)) { \
            TEST_FAIL(fmt, ##__VA_ARGS__); \
        } \
    } while (0)

// __stdcall exports are name-decorated only on i386 (name@bytes); x64 uses the bare name.
#if defined(_WIN64) || defined(__x86_64__)
#define STDCALL_EXPORT_NAME(name, bytes) #name
#else
#define STDCALL_EXPORT_NAME(name, bytes) #name "@" #bytes
#endif

#define TEST_CHECK_EQ(expected, actual) \
    do { \
        long long _expected_value = (long long)(expected); \
        long long _actual_value = (long long)(actual); \
        if (_expected_value != _actual_value) { \
            TEST_FAIL("Expected %s (%lld) == %s (%lld)", \
                      #expected, _expected_value, #actual, _actual_value); \
        } \
    } while (0)

#define TEST_CHECK_U64_EQ(expected, actual) \
    do { \
        unsigned long long _expected_value = (unsigned long long)(expected); \
        unsigned long long _actual_value = (unsigned long long)(actual); \
        if (_expected_value != _actual_value) { \
            TEST_FAIL("Expected %s (%llu) == %s (%llu)", \
                      #expected, _expected_value, #actual, _actual_value); \
        } \
    } while (0)

#define TEST_CHECK_STR_EQ(expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            TEST_FAIL("Expected %s (\"%s\") == %s (\"%s\")", \
                      #expected, (expected), #actual, (actual)); \
        } \
    } while (0)

#endif
