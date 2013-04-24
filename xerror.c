#define _GNU_SOURCE     1
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <stdint.h>

#define NO_MCONTEXT
#ifndef NO_MCONTEXT
# ifdef __APPLE__
/* MacOSX deprecates <ucontext.h> */
#  include <sys/ucontext.h>
# else
#  include <ucontext.h>
# endif
#endif  /* NO_MCONTEXT */

#include <unistd.h>
#include <signal.h>
#include <execinfo.h>

#ifndef __GLIBC__
/* In GLIBC, <string.h> will provide better basename(3). */
#include <libgen.h>
#endif

#include "xerror.h"

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libgen.h>
#include <limits.h>
#endif

#define BACKTRACE_MAX   16

/* glibc compatible name */
const char *program_name __attribute__((weak)) = 0;

int debug_mode __attribute__((weak));
int backtrace_mode __attribute__((weak)) = 1;

static void set_program_name(void) __attribute__((constructor));

static FILE *xerror_stream = (FILE *)-1;

static void bt_handler(int signo, siginfo_t *info, void *uctx_void);

FILE *
xerror_redirect(FILE *fp)
{
  FILE *old = xerror_stream;

  assert(fp != NULL);

  if (old == (FILE *)-1)
    old = NULL;

  xerror_stream = fp;

  return old;
}


#ifdef __APPLE__
static void
darwin_program_name(void)
{
  static char namebuf[PATH_MAX];
  uint32_t bufsize = PATH_MAX;
  int ret;

  ret = _NSGetExecutablePath(namebuf, &bufsize);
  if (ret == 0) {
    program_name = basename(namebuf);
  }
}
#endif  /* __APPLE__ */


static void
set_program_name(void)
{
#ifdef __APPLE__
  darwin_program_name();
#elif defined(__GLIBC__)
  program_name = basename(program_invocation_short_name);
#endif

}


int
xbacktrace_on_signals(int signo, ...)
{
  struct sigaction act;
  va_list ap;

  int ret = 0;

  memset(&act, 0, sizeof(act));

  act.sa_sigaction = bt_handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_SIGINFO;

  ret = sigaction(signo, &act, NULL);
  if (ret != 0) {
    xerror(0, errno, "can't register a handler for signal %d", signo);
    return -1;
  }

  va_start(ap, signo);
  while ((signo = (int)va_arg(ap, int)) != 0) {
    ret = sigaction(signo, &act, NULL);
    if (ret != 0) {
      xerror(0, errno, "can't register a handler for signal %d", signo);
      va_end(ap);
      return -1;
    }
  }
  va_end(ap);
  return 0;
}

void
xerror(int status, int code, const char *format, ...)
{
  va_list ap;

  va_start(ap, format);
  xmessage(1, code, format, ap);
  va_end(ap);

  if (status)
    exit(status);
}


int
xifdebug()
{
  return debug_mode;
}


void
xdebug_(int code, const char *format, ...)
{
  va_list ap;

  if (!debug_mode)
    return;

  va_start(ap, format);
  xmessage(0, code, format, ap);
  va_end(ap);
}


void
xmessage(int progname, int code, const char *format, va_list ap)
{
  char errbuf[BUFSIZ];

  if (xerror_stream == (FILE *)-1)
    xerror_stream = stderr;

  if (!xerror_stream)
    return;

  fflush(stdout);
  fflush(xerror_stream);

  flockfile(xerror_stream);

  if (progname && program_name)
    fprintf(xerror_stream, "%s: ", program_name);

  vfprintf(xerror_stream, format, ap);

  if (code) {
    if (strerror_r(code, errbuf, BUFSIZ) == 0)
      fprintf(xerror_stream, ": %s", errbuf);
  }
  fputc('\n', xerror_stream);

  funlockfile(xerror_stream);
}


static void
bt_handler(int signo, siginfo_t *info, void *uctx_void)
{
  void *trace[BACKTRACE_MAX];
  int ret;
  ucontext_t *uctx = (ucontext_t *)uctx_void;

  (void)uctx;

  if (!backtrace_mode)
    return;

  {
#ifndef NO_MCONTEXT
# ifdef __APPLE__
    uint64_t pc = uctx->uc_mcontext->__ss.__rip;
    xerror(0, 0, "Got signal (%d) at address %8p, PC=[%08llx]", signo,
           info->si_addr, pc);
# else  /* linux */
    greg_t pc = uctx->uc_mcontext.gregs[REG_EIP];
    xerror(0, 0, "Got signal (%d) at address %8lx, PC=[%08x]", signo,
           (long) info->si_addr, pc);
# endif
#else
    xerror(0, 0, "Got signal (%d) at address %08lx", signo,
           (long) info->si_addr);
#endif  /* NO_MCONTEXT */
  }

  ret = backtrace(trace, BACKTRACE_MAX);
  /* TODO: error check on backtrace(3)? */

  fflush(xerror_stream);
  backtrace_symbols_fd(trace, ret, fileno(xerror_stream));
  fflush(xerror_stream);

  /* http://tldp.org/LDP/abs/html/exitcodes.html */
  exit(128 + signo);
}


#ifdef _TEST_XERROR
#include <errno.h>

int debug_mode = 1;

static void bar(int a)
{
  unsigned char *p = 0;
  *p = 3;                       /* SIGSEGV */
}

void foo(int a, int b)
{
  bar(a);
}

int
main(int argc, char *argv[])
{
  xbacktrace_on_signals(SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGTRAP, 0);

  xdebug(0, "program_name = %s\n", program_name);

  if (argc != 2)
    xerror(1, 0, "argument required, argc = %d", argc);

  xerror(0, EINVAL, "invalid argv[1] = %s", argv[1]);

  foo(1, 3);
  return 0;
}

#endif  /* _TEST_XERROR */
