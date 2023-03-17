#!/bin/sh -e

if command -V ccache >/dev/null 2>/dev/null; then
  CCACHE=ccache
else
  CCACHE=
fi

if [ "$(ps -p $(ps -p $PPID -o ppid --no-headers) -o comm --no-headers)" != ninja ]; then
    # Not invoked by ninja (e.g. for a meson feature check)
    exec $CCACHE g++ "$@"
fi

if [ "$(eval printf "'%s'" "\"\${$(($#-1))}\"")" = "-c" ]; then
    # Not invoked for linking
    exec $CCACHE g++ "$@"
fi

# Compiler invoked by ninja for linking. Add -Werror to turn compiler warnings into errors
# with LTO. (meson's werror should arguably do this, but meanwhile we need to)
exec $CCACHE g++ "$@" -Werror
