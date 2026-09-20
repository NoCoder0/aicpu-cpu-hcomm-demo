#!/bin/bash
set -eu
cd "$(dirname "$0")"
gcc -std=gnu11 -O2 -Wall -Wextra host_server.c -libverbs -o host_server
