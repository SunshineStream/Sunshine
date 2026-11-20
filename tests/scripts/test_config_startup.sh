#!/bin/bash

## @file tests/scripts/test_config_startup.sh
## @brief Verify command-line config loading and diagnostics through the Sunshine executable on Linux.

set -euo pipefail

sunshine_executable=${1:?Pass the path to the Sunshine executable}
test_root=$(mktemp -d)
trap 'rm -rf -- "$test_root"' EXIT
printf '{"apps":[]}\n' > "$test_root/apps.json"

## @brief Start Sunshine with isolated app data and a log file, then exit through --version.
## @param $1 Scenario name used for paths. Remaining arguments are passed through to Sunshine.
run_sunshine() {
  local scenario=$1
  shift
  CONFIGURATION_DIRECTORY="$test_root/$scenario" SUNSHINE_MIGRATE_CONFIG=0 \
    "$sunshine_executable" "$@" "file_apps=$test_root/apps.json" "log_path=$test_root/$scenario.log" --version
}

run_sunshine empty ""
grep -Fq 'Warning: The configuration file path argument is empty.' "$test_root/empty.log"

run_sunshine default
test -f "$test_root/default/sunshine/sunshine.conf"
test -s "$test_root/default.log"
if grep -Fq 'Starting without a configuration file.' "$test_root/default.log"; then
  exit 1
fi

run_sunshine pipe <(printf 'encoder = software\n') output_name=first output_name=second
grep -Fq "config: 'encoder' = software" "$test_root/pipe.log"
grep -Fq "config: 'output_name' = second" "$test_root/pipe.log"
