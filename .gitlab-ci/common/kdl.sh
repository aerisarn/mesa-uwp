#!/usr/bin/env bash
# shellcheck disable=SC1091  # the path is created in build-kdl and
# here is check if exist

terminate() {
  echo "ci-kdl.sh caught SIGTERM signal! propagating to child processes"
  for job in $(jobs -p)
  do
    kill -15 "$job"
  done
}

trap terminate SIGTERM

if [ -f /ci-kdl.venv/bin/activate ]; then
  source /ci-kdl.venv/bin/activate
  echo -e "Launch ci-kdl"
  /ci-kdl.venv/bin/python /ci-kdl.venv/bin/ci-kdl | tee -a /results/kdl.log &
  child=$!
  wait $child
  ls -l
  mv kdl_*.json /results/kdl.json
  echo -e "ci-kdl json file moved to /results"
  ls -ls /results
else
  echo -e "Not possible to activate ci-kdl virtual environment"
fi

