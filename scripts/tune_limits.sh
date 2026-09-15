#!/bin/bash

# Run this as: source scripts/tune_limits.sh
# Sourcing is required because a child script cannot raise its caller's limit.

# Display the file-descriptor limits currently configured for this shell.
echo "Current soft limit: $(ulimit -Sn)"
echo "Current hard limit: $(ulimit -Hn)"

# Raise the soft limit for commands started from this shell (when sourced).
ulimit -n 65536

echo "New soft limit for this shell: $(ulimit -Sn)"

# These kernel settings can increase the pending connection queues before a
# load test. Applying them requires administrator privileges on the host.
# sudo sysctl -w net.core.somaxconn=65536
# sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65536
# sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
# sudo sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216"
# sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"

# Containers can receive an equivalent file-descriptor limit at startup:
# docker run --ulimit nofile=200000:200000 ...

# Use these settings as a starting point for Docker-based load testing. Ramp
# connections gradually, then sustain each level to expose leaks, backpressure,
# and queue saturation.
