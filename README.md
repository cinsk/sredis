

sredis
======

Simple Redis Client (wrapper to hiredis) in C


Introduction
------------

Sredis is convenient wrapper to [hiredis](http://redis.io/), the official redis client.  What Sredis provides are:

- Disconnection recovery
- Connect to the master node automatically
- Easier redis transaction / pipeline interface

What Sredis lacks are:

- Asynchronous API


Compilation
-----------

Note that this project contains [hiredis](http://redis.io/) as a GIT submodule.  Installing Sredis will also install hiredis.

    $ git clone https://github.com/cinsk/sredis.git
    $ cd sredis
    $ git submodule update --init

Note that the sample program, `sredis-example` won't work unless it is installed.  If you want to test before installation, try to provide local installation directory such as:

    $ make PREFIX=`pwd`/root install
    
