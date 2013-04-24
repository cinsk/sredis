

sredis
======

Simple Redis Client (helpers to hiredis)

Compilation
-----------

    $ git clone https://github.com/cinsk/sredis.git
    $ cd sredis
    $ git submodule update --init

Note that the sample program, `sredis-example` won't work unless it is installed.  If you want to test before installation, try to provide local installation directory such as:

    $ make PREFIX=`pwd`/root install
    
