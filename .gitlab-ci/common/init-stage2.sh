#!/bin/sh

# Second-stage init, used to set up devices and our job environment before
# running tests.

. /set-job-env-vars.sh

set -ex

# Set up any devices required by the jobs
[ -z "$HWCI_KERNEL_MODULES" ] || (echo -n $HWCI_KERNEL_MODULES | xargs -d, -n1 /usr/sbin/modprobe)

# Fix prefix confusion: the build installs to $CI_PROJECT_DIR, but we expect
# it in /install
ln -sf $CI_PROJECT_DIR/install /install
export LD_LIBRARY_PATH=/install/lib
export LIBGL_DRIVERS_PATH=/install/lib/dri

# Store Mesa's disk cache under /tmp, rather than sending it out over NFS.
export XDG_CACHE_HOME=/tmp

# Make sure Python can find all our imports
export PYTHONPATH=$(python3 -c "import sys;print(\":\".join(sys.path))")

# Start a little daemon to capture the first devcoredump we encounter.  (They
# expire after 5 minutes, so we poll for them).
./capture-devcoredump.sh &

# If we want Xorg to be running for the test, then we start it up before the
# BARE_METAL_TEST_SCRIPT because we need to use xinit to start X (otherwise
# without using -displayfd you can race with Xorg's startup), but xinit will eat
# your client's return code
if [ -n "$HWCI_START_XORG" ]; then
  echo "touch /xorg-started; sleep 100000" > /xorg-script
  env \
    xinit /bin/sh /xorg-script -- /usr/bin/Xorg -noreset -s 0 -dpms -logfile /Xorg.0.log &

  # Wait for xorg to be ready for connections.
  for i in 1 2 3 4 5; do
    if [ -e /xorg-started ]; then
      break
    fi
    sleep 5
  done
  export DISPLAY=:0
fi

RESULT=fail
if sh $BARE_METAL_TEST_SCRIPT; then
  RESULT=pass
fi

# upload artifacts via webdav
WEBDAV=$(cat /proc/cmdline | tr " " "\n" | grep webdav | cut -d '=' -f 2 || true)
if [ -n "$WEBDAV" ]; then
  find /results -type f -exec curl -T {} $WEBDAV/{} \;
fi

echo "hwci: mesa: $RESULT"
