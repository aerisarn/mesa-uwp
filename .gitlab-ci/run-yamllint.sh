#!/usr/bin/env bash
set -e

# Run yamllint against all traces files.
find . -name '*traces*yml' -exec yamllint -d "{rules: {line-length: {max: 150}}}" {} \;
