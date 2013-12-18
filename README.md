

sredis
======

Simple Redis Client (wrapper to hiredis) in C


Introduction
------------

Sredis is convenient wrapper to [hiredis](http://redis.io/), the official redis client.  What Sredis provides are:

- Disconnection recovery
- Connect to the master node automatically
- Easier redis transaction / pipeline interface

What Sredis lacks is:

- Asynchronous API


Compilation
-----------

Note that this project contains [hiredis](http://redis.io/) as a GIT submodule.  Installing Sredis will also install hiredis.

    $ git clone https://github.com/cinsk/sredis.git
    $ cd sredis
    $ git submodule update --init
    $ ./autogen.sh
    $ ./configure
    & make
    
Note that the sample program, `sredis-example` won't work unless it is installed.  If you want to test before installation, try to provide local installation directory such as:

    $ make PREFIX=`pwd`/root install
    

Usage
-----

###Initialization

You'll need to create `REDIS` structure via one of following functions:

    REDIS *redis_new(void);
    REDIS *redis_open(const char *host, int port,
                      const struct timeval *c_timeout,
                      const struct timeval *o_timeout);


If you create `REDIS` structure via `redis_new()`, then you'll need to
add redis server endpoint using `redis_host_add()`:

    REDIS *redis;
    redis = redis_new();
    redis_host_add(redis, "127.0.0.1", 6379, NULL, NULL);

###Simple Redis Operation

You can execute any redis commands using `redis_command()`.  The
returned value, `redisReply` is the same as _hiredis_.  The returned
value should be freed by calling `redis_free()`.  Unlike
`freeReplyObject()` in _hiredis_, `redis_free()` can accept NULL.
Thus, any value returned by `redis_comman()` can safely be passed to
`redis_free()`.

    redisReply *reply;
    reply = redis_command(redis, "GET foo");
    if (redis_iserror(reply)) {
      /* handle an error */
    }
    redis_free(reply);

As in _hiredis_, redis_command accept `printf(3)` like arguments:

    reply = redis_command(redis, "SET foo %s", foo_value);
    redis_free(reply);

###Pipeline

Just use `redis_append()` instead of `redis_command()`.  All pipelined
redis commands are executed when `redis_flush()` is called:

    redisReply *reply;
    redis_append(redis, "SET bar %s", bar_value);
    redis_append(redis, "GET foo");
    ...
    reply = redis_flush(redis);
    if (redis_iserror(reply)) {
      /* handle error */
    }
    else {
      if (reply->type == REDIS_REPLY_ARRAY) {
        int i;
        for (i = 0; i < reply->elements; i++) {
          redisReply *r = reply->element[i];
          /* 'r' points each pipelined reply */
        }
      }
      redis_free(reply);
    }
    
Unlike _hiredis_, `redis_flush()` accumulates all replies in the
pipelined commands, and return it as an array.  In other words, the
return value from `redis_flush()` has `REDIS_REPLY_ARRAY` type on
success.

###Transaction

A redis transaction (`MULTI` ... `EXEC`) starts with `redis_multi()`
and ends with `redis_exec()`.  Commands in the transaction is canned with
`redis_append()`, and executed when `redis_flush()' is called:

    redisReply *reply;
    
    redis_multi(redis);
    redis_append(redis, "GET foo");
    redis_append(redis, "GET bar");
    redis_append(redis, "BRPOPLPUSH %s %s %d", source, destination, timeout);
    redis_exec(redis);
    
    reply = redis_flush(redis);
    
Note that the returned value from `redis_flush()` contains all the replies
from the queued commands, including `redis_multi()`, `redis_append()`, and
`redis_exec()`.  For example, the above `reply` will contains something like:

    reply->element[0] = /* the reply of MULTI */;
    reply->element[1] = /* the reply of GET foo */;
    reply->element[2] = /* the reply of GET bar */;
    reply->element[3] = /* the reply of BRPOPLPUSH */;
    reply->element[4] = /* the reply of EXEC, which is an REDIS_REPLY_ARRAY */;

Mostly, you'll have an interest only on the reply of `EXEC`.  To make
it easy, _sredis_ provides `redis_multi_reply()` to get the reply of
`EXEC` command:

    ...
    reply = redis_flush(redis);
    {
        redisReply *transaction_result;
        transaction_result = redis_multi_reply(redis, reply, 0);
        /* transaction_result holds the result of EXEC */
    }
    redis_free(reply);

The third parameter of `redis_multi_reply()' is the index of the
transaction.  In the above example, only one transaction is queued, so
the index should be zero.

Note that you'll need to call `redis_free()` on the returned value of
`redis_flush()`, not the value returned from `redis_multi_reply()`.

###High Availabilty

_sredis_ is designed to work with a redis replication with following properties:

- All severs have fixed endpoints (IP address/port).
- All redis commands will go to the master server.
- When a master redis failed, one of slaves will escalate to new master.
- New slave will join with the previous master's endpoint.
- All slaves are read-only servers.

Note that _sredis_ itself has nothing to do with the replication itself.
You'll need to setup the replication yourself. (via redis-sentinel or
some custom scripts)

When there is a sudden disconnection between the application and redis
replication (due to network hazard or master failure), either of
`redis_command()` or `redis_flush()` will return NULL.

If you added more than one endpoint via `redis_host_add()`, then
subsequent call to `redis_command()` or `redis_flush()` will
automatically connect to the next available master.


