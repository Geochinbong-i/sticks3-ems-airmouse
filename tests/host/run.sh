#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build-host
g++ -std=c++17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I main \
  tests/host/test_main.cpp \
  main/timer/ems_timer_core.cpp \
  main/timer/simple_timer_core.cpp \
  main/airmouse/airmouse_math.cpp \
  main/air_harp/air_harp_core.cpp \
  main/ir/ir_signal.cpp \
  main/time_sync_parse.cpp \
  -o build-host/test-core
ASAN_OPTIONS=detect_leaks=0 ./build-host/test-core
