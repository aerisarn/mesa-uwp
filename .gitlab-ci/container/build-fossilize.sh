#!/bin/bash

set -ex

git clone https://github.com/ValveSoftware/Fossilize.git
cd Fossilize
git checkout b43ee42bbd5631ea21fe9a2dee4190d5d875c327
git submodule update --init
mkdir build
cd build
cmake -S .. -B . -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C . install
cd ../..
rm -rf Fossilize
