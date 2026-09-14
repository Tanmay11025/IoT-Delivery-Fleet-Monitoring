#!/usr/bin/env bash
set -euo pipefail

docker compose -f docker/docker-compose.loadtest.yml up \
  --build \
  --abort-on-container-exit \
  --exit-code-from loadtest \
  --remove-orphans
