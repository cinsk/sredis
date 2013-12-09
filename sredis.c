#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <errno.h>

#include "sredis.h"


#ifndef FALSE
#define FALSE   0
#define TRUE    (!FALSE)
#endif

#define ERR_READONLY    "READONLY"

#define ENDPOINT_DELIMS " \t\v\n\r"
#define REDIS_INFO_DELIMS       "\r\n"
#define REDIS_INFOENT_DELIMS       ":"

#define REDIS_INFO_VERSION      "redis_version"
#define REDIS_INFO_ROLE         "role"
#define REDIS_INFO_MASTER_HOST  "master_host"
#define REDIS_INFO_MASTER_PORT  "master_port"

#define MARK_FAILED(rd)        do {      \
    if ((rd)->chost >= 0)                \
    (rd)->hosts[(rd)->chost]->failure++; \
  } while (0)

#define MARK_SUCCESS(rd)        do {     \
    if ((rd)->chost >= 0)                \
    (rd)->hosts[(rd)->chost]->success++; \
  } while (0)

#define N_FAILED(rd)    (((rd)->chost >= 0) ? \
                         (rd)->hosts[(rd)->chost]->failure : 0)

#define N_SUCCEEDED(rd) (((rd)->chost >= 0) ? \
                         (rd)->hosts[(rd)->chost]->success : 0)

struct repldata {
  int master;
  char *host;
  int port;
};

typedef int (*redis_info_handler)(REDIS *redis,
                                  char *section,
                                  char *field,
                                  char *value,
                                  void *data);

static int redis_parse_version(REDIS *rd);

static struct redis_hostent *redis_get_host(REDIS *rd, int index);
static struct redis_hostent *redis_next_host(REDIS *rd);
static redisReply *redis_vcommand(REDIS *redis, int reopen,
                                  const char *format, va_list ap);
static redisContext *redis_context(struct redis_hostent *endpoint);
static struct redis_hostent *redis_get_hostent_create(REDIS *redis,
                                                      const char *host,
                                                      int port);
static int redis_parse_info(REDIS *rd, redis_info_handler handler, void *data);

//static int redis_get_info(REDIS *rd);
static int redis_find_master(REDIS *redis, struct redis_hostent **ent);
static int redis_find_master_24(REDIS *redis, struct redis_hostent **ent);
static int redis_find_master_26(REDIS *redis, struct redis_hostent **ent);


static inline int
REDIS_IS_HIGHER(REDIS *rd, short major, short minor)
{
  int dst = major;
  int src = rd->ver_major;

  dst <<= sizeof(major) * CHAR_BIT;
  dst += minor;

  src <<= sizeof(minor) * CHAR_BIT;
  src += rd->ver_minor;

  return src - dst;
}


static int
redis_parse_master_handler(REDIS *rd,
                           char *section,
                           char *field,
                           char *value,
                           void *data)
{
  struct repldata *p = (struct repldata *)data;

  (void)rd;
  (void)section;

  if (strcmp(field, REDIS_INFO_ROLE) == 0) {
    if (strcasecmp(value, "slave") == 0)
      p->master = FALSE;
    else
      p->master = TRUE;
  }
  else if (strcmp(field, REDIS_INFO_MASTER_HOST) == 0) {
    if (p->host)
      free(p->host);
    p->host = strdup(value);
  }
  else if (strcmp(field, REDIS_INFO_MASTER_PORT) == 0) {
    p->port = atoi(value);
  }
  return 1;
}

/*
 * redis_find_master*(), all these functions returns -1 on failure,
 * and returns zero on success.  Success means that it found the
 * master.
 *
 * If the current connected one is the master, *ENT will be set to
 * zero.  Otherwise, *ENT will be set to the new master endpoint.
 */
static int
redis_find_master(REDIS *redis, struct redis_hostent **ent)
{
  assert(REDIS_IS_HIGHER(redis, 2, 4) >= 0);

  if (REDIS_IS_HIGHER(redis, 2, 6) >= 0)
    return redis_find_master_26(redis, ent);
  else
    return redis_find_master_24(redis, ent);
}


static int
redis_find_master_24(REDIS *redis, struct redis_hostent **ent)
{
  struct repldata data = { 0, NULL, 0 };
  struct redis_hostent *ret = NULL;
  int s;

  /* redis_parse_info() will allocate data->host to point the master
   * host if it returns zero, so you need to free() it later. */
  s = redis_parse_info(redis, redis_parse_master_handler, &data);
  if (s < 0)
    return -1;                  /* couldn't find the master */

  if (data.master) {
    free(data.host);
    *ent = 0;
    return 0;                   /* found, this is the master */
  }
  else {                        /* found. */
    ret = redis_get_hostent_create(redis, data.host, data.port);

    free(data.host);

    if (ret) {                  /* found, registered */
      *ent = ret;
      return 0;
    }
    else {                      /* found, but couldn't register */
      *ent = 0;
      return -1;
    }
  }
}


static int
redis_find_master_26(REDIS *redis, struct redis_hostent **ent)
{
  char *endpoint;
  char *tok, *saveptr, *addr;
  int port;
  int retval = -1;
  struct redis_hostent *ret = NULL;
  redisReply *reply = redis_command_fast(redis, "CONFIG GET slaveof");

  if (!reply) {                 /* can't connect to the current server */
    xdebug(0, "can't connect to the redis server");
    goto fin;
  }

  if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
    xdebug(0, "unexpected redis response type(%d), elms(%zd)",
           reply->type, reply->elements);
    goto fin;
  }
  if (reply->element[1]->type != REDIS_REPLY_STRING) {
    xdebug(0, "unexpected redis response type(%d)", reply->element[1]->type);
    goto fin;
  }

  endpoint = reply->element[1]->str;

  tok = strtok_r(endpoint, ENDPOINT_DELIMS, &saveptr);
  if (!tok) {                   /* this is the master, nothing to do */
    ret = NULL;
    retval = 0;
  }
  else {
    addr = tok;
    tok = strtok_r(NULL, ENDPOINT_DELIMS, &saveptr);
    if (!tok) {                 /* no port?? */
      xdebug(0, "CONFIG GET slaveof returns no port, finding master failed");
      goto fin;
    }
    port = atoi(tok);

    ret = redis_get_hostent_create(redis, addr, port);
    if (ret)
      retval = 0;
  }

 fin:
  *ent = ret;
  redis_free(reply);
  return retval;
}

/*
 * Add the new endpoint to the REDIS::hosts, and return the address of
 * the entry.  If adding failed (e.g. out of slot), it returns NULL.
 */
static struct redis_hostent *
redis_get_hostent_create(REDIS *redis, const char *host, int port)
{
  int i;
  struct redis_hostent *p;

  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    /* In this for loop, we try to determine whether the master
     * endpoint is already registered in redis->hosts[]. */
    p = redis->hosts[i];
    if (p == NULL)
      continue;

    if (strcmp(host, p->host) == 0 && port == p->port)
      return p;
  }

  p = redis->hosts[redis->chost];
  assert(p != NULL);            /* TODO: is this necessary? */

  i = redis_host_add(redis, host, port, &p->c_timeout, &p->o_timeout);
  if (i < 0)
    return NULL;                /* error: no more space in redis->hosts[] */

  /* TODO: Is this right approach? I mean, to add new master endpoint in
   *       redis->hosts[]. */

  /* TODO: I don't know whether it is right to update redis->chost to the
   *       new master index.   If we have endpoints at index 0, 1, and 2.
   *       and suppose redis->chost is 0.  If we add new master, it will
   *       have index to 3.  So if this failed, we'll start to find new
   *       one at index zero.  This mean, the endpoint at index 1 and 2
   *       is not tried at first iteration. */
  redis->chost = i;

  return redis->hosts[i];
}


static struct redis_hostent *
redis_get_host(REDIS *rd, int index)
{
  assert(index >= 0 && index < REDIS_HOSTS_MAX);
  return (rd->hosts[index]);
}

static struct redis_hostent *
redis_next_host(REDIS *rd)
{
  int i, j;
  int pos;

  /* For maintainers:  please update the comments in struct REDIS_ in
   * <sredis.h> whenever you update this function. */

  assert(rd != NULL);

  pos = (rd->chost == -1) ? 0 : (rd->chost + 1) % REDIS_HOSTS_MAX;

  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    j = (pos + i) % REDIS_HOSTS_MAX;

    if (rd->hosts[j]) {
      rd->chost = j;
      return rd->hosts[j];
    }
  }

  return NULL;
}


static redisContext *
redis_context(struct redis_hostent *ent)
{
  redisContext *ctx;

  if (ent->c_timeout.tv_sec == 0 && ent->c_timeout.tv_usec == 0)
    ctx = redisConnect(ent->host, ent->port);
  else
    ctx = redisConnectWithTimeout(ent->host, ent->port, ent->c_timeout);

  if (ctx == NULL || ctx->err) {
    if (ctx) {
      xerror(0, 0, "connection error: [%s:%d] %s",
             ent->host, ent->port, ctx->errstr);
      redisFree(ctx);
      ctx = NULL;
    }
    else
      xerror(0, 0, "connection error: can't allocate redis context");
  }
  else {
    if (ent->o_timeout.tv_sec != 0 || ent->o_timeout.tv_usec != 0)
      redisSetTimeout(ctx, ent->o_timeout);
  }
  return ctx;
}


static int
redis_parse_info(REDIS *rd, redis_info_handler handler, void *data)
{
  redisReply *reply;
  char *tok, *saveptr;
  char *p, *name, *value;
  char *section = NULL;
  int n, ret = 0;

  assert(rd->ctx != NULL);

  reply = redis_command_fast(rd, "INFO");
  if (!reply || reply->type != REDIS_REPLY_STRING) {
    if (reply->type == REDIS_REPLY_ERROR)
      xerror(0, 0, "can't connect to the server: %s", reply->str);
    else
      xerror(0, 0, "can't connect to the server: type(%d)", reply->type);
    return -1;
  }

  tok = strtok_r(reply->str, REDIS_INFO_DELIMS, &saveptr);
  do {
    if (!tok)
      break;

    if (tok[0] == '#') {        /* something like "# Replication\r\n" */
      if (section)
        free(section);
      n = strspn(tok, "# \t\v\r\n");
      section = tok + n;
      //n = strcspn(tok, "\r\n");
      //*(section + n) = '\0';
      section = strdup(section);
      continue;
    }

    p = strchr(tok, ':');
    if (!p) {                    /* no name:value */
      xdebug(0, "unrecognized info entry: %s", tok);
      continue;
    }
    *p = '\0';
    name = tok;
    value = p + 1;

    // xdebug(0, "info: name[%s] = value[%s]", name, value);

    if ((ret = handler(rd, section, name, value, data)) == 0)
      break;
  } while ((tok = strtok_r(NULL, REDIS_INFO_DELIMS, &saveptr)) != NULL);

  free(section);

  redis_free(reply);
  return ret;
}


#if 0
static int
redis_get_info(REDIS *rd)
{
  redisReply *reply;
  char *tok, *saveptr, *savefield;
  char *p, *name, *value;

  reply = redis_command_fast(rd, "INFO");

  tok = strtok_r(reply->str, REDIS_INFO_DELIMS, &saveptr);
  do {
    if (!tok)
      break;

    if (tok[0] == '#')          /* section name, ignored */
      continue;

    p = strchr(tok, ':');
    if (!p) {                    /* no name:value */
      xdebug(0, "unrecognized info entry: %s", tok);
      continue;
    }
    *p = '\0';
    name = tok;
    value = p + 1;

    xdebug(0, "info: name[%s] = value[%s]", name, value);

    if (strcmp(REDIS_INFO_VERSION, name) == 0) {
      p = strchr(value, '.');
      *p = '\0';
      rd->ver_major = atoi(value);
      value = p + 1;
      p = strchr(value, '.');
      *p = '\0';
      rd->ver_minor = atoi(value);
    }

  } while ((tok = strtok_r(NULL, REDIS_INFO_DELIMS, &saveptr)) != NULL);

  redis_free(reply);
  return 0;
}
#endif  /* 0 */


#if 0
static int
redis_parse_dump_handler(REDIS *rd,
                         char *section,
                         char *field,
                         char *value,
                         void *data)
{
  (void)rd;
  (void)data;

  xdebug(0, "[%s] %s = %s", section, field, value);
  return 1;
}
#endif  /* 0 */


static int
redis_parse_version_handler(REDIS *rd,
                            char *section,
                            char *field,
                            char *value,
                            void *data)
{
  char *p;

  (void)section;
  (void)data;

  if (strcmp(field, REDIS_INFO_VERSION) == 0) {
    p = strchr(value, '.');
    if (p) {
      *p = '\0';
      rd->ver_major = atoi(value);
      value = p + 1;
    }
    else {
      rd->ver_major = atoi(value);
      rd->ver_minor = 0;
      return 0;
    }

    p = strchr(value, '.');
    if (p)
      *p = '\0';
    rd->ver_minor = atoi(value);

    return 0;                  /* force end of parsing */
  }
  return 1;
}


static int
redis_parse_version(REDIS *rd)
{
  int ret;

  rd->ver_major = rd->ver_minor = 0;

  ret = redis_parse_info(rd, redis_parse_version_handler, NULL);
  return ret;
}


int
redis_reopen_unlocked(REDIS *rd)
{
  /* For maintainers:
   *   remember that whenever REDIS->CTX is changed, you need to reset
   *   members in REDIS (e.g. ver_major and ver_minor) according to the
   *   new REDIS->CTX. */
  struct redis_hostent *ent;
  int i;

#if 0
  if (rd->chost <= 0) {
    return -1;
  }
#endif  /* 0 */

  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    rd->chost = (rd->chost + 1) % REDIS_HOSTS_MAX;
    ent = redis_get_host(rd, rd->chost);
    if (!ent)
      continue;

    MARK_FAILED(rd);

    if (rd->ctx) {
      redisFree(rd->ctx);
      rd->ctx = NULL;
    }

    rd->ver_major = rd->ver_minor = 0;

    rd->ctx = redis_context(ent);
    if (!rd->ctx) {
      xdebug(0, "can't connect to the redis server");
      continue;
    }
    else {
      if (rd->password) {
        redisReply *reply = redis_command_fast(rd, "AUTH %s", rd->password);

        if (redis_iserror(reply) || reply->type != REDIS_REPLY_STATUS) {
          if (reply->type == REDIS_REPLY_ERROR)
            xerror(0, 0, "authentication failed: %s", reply->str);
          else
            xerror(0, 0, "authentication failed: type(%d)", reply->type);
          redis_free(reply);
          continue;
        }
        else
          redis_free(reply);
      }

      if (redis_parse_version(rd) == -1) {
        redisFree(rd->ctx);
        rd->ctx = NULL;
        xdebug(0, "can't parse the redis version");
        continue;
      }
      MARK_SUCCESS(rd);
    }

    if (redis_find_master(rd, &ent) == -1) {
      xerror(0, 0, "can't find the master! need to patch sredis.c");
      continue;
    }

    if (ent != NULL) {            /* ENT could be the new master */
      rd->ver_major = rd->ver_minor = 0;
      redisFree(rd->ctx);
      rd->ctx = redis_context(ent);
      /* Note that if redis is mis-configured for the master, so that
       * if we can't connect to it, above call may fail */

      if (!rd->ctx) {
        xerror(0, 0, "can't connect to the master (%s:%d)",
               ent->host, ent->port);
      }
      else {
        if (redis_parse_version(rd) == -1) {
          redisFree(rd->ctx);
          rd->ctx = NULL;
          xdebug(0, "can't parse the redis version");
        }
        else {
          break;                /* we found the master! */
        }
      }
    }
    else
      break;                    /* this is the master! */
  }

  if (!rd->ctx) {
    xdebug(0, "tried all registered redis endpoints, none works.");
    return -1;
  }
  return 0;
}


int
redis_reopen(REDIS *rd)
{
  int ret;

  redis_lock(rd);
  ret = redis_reopen_unlocked(rd);
  redis_unlock(rd);

  return ret;
}


#if 0
int
redis_reopen(REDIS *rd)
{
  struct redis_hostent *ent;

  assert(rd->stacked == 0);

  if (rd->ctx)
    redisFree(rd->ctx);

  ent = redis_next_host(rd);
  if (ent == NULL)
    return -1;

  if (ent->c_timeout.tv_sec == 0 && ent->c_timeout.tv_usec == 0)
    rd->ctx = redisConnect(ent->host, ent->port);
  else
    rd->ctx = redisConnectWithTimeout(ent->host, ent->port, ent->c_timeout);

  if (rd->ctx == NULL || rd->ctx->err) {
    if (rd->ctx) {
      xerror(0, 0, "connection error: [%s:%d] %s",
             ent->host, ent->port, rd->ctx->errstr);
      redisFree(rd->ctx);
      rd->ctx = 0;
    }
    else
      xerror(0, 0, "connection error: can't allocate redis context");
    return -1;
  }
  else {
    if (ent->o_timeout.tv_sec != 0 || ent->o_timeout.tv_usec != 0)
      redisSetTimeout(rd->ctx, ent->o_timeout);

  }
  return 0;
}
#endif  /* 0 */


void
redis_close(REDIS *rd)
{
  if (rd == 0)
    return;

  redis_lock(rd);
  redis_shutdown(rd);
  redis_unlock(rd);

#ifdef _PTHREAD
  pthread_mutex_destroy(&rd->mutex);
#endif

  free(rd->password);
  free(rd);
}


int
redis_host_add(REDIS *redis, const char *host, int port,
               const struct timeval *c_timeout,
               const struct timeval *o_timeout)
{
  int i;
  struct redis_hostent *p;

  /* For maintainers:  please update the comments in struct REDIS_ in
   * <sredis.h> whenever you update this function. */
  redis_lock(redis);

  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    if (redis->hosts[i] == NULL) {
      p = malloc(sizeof(*p));
      if (!p) {
        redis_unlock(redis);
        return -1;
      }

      p->success = p->failure = 0;

      p->host = strdup(host);
      if (!p->host) {
        free(p);
        redis_unlock(redis);
        return -1;
      }
      p->port = port;

      if (c_timeout)
        memcpy(&p->c_timeout, c_timeout, sizeof(*c_timeout));
      else {
        p->c_timeout.tv_sec = 0;
        p->c_timeout.tv_usec = 0;
      }

      if (o_timeout)
        memcpy(&p->o_timeout, o_timeout, sizeof(*o_timeout));
      else {
        p->o_timeout.tv_sec = 0;
        p->o_timeout.tv_usec = 0;
      }
      redis->hosts[i] = p;
      redis_unlock(redis);
      return i;
    }
  }

#if 0
  ifail = -1;
  nfail = 0;
  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    if (redis->hosts[i] != NULL) {
      if (redis->hosts[i]->failure > nfail) {
        ifail = i;
        nfail = redis->hosts[i]->failure;
      }
    }
  }

  if (ifail >= 0) {
  }
#endif  /* 0 */

  errno = ENOSPC;
  redis_unlock(redis);
  return -1;
}


int
redis_shutdown(REDIS *redis)
{
  int i;

  assert(redis->stacked == 0);

  if (redis->ctx) {
    redisFree(redis->ctx);
    redis->ctx = NULL;
  }

  for (i = 0; i < REDIS_HOSTS_MAX; i++)
    redis_host_del(redis, i);

  return 0;
}


int
redis_host_del(REDIS *redis, int index)
{
  assert(index >= 0);
  assert(index < REDIS_HOSTS_MAX);

  redis_lock(redis);
  if (redis->hosts[index]) {
    if (redis->hosts[index]->host)
      free((void *)redis->hosts[index]->host);
    free(redis->hosts[index]);
    redis->hosts[index] = NULL;
    redis_unlock(redis);
    return 0;
  }
  redis_unlock(redis);
  return -1;
}


REDIS *
redis_new(void)
{
  REDIS *p;
  int i;

  p = malloc(sizeof(*p));
  if (!p)
    return NULL;

  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    p->hosts[i] = NULL;
  }
  p->chost = -1;
  p->ctx = 0;
  p->stacked = 0;

  p->ver_major = 0;
  p->ver_minor = 0;

  p->password = NULL;

#ifdef _PTHREAD
  {
    int err;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (err) {
      xdebug(err, "pthread_mutexattr_settype() failed");
      pthread_mutexattr_destroy(&attr);
      free(p);
      return NULL;
    }

    err = pthread_mutex_init(&p->mutex, &attr);
    if (err) {
      xdebug(err, "pthread_mutex_init() failed");
      pthread_mutexattr_destroy(&attr);
      free(p);
      return NULL;
    }

    pthread_mutexattr_destroy(&attr);
  }
#endif  /* _PTHREAD */

  return p;
}


void
redis_set_password(REDIS *redis, const char *password)
{
  if (redis->password)
    free(redis->password);
  redis->password = strdup(password);
}


REDIS *
redis_open(const char *host, int port, const struct timeval *c_timeout,
           const struct timeval *o_timeout)
{
  REDIS *p = redis_new();

  if (!p)
    return NULL;

  if (redis_host_add(p, host, port, c_timeout, o_timeout) != 0) {
    redis_close(p);
    return NULL;
  }

#if 0
  /* If the redis server is not available yet, redis_reopen() will fail,
   * but we can continue if later the server is available, since redis_reopen()
   * will be called if redis->ctx is NULL. */
  if (redis_reopen_unlocked(p) != 0) {
    redis_close(p);
    return NULL;
  }
#endif  /* 0 */

  return p;
}


static redisReply *
redis_vcommand_unlocked(REDIS *redis, int reopen,
                        const char *format, va_list ap)
{
  redisReply *reply = NULL;

  assert(redis->stacked == 0);

#if 0
  if (redis->chost < 0) {
    xdebug(0, "redis was not configured, no server endpoint");
    return NULL;
  }
#endif  /* 0 */

  if (!redis->ctx) {
    if (reopen && redis_reopen_unlocked(redis) != 0) {
      xdebug(0, "redis re-connection failed");
      return NULL;
    }
    else
      xdebug(0, "redis re-connected");
  }

  if (redis->ctx) {
    /* We need double check for redis->ctx since
     * wrong master configuration may causes redis_reopen() failed.*/
    reply = redisvCommand(redis->ctx, format, ap);
  }

  if (!reply) {
    xdebug(0, "redis null reply");
    if (reopen && redis_reopen_unlocked(redis) != 0)
      xdebug(0, "redis re-connection failed");
    else
      xdebug(0, "redis re-connected");
  }
  else if (reply->type == REDIS_REPLY_ERROR) {
    xdebug(0, "redis error: %s", reply->str);
    if (reply->str != 0 && strncasecmp(ERR_READONLY,
                                       reply->str,
                                       sizeof(ERR_READONLY) - 1) == 0) {
      /* Strange, currently connected to the master, but it is
       * actually a slave.   It seems that hiredis didn't give
       * a detailed error but a error string. */
      xdebug(0, "volunterily disconnect from the possible slave");
      if (redis_reopen_unlocked(redis) != 0)
        xdebug(0, "redis re-connection failed");
      else
        xdebug(0, "redis re-connected");
    }
  }

  return reply;
}


static redisReply *
redis_vcommand(REDIS *redis, int reopen,
               const char *format, va_list ap)
{
  redisReply *reply;
  redis_lock(redis);
  reply = redis_vcommand_unlocked(redis, reopen, format, ap);
  redis_unlock(redis);
  return reply;
}


redisReply *
redis_command_fast_unlocked(REDIS *redis, const char *format, ...)
{
  va_list ap;
  redisReply *reply = NULL;

  va_start(ap, format);
  reply = redis_vcommand_unlocked(redis, FALSE, format, ap);
  va_end(ap);

  return reply;
}


redisReply *
redis_command_fast(REDIS *redis, const char *format, ...)
{
  va_list ap;
  redisReply *reply = NULL;

  va_start(ap, format);
  reply = redis_vcommand(redis, FALSE, format, ap);
  va_end(ap);

  return reply;
}


redisReply *
redis_command_unlocked(REDIS *redis, const char *format, ...)
{
  va_list ap;
  redisReply *reply = NULL;

  va_start(ap, format);
  reply = redis_vcommand_unlocked(redis, TRUE, format, ap);
  va_end(ap);

  return reply;
}


redisReply *
redis_command(REDIS *redis, const char *format, ...)
{
  va_list ap;
  redisReply *reply = NULL;

  va_start(ap, format);
  reply = redis_vcommand(redis, TRUE, format, ap);
  va_end(ap);

  return reply;
}


static int
redis_vappend(REDIS *redis, const char *format, va_list ap)
{
  int ret;

  if (!redis->ctx) {
    if (redis->stacked != 0) {
      xdebug(0, "redis connection failed during building PIPELINE or MULTI");
      redis->stacked = 0;
    }
    /* we cannot call redis_reopen iff (redis->stacked != 0) */
    if (redis_reopen_unlocked(redis) != 0) {
      xdebug(0, "redis re-connection failed");
      return REDIS_ERR;
    }
    else
      xdebug(0, "redis re-connected");
  }

  ret = redisvAppendCommand(redis->ctx, format, ap);

  if (ret == REDIS_OK)
    redis->stacked++;

  return ret;
}


int
redis_append_unlocked(REDIS *redis, const char *format, ...)
{
  va_list ap;
  int ret;

  va_start(ap, format);
  ret = redis_vappend(redis, format, ap);
  va_end(ap);

  return ret;
}


int
redis_append(REDIS *redis, const char *format, ...)
{
  va_list ap;
  int ret;

  va_start(ap, format);
  redis_lock(redis);
  ret = redis_vappend(redis, format, ap);
  redis_unlock(redis);
  va_end(ap);

  return ret;
}


/* Create a reply object: copied from hiredis.c */
static redisReply *
createReplyObject(int type)
{
  redisReply *r = calloc(1, sizeof(*r));

  if (r == NULL)
    return NULL;

  r->type = type;
  return r;
}


redisReply *
redis_exec_unlocked(REDIS *redis)
{
  redisReply *reply, *packed;
  size_t i;

  assert(redis != NULL);

  if (redis->stacked == 0)
    return NULL;

  packed = createReplyObject(REDIS_REPLY_ARRAY);
  if (!packed)
    return NULL;

  packed->element = malloc(redis->stacked * sizeof(redisReply *));
  if (packed->element == NULL) {
    xerror(0, errno, "can't allocate memory for redisReply *");
    freeReplyObject(packed);
    return NULL;
  }

  for (i = 0; i < redis->stacked; i++)
    packed->element[i] = NULL;

  packed->elements = redis->stacked;

  for (i = 0; i < redis->stacked; i++) {
    if (redisGetReply(redis->ctx, (void **)&reply) == REDIS_ERR) {
      goto err;
    }
    if (reply == NULL) {
      xerror(0, 0, "unrecognized reply from redis!");
      abort();
    }
    else if (reply->type == REDIS_REPLY_ERROR) {
      xdebug(0, "redis error: %s", reply->str);
    }
    packed->element[i] = reply;
  }
  redis->stacked = 0;
  return packed;

 err:
  for (i = 0; i < redis->stacked; i++) {
    if (packed->element[i] != NULL) {
      freeReplyObject(packed->element[i]);
      packed->element[i] = NULL;
    }
  }
  packed->elements = 0;
  freeReplyObject(packed);

  redis->stacked = 0;
  redis_reopen_unlocked(redis);

  return NULL;
}


redisReply *
redis_exec(REDIS *redis)
{
  redisReply *reply;
  redis_lock(redis);
  reply = redis_exec_unlocked(redis);
  redis_unlock(redis);
  return reply;
}


void
redis_free(redisReply *reply)
{
  if (reply)
    freeReplyObject(reply);
}


static struct {
  int type;
  char *name;
} reply_types[] = {
#define P(x)    { x, #x }
  P(REDIS_REPLY_STRING),
  P(REDIS_REPLY_ARRAY),
  P(REDIS_REPLY_INTEGER),
  P(REDIS_REPLY_NIL),
  P(REDIS_REPLY_STATUS),
  P(REDIS_REPLY_ERROR),
  { 0, 0 },
#undef P
};


static const char *
reply_type_string(int type)
{
  int i;
  for (i = 0; reply_types[i].type != 0; i++)
    if (reply_types[i].type == type)
      return reply_types[i].name;
  return "*UNKNOWN*";
}


void
redis_dump_reply(const redisReply *reply,
                 const char *prefix, int indent)
{
  size_t i;
  int idnt = indent * 2;
  if (!prefix)
    prefix = "";

  if (!reply)
    xerror(0, 0, "%*s%s reply: null", idnt, " ", prefix);
  else {
    xerror(0, 0, "%*s%s reply: type(%d:%s)", idnt, " ", prefix,
           reply->type, reply_type_string(reply->type));

    switch (reply->type) {
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_ERROR:
      xerror(0, 0, "%*s%s reply: %s", idnt, " ", prefix, reply->str);
      break;
    case REDIS_REPLY_INTEGER:
      xerror(0, 0, "%*s%s reply: %lld", idnt, " ", prefix, reply->integer);
      break;
    case REDIS_REPLY_NIL:
      xerror(0, 0, "%*s%s reply: nil", idnt, " ", prefix);
      break;
    case REDIS_REPLY_ARRAY:
      xerror(0, 0, "%*s%s reply: array size(%zd)", idnt, " ", prefix,
             reply->elements);
      for (i = 0; i < reply->elements; i++) {
        redis_dump_reply(reply->element[i], prefix, indent + 1);
      }
    default:
      break;
    }
  }
}


long long
redis_reply_integer(redisReply *reply)
{
  char *endptr;
  long long ret = 0;

  switch (reply->type) {
  case REDIS_REPLY_INTEGER:
    ret = reply->integer;
    break;
  case REDIS_REPLY_STRING:
    if (reply->str[0] == '\0')
      break;                 /* empty string("") is treated as zero. */
    ret = strtoll(reply->str, &endptr, 10);
    if (*endptr != '\0') {
      /* When the REPLY is from lua scripts, the REPLY may contain
       * string, which uses scientific notation such as
       * "3.2414214213422e+16". */
      ret = (long long)strtold(reply->str, &endptr);
      if (*endptr != '\0') {
        xdebug(0, "warning: invalid value (%s) detected when integer expected",
               reply->str);
        errno = EINVAL;
        ret = 0;
      }
    }
    break;
  default:
    xdebug(0, "warning: wrong type(%d) detected when integer expected",
           reply->type);
    errno = EINVAL;
    break;
  }
  return ret;
}
