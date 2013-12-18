#include <pthread.h>
#include <unistd.h>

#include "sredis.h"
#include "xerror.h"

int debug_mode = 1;

pthread_mutex_t child_done_mutex = PTHREAD_MUTEX_INITIALIZER;
int child_done = 0;
pthread_t busy_thread;

static void *busy_thread_main(void *arg);

int
main(void)
{
  REDIS *redis, *redis2;
  redisReply *reply;
  int ret;

  struct timeval ctv = { 1, 50000 };
  struct timeval otv = { 5, 00000 };
  int i;

  redis = redis_new();
  if (!redis)
    xerror(1, 0, "cannot connect to redis");
  redis2 = redis_new();
  if (!redis2)
    xerror(1, 0, "cannot connect to redis");

  redis_host_add(redis, "127.0.0.1", 6379, &ctv, &otv);
  redis_host_add(redis2, "127.0.0.1", 6379, &ctv, &otv);

  {
    reply = redis_command(redis, "SET foo 0");
    redis_free(reply);
  }

  if (1) {
    ret = pthread_create(&busy_thread, NULL, busy_thread_main, (void *)redis2);
    if (ret)
      xerror(1, ret, "pthread_create() failed");
  }

  for (i = 0; i < 10; i++) {
    int data = -10000;

    reply = redis_command(redis, "WATCH foo");
    redis_free(reply);

    usleep(rand() % (1000000 / 5));
    reply = redis_command(redis, "GET foo");
    if (redis_iserror(reply))
      xerror(0, 0, "can't get foo: %s", reply ? reply->str : "");
    else {
      data = redis_reply_integer(reply);
      xerror(0, 0, "DATA before transaction: %d", data);
    }

    redis_free(reply);

    redis_multi(redis);
    redis_append(redis, "GET foo");
    redis_append(redis, "SET foo %d", data);
    redis_append(redis, "GET foo");
    redis_multi_exec(redis);
    reply = redis_exec(redis);

    if (reply) {
      redisReply *treply = redis_multi_reply(redis, reply, 0);
      redis_dump_reply(treply, "DUMP: ", 2);
    }
    redis_free(reply);
    fputs("\f\n", stderr);
  }

  pthread_mutex_lock(&child_done_mutex);
  child_done = 1;
  pthread_mutex_unlock(&child_done_mutex);

  ret = pthread_join(busy_thread, NULL);
  if (ret)
    xerror(1, ret, "pthread_join() failed");

  redis_close(redis);
  redis_close(redis2);

  return 0;
}


static void *
busy_thread_main(void *arg)
{
  REDIS *redis = (REDIS *)arg;
  redisReply *reply;
  int prio, ret;

  prio = sched_get_priority_max(SCHED_RR);
  if (prio == -1)
    xerror(0, errno, "sched_get_priority_max() failed");
  else {
    struct sched_param param;
    param.sched_priority = prio;
    xerror(0, 0, "max priority: %d", prio);
    ret = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    if (ret != 0)
      xerror(0, ret, "pthread_setschedparam() failed");
    else
      xerror(0, 0, "pthread_setschedparam(): priority=%d", prio);
  }

  while (1) {
    pthread_mutex_lock(&child_done_mutex);
    if (child_done) {
      pthread_mutex_unlock(&child_done_mutex);
      break;
    }
    else
      pthread_mutex_unlock(&child_done_mutex);

    usleep(rand() % (1000000 / 2));
    reply = redis_command(redis, "INCR foo");
    if (redis_iserror(reply)) {
      if (reply)
        error(0, 0, "INCR foo failed: %s", reply->str);
      else
        error(0, 0, "INCR foo failed");
    }
    redis_free(reply);
  }

  return NULL;
}
