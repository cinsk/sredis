#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <pthread.h>
#include <getopt.h>

#include "sredis.h"

#define REDIS_HOST      "127.0.0.1"
#define REDIS_PORT      6379

char *redis_host;
int redis_port = REDIS_PORT;

struct timeval redis_ctimeout = { 5, 0 };
struct timeval redis_otimeout = { 30, 0 };

int redis_repeat_count = 100;

struct option long_opts[] = {
  { "host", required_argument, 0, 'h' },
  { "port", required_argument, 0, 'p' },
  { "ctimeout", required_argument, 0, 'C' },
  { "otimeout", required_argument, 0, 'O' },
  { "repeat", required_argument, 0, 'r' },
  { NULL, 0, NULL, 0, },
};


REDIS *redis;

static long diff_timespec(const struct timespec *now,
                          const struct timespec *old);

static void *thread_main(void *arg);


pthread_t *child_threads;
size_t nchild_threads;
size_t nchild_ready;

pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
int child_go = 0;

int
main(int argc, char *argv[])
{
  int i;

  redis_host = strdup(REDIS_HOST);

  while (1) {
    int opt = getopt_long(argc, argv, "h:p:C:O:", long_opts, NULL);
    if (opt == -1)
      break;
    switch (opt) {
    case 'h':
      free(redis_host);
      redis_host = strdup(optarg);
      break;
    case 'p':
      redis_port = atoi(optarg);
      break;
    case 'C':
      redis_ctimeout.tv_sec = atoi(optarg);
      break;
    case 'O':
      redis_otimeout.tv_sec = atoi(optarg);
      break;
    case 'r':
      redis_repeat_count = atoi(optarg);
      break;
    }
  }

  argc -= optind;
  argv -= optind;

  for (i = 0; i < nchild_threads; i++) {
    pthread_create(&child_threads[i], thread_main, argv[0]);
  }

  while (1) {
    int ready = __sync_fetch_and_add(&nchild_ready, 0);
    if (ready >= nchild_threads)
      break;

    fprintf(stderr, "%d thread(s) ready\n", ready);
    usleep(100);
  }

  pthread_mutex_lock(&cond_mutex);
  pthread_cond_broadcast(&cond);

  {
    int i;
    long retval;

    for (i = 0; i < nchild_threads; i++) {
      pthread_join(child_threads[i], (void **)&retval);
    }
  }

  //redis = redis_open(redis_host, redis_port, &redis_ctimeout, &redis_otimeout);




  redis_close(redis);
  return 0;
}


static long
diff_timespec(const struct timespec *now, const struct timespec *old)
{
  long d;

  d = (now->tv_nsec - old->tv_nsec) / 1000000;
  d += (now->tv_sec - old->tv_sec) * 1000;

  return d;
}


static void *
thread_main(void *arg)
{
  struct timespec tm_begin, tm_end;
  redisReply *reply;
  int i;

  pthread_mutex_lock(&cond_mutex);
  pthread_cond_wait(&cond, &cond_mutex);

  clock_gettime(CLOCK_MONOTONIC, &tm_begin);

  for (i = 0; i < redis_repeat_count; i++) {
    reply = redis_command(redis, (const char *)arg);
    redis_free(reply);
  }

  clock_gettime(CLOCK_MONOTONIC, &tm_end);

  return (void *)diff_timespec(&tm_end, &tm_begin);
}
