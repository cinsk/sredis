#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <errno.h>

#include "sredis.h"
#include "xerror.h"


#ifndef FALSE
#define FALSE   0
#define TRUE    (!FALSE)
#endif

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

static struct redis_hostent *redis_next_host(REDIS *rd);
static redisReply *redis_vcommand(REDIS *redis, int reopen,
                                  const char *format, va_list ap);
static redisContext *redis_context(struct redis_hostent *endpoint);
static struct redis_hostent *redis_get_hostent_create(REDIS *redis,
                                                      const char *host,
                                                      int port);
static int redis_parse_info(REDIS *rd, redis_info_handler handler, void *data);

//static int redis_get_info(REDIS *rd);
static struct redis_hostent *redis_find_master(REDIS *redis);
static struct redis_hostent *redis_find_master_24(REDIS *redis);
static struct redis_hostent *redis_find_master_26(REDIS *redis);


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


static struct redis_hostent *
redis_find_master(REDIS *redis)
{
  assert(REDIS_IS_HIGHER(redis, 2, 4) >= 0);

  if (REDIS_IS_HIGHER(redis, 2, 6) >= 0)
    return redis_find_master_26(redis);
  else
    return redis_find_master_24(redis);
}


static struct redis_hostent *
redis_find_master_24(REDIS *redis)
{
  struct repldata data = { 0, NULL, 0 };
  struct redis_hostent *ret = NULL;
  int s;

  s = redis_parse_info(redis, redis_parse_master_handler, &data);
  if (s < 0)
    return NULL;

  ret = redis_get_hostent_create(redis, data.host, data.port);
  free(data.host);
  return ret;
}


static struct redis_hostent *
redis_find_master_26(REDIS *redis)
{
  char *endpoint;
  char *tok, *saveptr, *addr;
  int port;
  struct redis_hostent *ret = NULL;
  redisReply *reply = redis_command_fast(redis, "CONFIG GET slaveof");

  if (!reply) {                 /* can't connect to the current server */
    xdebug(0, "can't connect to the redis server");
    goto fin;
  }

  if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
    xdebug(0, "unexpected redis response type(%d), elms(%d)",
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
  }

 fin:
  redis_free(reply);
  return ret;
}


static struct redis_hostent *
redis_get_hostent_create(REDIS *redis, const char *host, int port)
{
  int i;
  struct redis_hostent *p;

  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    p = redis->hosts[i];
    if (p == NULL)
      continue;

    if (strcmp(host, p->host) == 0 && port == p->port)
      return p;
  }

  p = redis->hosts[redis->chost];
  assert(p != NULL);

  i = redis_host_add(redis, host, port, &p->c_timeout, &p->o_timeout);
  if (i < 0)
    return NULL;
  redis->chost = i;

  return redis->hosts[i];
}


static struct redis_hostent *
redis_next_host(REDIS *rd)
{
  int i, j;
  int pos;

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
    xerror(0, 0, "can't connect to the server");
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
    *p = '\0';
    rd->ver_major = atoi(value);
    value = p + 1;
    p = strchr(value, '.');
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
redis_reopen(REDIS *rd)
{
  /* For maintainers:
   *   remember that whenever REDIS->CTX is changed, you need to reset
   *   members in REDIS (e.g. ver_major and ver_minor) according to the
   *   new REDIS->CTX. */
  struct redis_hostent *ent;

  MARK_FAILED(rd);

  if (rd->ctx) {
    redisFree(rd->ctx);
    rd->ctx = NULL;
  }

  ent = redis_next_host(rd);

  rd->ver_major = rd->ver_minor = 0;

  rd->ctx = redis_context(ent);
  if (!rd->ctx)
    return -1;
  else {
    if (redis_parse_version(rd) == -1) {
      redisFree(rd->ctx);
      rd->ctx = NULL;
      return -1;
    }

    MARK_SUCCESS(rd);
  }

  ent = redis_find_master(rd);

  if (ent != NULL) {            /* ENT is the new master */
    rd->ver_major = rd->ver_minor = 0;
    redisFree(rd->ctx);
    rd->ctx = redis_context(ent);
    /* Note that if redis is mis-configured for the master, above call
     * may fail */

    if (!rd->ctx) {
      xerror(0, 0, "can't connect to the master (%s:%d)", ent->host, ent->port);
      return -1;
    }
    else {
      redis_parse_version(rd);
    }

  }
  return 0;
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

  redis_shutdown(rd);

  free(rd);
}


int
redis_host_add(REDIS *redis, const char *host, int port,
               const struct timeval *c_timeout,
               const struct timeval *o_timeout)
{
  int i;
  struct redis_hostent *p;

  for (i = 0; i < REDIS_HOSTS_MAX; i++) {
    if (redis->hosts[i] == NULL) {
      p = malloc(sizeof(*p));

      p->success = p->failure = 0;

      p->host = strdup(host);
      if (!p->host) {
        free(p);
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
  return -1;
}


int
redis_shutdown(REDIS *redis)
{
  int i;

  assert(redis->stacked == 0);

  if (redis->ctx)
    redisFree(redis->ctx);

  for (i = 0; i < REDIS_HOSTS_MAX; i++)
    redis_host_del(redis, i);

  return 0;
}


int
redis_host_del(REDIS *redis, int index)
{
  assert(index >= 0);
  assert(index < REDIS_HOSTS_MAX);

  if (redis->hosts[index]) {
    if (redis->hosts[index]->host)
      free((void *)redis->hosts[index]->host);
    free(redis->hosts[index]);
    redis->hosts[index] = NULL;
    return 0;
  }
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

  return p;
}


REDIS *
redis_open(const char *host, int port, const struct timeval *c_timeout,
           const struct timeval *o_timeout)
{
  REDIS *p = redis_new();

  if (redis_host_add(p, host, port, c_timeout, o_timeout) != 0) {
    redis_close(p);
    return NULL;
  }

#if 0
  /* If the redis server is not available yet, redis_reopen() will fail,
   * but we can continue if later the server is available, since redis_reopen()
   * will be called if redis->ctx is NULL. */
  if (redis_reopen(p) != 0) {
    redis_close(p);
    return NULL;
  }
#endif  /* 0 */

  return p;
}


static redisReply *
redis_vcommand(REDIS *redis, int reopen, const char *format, va_list ap)
{
  redisReply *reply = NULL;

  assert(redis->stacked == 0);

  if (!redis->ctx) {
    if (reopen && redis_reopen(redis) != 0) {
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
    if (reopen && redis_reopen(redis) != 0)
      xdebug(0, "redis re-connection failed");
    else
      xdebug(0, "redis re-connected");
  }
  else if (reply->type == REDIS_REPLY_ERROR) {
    xdebug(0, "redis error: %s", reply->str);
  }

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
redis_command(REDIS *redis, const char *format, ...)
{
  va_list ap;
  redisReply *reply = NULL;

  va_start(ap, format);
  reply = redis_vcommand(redis, TRUE, format, ap);
  va_end(ap);

  return reply;
}


int
redis_append(REDIS *redis, const char *format, ...)
{
  va_list ap;
  int ret;

  if (!redis->ctx) {
    if (redis->stacked != 0) {
      xdebug(0, "redis connection failed during building PIPELINE or MULTI");
      redis->stacked = 0;
    }
    /* we cannot call redis_reopen iff (redis->stacked != 0) */
    if (redis_reopen(redis) != 0) {
      xdebug(0, "redis re-connection failed");
      return REDIS_ERR;
    }
    else
      xdebug(0, "redis re-connected");
  }

  va_start(ap, format);
  ret = redisvAppendCommand(redis->ctx, format, ap);
  va_end(ap);

  if (ret == REDIS_OK)
    redis->stacked++;

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
redis_exec(REDIS *redis)
{
  redisReply *reply, *packed;
  size_t i;

  assert(redis != NULL);

  if (redis->stacked == 0)
    return NULL;

  packed = createReplyObject(REDIS_REPLY_ARRAY);

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
  redis_reopen(redis);

  return NULL;
}


void
redis_free(redisReply *reply)
{
  if (reply)
    freeReplyObject(reply);
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
    xerror(0, 0, "%*s%s reply: type(%d)", idnt, " ", prefix, reply->type);

    switch (reply->type) {
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_ERROR:
      xerror(0, 0, "%*s%s reply: NULL", idnt, " ", prefix);
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