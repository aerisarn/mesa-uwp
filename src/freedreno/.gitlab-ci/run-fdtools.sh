#!/bin/bash

set -e
set -x

# Also, while we are still using headergen2 for generating kernel
# headers, make sure that doesn't break:
headergen="_build/src/freedreno/rnn/headergen2"
$headergen adreno.xml
$headergen msm.xml
