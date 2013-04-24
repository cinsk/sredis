#ifndef XERROR_H__
#define XERROR_H__

#ifndef __GNUC__
#error GCC is required to use this header
#endif

/*
 * This header provides simple error message printing functions,
 * which is almost duplicated version of error in GLIBC.
 *
 * Works in Linux and MacOS.
 */

#include <stdarg.h>
#include <stdio.h>

/* This indirect using of extern "C" { ... } makes Emacs happy */
#ifndef BEGIN_C_DECLS
# ifdef __cplusplus
#  define BEGIN_C_DECLS extern "C" {
#  define END_C_DECLS   }
# else
#  define BEGIN_C_DECLS
#  define END_C_DECLS
# endif
#endif /* BEGIN_C_DECLS */

BEGIN_C_DECLS

/*
 * xerror() is the same as error() in GLIBC.
 */
extern void xerror(int status, int code, const char *format, ...)
  __attribute__((format (printf, 3, 4)));

/*
 * xdebug(...) is like xerror(0, ...), except that it embeds the
 * caller's filename and line number in the messages.  The output will
 * not be generated if the application defined 'debug_mode' to zero.
 *
 * By default, 'debug_mode' is set to zero.
 */
#define xdebug(code, fmt, ...)                                          \
    xdebug_((code), ("%s:%d: " fmt), __FILE__, __LINE__, ## __VA_ARGS__)

/*
 * Return nonzero if 'debug_mode' is nonzero.
 */
extern int xifdebug(void);

extern void xdebug_(int code, const char *format, ...)
  __attribute__((format (printf, 2, 3)));

extern void xmessage(int progname, int code, const char *format, va_list ap);

/*
 * By default, all x*() functions will send the output to STDERR.
 * You can override the output stream using xerror_redirect().
 *
 * This function returns the previous output stream if any.
 *
 * Note that if you didn't set explicitly the output stream to STDERR,
 * this function will return NULL.  This may be helpful if you want to
 * close the previous output stream except STDERR.
 */
extern FILE *xerror_redirect(FILE *fp);

/*
 * Register one or more signals to generate backtrace if the program
 * receives signals.  Note that the last argument should be zero.
 */
extern int xbacktrace_on_signals(int signo, ...);

END_C_DECLS

#endif  /* XERROR_H__ */
