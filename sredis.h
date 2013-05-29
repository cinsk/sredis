#ifndef SREDIS_H__
#define SREDIS_H__

#include <sys/time.h>
#ifdef _PTHREAD
#include <pthread.h>
#endif

#include "hiredis/hiredis.h"
#include "xerror.h"

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


#define REDIS_HOSTS_MAX         16

struct redis_hostent {
  const char *host;
  int port;
  struct timeval o_timeout;
  struct timeval c_timeout;

  unsigned short success;
  unsigned short failure;
};

struct REDIS_ {
  redisContext *ctx;

  struct redis_hostent *hosts[REDIS_HOSTS_MAX];
  int chost;                    /* index to the current host in HOSTS  */

  size_t stacked;

  short ver_major;
  short ver_minor;
  // short ver_tiny;

#ifdef _PTHREAD
  pthread_mutex_t mutex;
#endif
};
typedef struct REDIS_ REDIS;

#ifdef _PTHREAD
# define redis_lock(redis)       do {                   \
  int ret = pthread_mutex_lock(&(redis)->mutex);        \
  if (ret)                                              \
    xdebug(ret, "pthread_mutex_lock() failed");         \
  } while (0)
# define redis_unlock(redis)     do {                   \
  int ret = pthread_mutex_unlock(&(redis)->mutex);      \
  if (ret)                                              \
    xdebug(ret, "pthread_mutex_unlock() failed");       \
  } while (0)

static __inline__ int
redis_trylock(REDIS *redis)
{
  int ret = pthread_mutex_trylock(&(redis)->mutex);
  if (ret && ret != EBUSY)
    xdebug(ret, "pthread_mutex_trylock() failed");
  return ret;
}

#else
# define redis_lock(redis)       (void)0
# define redis_unlock(redis)     (void)0
# define redis_trylock(redis)    (EINVAL)
#endif  /* _PTHREAD */

/*
 * Create and allocate REDIS structure.
 */
REDIS *redis_new(void);

/*
 * Create and allocate REDIS structure, plus
 * bind it to the given connection information.
 *
 * See redis_host_add() for the description of the pamameters
 */
REDIS *redis_open(const char *host, int port,
                  const struct timeval *c_timeout,
                  const struct timeval *o_timeout);

/*
 * Close and deallocate REDIS structure.
 */
void redis_close(REDIS *redis);

/* release the current redis context, and remove all hosts information
 * on REDIS.  Note that after calling this function, you should call
 * redis_host_add() to add new hosts. */
int redis_shutdown(REDIS *redis);

/*
 * Add new connection information to the REDIS structure.
 *
 * host:  address of the redis server.
 * port:  port of the redis server,
 * c_timeout:  If non-null, connection failed if
 *             it took more than c_timeout. See redisConnectWithTimeout()
 * o_timeout: If non-null, all redis operation should be finished
 *            before this time.  See redisSetTimeout().
 *
 * Returns the index to the internal structure, which is zero or postive.
 *
 * On failure, it returns -1.
 */
int redis_host_add(REDIS *redis, const char *host, int port,
                   const struct timeval *c_timeout,
                   const struct timeval *o_timeout);

/*
 * Delete the connection infomation by the index.
 */
int redis_host_del(REDIS *redis, int index);

/*
 * Close the current connection, then establish new connection (to the
 * new master node) using connection information registered by
 * redis_host_add().
 *
 * Normally, you don't need to call this function.  Either
 * redis_command() or redis_append() will call it implicitly when
 * disconnected.
 */
int redis_reopen(REDIS *redis);
int redis_reopen_unlocked(REDIS *rd);

/*
 * A wrapper to redisCommand().
 *
 * If the previous redis operation failed, it will establish new
 * connection by calling redis_reopen(), then request the operation.
 *
 * Note that this does not mean that redis_command() will always
 * succeed.
 */
redisReply *redis_command(REDIS *redis, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));
redisReply *redis_command_unlocked(REDIS *redis, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));

/*
 * Similar to redis_command() but this function will just fail if the
 * current connection is bad.
 */
redisReply *redis_command_fast(REDIS *redis, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));
redisReply *redis_command_fast_unlocked(REDIS *redis, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));


/*
 * A wrapper to redisAppendCommand().
 *
 * Queue multiple redis commands for execution (e.g. pipeline or
 * transation)
 */
int redis_append(REDIS *redis, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));
int redis_append_unlocked(REDIS *redis, const char *format, ...)
  __attribute__ ((format (printf, 2, 3)));

/*
 * Execute queued commands from one or more redis_append() calls.
 *
 * Unlike redisGetReply() of the hiredis, No matter how many you
 * called redis_append(), you need to call redis_exec() just once.
 *
 * The return value will have the type REDIS_RESPONSE_ARRAY, which
 * contains each result of the redis_append() calls.
 */
redisReply *redis_exec(REDIS *redis);
redisReply *redis_exec_unlocked(REDIS *redis);

/*
 * Release redisReply struct.
 *
 * Note that it is okay to pass NULL to this function.  This way, you
 * always call redis_free() after calling either redis_command() or
 * redis_exec().
 */
void redis_free(redisReply *reply);

/*
 * Check if the redisReply from either redis_command() or redis_exec()
 * whether the reply failed.
 */
#define redis_iserror(reply)    ((!reply) || reply->type == REDIS_REPLY_ERROR)

/*
 * Dump the REDISREPLY structure (for debugging purpose)
 */
void redis_dump_reply(const redisReply *reply,
                      const char *prefix, int indent);

/*
 * Convenient function to get the integer value from the redisReply.
 *
 * This function will try to convert REPLY at best effort.  i.e. It
 * will try to parse REPLY even if REPLY is string type, using strtoll() or
 * strtold().
 *
 * On error, it returns zero, and errno is set.  If the input is
 * invalid (neither integer nor string, invalid string value, etc.),
 * errno is set to EINVAL.  If underflow or overflow detected, errno
 * is set to ERANGE.
 */
long long redis_reply_integer(redisReply *reply);

END_C_DECLS

#endif  /* SREDIS_H__ */
