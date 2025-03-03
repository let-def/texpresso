#ifndef UTILS_H_
#define UTILS_H_

#include <stdlib.h>
#include <stdio.h>

/**
 * @brief Logging helper functions for debugging and error handling.
 */

// Is logging enabled
extern int logging;

/**
 * @brief Prints a backtrace to stderr.
 *
 * This function captures the current call stack and prints it to the standard error output.
 * It uses the `backtrace` and `backtrace_symbols` functions to retrieve and format the stack trace.
 */
void print_backtrace(void);

/**
 * @brief Aborts the program and prints a backtrace.
 */
#define do_abort() \
    do { print_backtrace(); abort(); } while(0)

#define do_abortf(...) do_abortf_(__VA_ARGS__)
#define do_abortf_(fmt, ...) \
    do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); print_backtrace(); abort(); } while(0)

/**
 * @brief Stringification macro.
 *
 * STR(x) expands macros in x and turn the result into a string
 */
#define STR(x) STR_(x)
#define STR_(x) #x

/**
 * @brief Logs entry to a (long-running) function
 *
 * @param kind  Boolean value that determines whether the log should be printed.
 * @param fmt   Format string for the function arguments.
 * @param ...   Arguments to be logged.
 */
#define log_proc(kind, ...) log_proc_(kind, __VA_ARGS__)
#define log_proc_(kind, fmt, ...) \
    do { if (kind) fprintf(stderr, "%s(" fmt ")\n", __func__, ##__VA_ARGS__); } while (0)

/**
 * @brief Logs entry to a leaf function
 *
 * @param kind  Boolean value that determines whether the log should be printed.
 * @param fmt   Format string for the function arguments.
 * @param ...   Arguments to be logged.
 */
#define log_func(kind, ...) log_func_(kind, __VA_ARGS__)
#define log_func_(kind, fmt, ...) \
    do { if (kind) fprintf(stderr, "%s(" fmt ")", __func__, ##__VA_ARGS__); } while(0)

/**
 * @brief Logs function result with a format string.
 *
 * @param kind  Boolean value that determines whether the log should be printed.
 * @param fmt   Format string for the result.
 * @param ...   Result to be logged.
 */
#define log_result(kind, ...) log_result_(kind, __VA_ARGS__)
#define log_result_(kind, fmt, ...) \
    do { if (kind) fprintf(stderr, " = " fmt "\n", ##__VA_ARGS__); } while(0)

/**
 * Construct a cache path based on the provided folder and name.
 *
 * This function constructs a cache path by combining the folder and name with
 * a base path derived from the XDG_CACHE_HOME or HOME environment variables.
 * It ensures that the directory structure exists and is properly normalized.
 *
 * @param folder The subdirectory name within the cache path.
 * @param name The file name within the cache path.
 * @return The constructed cache path, or NULL if an error occurs.
 *         The returned buffer is managed by the function and valid until the
 *         next call.
 */
const char *cache_path_(const char *folder, const char *name[]);
#define cache_path(folder, ...) cache_path_(folder, (const char*[]){__VA_ARGS__, NULL})

#endif // UTILS_H_
