#!/bin/sh -e

if [ "$(ps -p $(ps -p $PPID -o ppid --no-headers) -o comm --no-headers)" != ninja ]; then
    # Not invoked by ninja (e.g. for a meson feature check)
    exec ccache g++ "$@"
fi

if [ "$(eval printf "'%s'" "\"\${$(($#-1))}\"")" = "-c" ]; then
    # Not invoked for linking
    exec ccache g++ "$@"
fi

# Compiler invoked by ninja for linking. Add -Werror to turn compiler warnings into errors
# with LTO. (meson's werror should arguably do this, but meanwhile we need to)
exec ccache g++ "$@" -Werror
