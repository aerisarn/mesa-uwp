#!/bin/bash

set -ex

# Pull down repositories that crosvm depends on to cros checkout-like locations.
CROS_ROOT=/
THIRD_PARTY_ROOT=$CROS_ROOT/third_party
mkdir -p $THIRD_PARTY_ROOT
AOSP_EXTERNAL_ROOT=$CROS_ROOT/aosp/external
mkdir -p $AOSP_EXTERNAL_ROOT
PLATFORM2_ROOT=/platform2

PLATFORM2_COMMIT=2079dd5fcd61f1ac39e2fc16595956617f3f1e9e
git clone --single-branch --no-checkout https://chromium.googlesource.com/chromiumos/platform2 $PLATFORM2_ROOT
pushd $PLATFORM2_ROOT
git checkout $PLATFORM2_COMMIT
popd

# minijail does not exist in upstream linux distros.
MINIJAIL_COMMIT=5f9e3001c61626d2863dad91248ba8496c3ef511
git clone --single-branch --no-checkout https://android.googlesource.com/platform/external/minijail $AOSP_EXTERNAL_ROOT/minijail
pushd $AOSP_EXTERNAL_ROOT/minijail
git checkout $MINIJAIL_COMMIT
make
cp libminijail.so /usr/lib/x86_64-linux-gnu/
popd

# Pull the cras library for audio access.
ADHD_COMMIT=5068bdd18b51de8f2d5bcff754cdecda80de8f44
git clone --single-branch --no-checkout https://chromium.googlesource.com/chromiumos/third_party/adhd $THIRD_PARTY_ROOT/adhd
pushd $THIRD_PARTY_ROOT/adhd
git checkout $ADHD_COMMIT
popd

CROSVM_VERSION=f70350ba51e9631e3b7fe711c0296e041a61a499
git clone --single-branch --no-checkout https://chromium.googlesource.com/chromiumos/platform/crosvm /platform/crosvm
pushd /platform/crosvm
git checkout "$CROSVM_VERSION"

RUSTFLAGS='-L native=/usr/local/lib' cargo install \
  -j ${FDO_CI_CONCURRENT:-4} \
  --locked \
  --features 'default-no-sandbox gpu x virtio-gpu-next' \
  --path . \
  --root /usr/local \
  $EXTRA_CARGO_ARGS

popd

rm -rf $PLATFORM2_ROOT $AOSP_EXTERNAL_ROOT/minijail $THIRD_PARTY_ROOT/adhd /platform/crosvm
