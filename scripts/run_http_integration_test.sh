#!/usr/bin/env bash
set -euo pipefail

docker compose -f docker/docker-compose.http-test.yml up \
  --build \
  --abort-on-container-exit \
  --exit-code-from http-test \
  --remove-orphans
