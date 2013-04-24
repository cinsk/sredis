#include "sredis.h"
#include "xerror.h"

int debug_mode = 1;

int
main(void)
{
  REDIS *redis;
  struct timeval ctv = { 1, 50000 };
  struct timeval otv = { 5, 00000 };
  redisReply *reply;
  int i;

  redis = redis_new();
  if (!redis)
    xerror(1, 0, "cannot connect to redis");

  redis_host_add(redis, "127.0.0.1", 6379, &ctv, &otv);
  redis_host_add(redis, "127.0.0.1", 6380, &ctv, &otv);
  redis_host_add(redis, "127.0.0.1", 6381, &ctv, &otv);

  for (i = 0; i < 5; i++) {
    reply = redis_command(redis, "PING");
    if (redis_iserror(reply)) {
      redis_dump_reply(reply, "error", 1);
    }
    else
      redis_dump_reply(reply, "okay", 1);
    redis_free(reply);

    redis_append(redis, "PING");
    redis_append(redis, "SET foo %s", "bar");
    redis_append(redis, "EXPIRE foo %d", 10);
    reply = redis_exec(redis);
    if (redis_iserror(reply)) {
      redis_dump_reply(reply, "error", 1);
    }
    else
      redis_dump_reply(reply, "okay", 1);
    redis_free(reply);
  }

  redis_close(redis);

  return 0;
}
