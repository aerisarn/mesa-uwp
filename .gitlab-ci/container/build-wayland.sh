#!/bin/bash

set -ex

export LIBWAYLAND_VERSION="1.18.0"

git clone https://gitlab.freedesktop.org/wayland/wayland
cd wayland
git checkout "$LIBWAYLAND_VERSION"
meson -Ddocumentation=false -Ddtd_validation=false -Dlibraries=true _build
ninja -C _build install
cd ..
rm -rf wayland
