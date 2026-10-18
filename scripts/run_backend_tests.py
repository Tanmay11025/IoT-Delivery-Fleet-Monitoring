#!/usr/bin/env python3
"""Run concurrency, graceful-shutdown, and persistence checks by stage."""

import json
import os
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPOSE_FILE = "docker/docker-compose.backend-tests.yml"
STAGES = (100, 500, 1000, 2500, 5000, 10000, 20000, 30000, 50000)
RESULTS = ROOT / "build/backend-test-results"


def command(args, env=None, capture=False, check=True):
    return subprocess.run(
        ["docker", "compose", "-p", "backend-tests", "-f", COMPOSE_FILE, *args],
        cwd=ROOT, env=env,
        text=True, stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None, check=check,
    )


def gateway_metrics():
    request = b"GET /metrics HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    with socket.create_connection(("127.0.0.1", 8080), timeout=1) as client:
        client.settimeout(2)
        client.sendall(request)
        chunks = []
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    body = b"".join(chunks).split(b"\r\n\r\n", 1)[-1]
    samples = {}
    for line in body.decode("ascii", "replace").splitlines():
        if not line or line.startswith("#"):
            continue
        name, value = line.rsplit(" ", 1)
        samples[name] = float(value)
    return samples


def wait_gateway(timeout=60):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            return gateway_metrics()
        except OSError as error:
            last_error = error
            time.sleep(0.25)
    raise TimeoutError(f"gateway did not become ready: {last_error}")


def collect_maxima(maxima, samples):
    for key, value in samples.items():
        if key.startswith(("gateway_thread_pool_queue", "gateway_epoll_worker_",
                           "gateway_http_responses_total{")):
            maxima[key] = max(maxima.get(key, 0), value)
    for key in ("gateway_connections_active", "gateway_thread_pool_busy_threads",
                "gateway_thread_pool_workers", "gateway_http_requests_blocked_total",
                "gateway_http_parse_errors_total"):
        maxima[key] = max(maxima.get(key, 0), samples.get(key, 0))
    maxima["gateway_connections_accepted_total"] = max(
        maxima.get("gateway_connections_accepted_total", 0),
        samples.get("gateway_connections_accepted_total", 0),
    )
    maxima["gateway_http_requests_total"] = max(
        maxima.get("gateway_http_requests_total", 0),
        samples.get("gateway_http_requests_total", 0),
    )


def docker_stats():
    container_id = subprocess.run(
        ["docker", "compose", "-p", "backend-tests", "-f", COMPOSE_FILE,
         "ps", "-q", "gateway"], cwd=ROOT, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
    ).stdout.strip()
    if not container_id:
        return ""
    result = subprocess.run(
        ["docker", "stats", "--no-stream", "--format", "{{json .}}", container_id],
        cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    return result.stdout.strip()


def run_stage(stage):
    stage_name = str(stage)
    run_id = uuid.uuid4().hex[:12]
    storage = f"/tmp/backend-test-storage/{run_id}/stage-{stage_name}"
    env = os.environ.copy()
    env.update({"STAGE": stage_name, "CONCURRENCY": stage_name,
                "STORAGE_DIRECTORY": storage})
    for suffix in ("ready", "release"):
        (RESULTS / f"{suffix}-{stage_name}").unlink(missing_ok=True)
    (RESULTS / f"ack-{stage_name}.txt").unlink(missing_ok=True)

    command(["up", "-d", "--force-recreate", "gateway"], env=env)
    initial = wait_gateway()
    load = command(["run", "-d", "--no-deps", "backend-test"], env=env,
                   capture=True).stdout.strip().splitlines()[-1]
    deadline = time.monotonic() + max(300, stage / 50)
    ready_file = RESULTS / f"ready-{stage_name}"
    resource_samples = []
    last_resource_sample = 0.0
    while not ready_file.exists() and time.monotonic() < deadline:
        state = subprocess.run(["docker", "inspect", "--format", "{{.State.Running}}", load],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               check=False).stdout.strip()
        if state != "true":
            break
        if time.monotonic() - last_resource_sample >= 1:
            resource_samples.append(docker_stats())
            last_resource_sample = time.monotonic()
        time.sleep(0.05)
    if not ready_file.exists():
        logs = subprocess.run(["docker", "logs", load], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              check=False).stdout
        raise RuntimeError(f"stage {stage} failed before connection barrier:\n{logs}")

    ready_data = json.loads(ready_file.read_text())
    maxima = {}
    resource_samples.append(docker_stats())
    ready_metrics = gateway_metrics()
    accepted_deadline = time.monotonic() + 10
    accepted_key = "gateway_connections_accepted_total"
    while (ready_metrics.get(accepted_key, 0) < ready_data["connected"] and
           time.monotonic() < accepted_deadline):
        time.sleep(0.05)
        ready_metrics = gateway_metrics()
    collect_maxima(maxima, ready_metrics)
    (RESULTS / f"release-{stage_name}").touch()

    progress_deadline = time.monotonic() + max(300, stage / 50)
    progress_seen = False
    load_window_started = time.monotonic()
    latest_metrics = ready_metrics
    while time.monotonic() < progress_deadline:
        try:
            latest_metrics = gateway_metrics()
            collect_maxima(maxima, latest_metrics)
        except OSError:
            pass
        logs = subprocess.run(["docker", "logs", load], text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              check=False).stdout
        if "LOAD_IN_PROGRESS" in logs:
            progress_seen = True
            break
        state = subprocess.run(["docker", "inspect", "--format", "{{.State.Running}}", load],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               check=False).stdout.strip()
        if state != "true":
            break
        if time.monotonic() - last_resource_sample >= 1:
            resource_samples.append(docker_stats())
            last_resource_sample = time.monotonic()
        time.sleep(0.05)

    inflight_at_shutdown = 0
    shutdown_snapshot = latest_metrics
    try:
        shutdown_snapshot = gateway_metrics()
        collect_maxima(maxima, shutdown_snapshot)
        response_total = sum(value for key, value in shutdown_snapshot.items()
                             if key.startswith("gateway_http_responses_total{"))
        inflight_at_shutdown = max(
            0, int(shutdown_snapshot.get("gateway_http_requests_total", 0) - response_total - 1)
        )
    except OSError:
        pass

    resource_samples.append(docker_stats())
    shutdown_started = time.monotonic()
    stop_process = subprocess.Popen(
        ["docker", "compose", "-p", "backend-tests", "-f", COMPOSE_FILE,
         "stop", "--timeout", "120", "gateway"], cwd=ROOT, env=env,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    while stop_process.poll() is None:
        try:
            shutdown_snapshot = gateway_metrics()
            collect_maxima(maxima, shutdown_snapshot)
        except OSError:
            pass
        if time.monotonic() - last_resource_sample >= 1:
            resource_samples.append(docker_stats())
            last_resource_sample = time.monotonic()
        time.sleep(0.05)
    stop_output = stop_process.stdout.read() if stop_process.stdout else ""
    shutdown_seconds = time.monotonic() - shutdown_started
    final_worker_metrics = shutdown_snapshot

    subprocess.run(["docker", "wait", load], text=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    logs = subprocess.run(["docker", "logs", load], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          check=False).stdout
    summary_line = next((line.removeprefix("STAGE_RESULT ") for line in logs.splitlines()
                         if line.startswith("STAGE_RESULT ")), None)
    load_summary = json.loads(summary_line) if summary_line else {}
    submit_seconds = max(load_summary.get("request_submission_seconds", 0), 0.001)
    per_worker_rates = {}
    for key, value in shutdown_snapshot.items():
        if key.startswith("gateway_epoll_worker_requests_total{"):
            worker = key.split('worker="', 1)[1].split('"', 1)[0]
            start_value = ready_metrics.get(key, 0)
            per_worker_rates[worker] = max(0, value - start_value) / submit_seconds
    subprocess.run(["docker", "rm", "-f", load], cwd=ROOT, text=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    recovery = command(["run", "--rm", "--no-deps", "--entrypoint",
                        "./build/storage_recovery_verify", "gateway", storage,
                        f"/test-results/ack-{stage_name}.txt"], env=env,
                       capture=True, check=False)
    recovery_passed = recovery.returncode == 0
    command(["start", "gateway"], env=env)
    restart_metrics = wait_gateway()
    command(["stop", "--timeout", "120", "gateway"], env=env,
            capture=True, check=False)
    result = {
        "stage": stage,
        "run_id": run_id,
        "ready": ready_data,
        "initial_metrics": initial,
        "ready_metrics": ready_metrics,
        "max_metrics": maxima,
        "final_worker_metrics": {
            key: value for key, value in final_worker_metrics.items()
            if key.startswith("gateway_epoll_worker_")
        },
        "requests_per_second_by_epoll_worker_during_submit_window": per_worker_rates,
        "inflight_estimate_at_shutdown": inflight_at_shutdown,
        "load": load_summary,
        "graceful_shutdown": {
            "progress_marker_seen": progress_seen,
            "compose_stop_exit_code": stop_process.returncode,
            "compose_stop_output": stop_output.strip(),
            "duration_seconds": shutdown_seconds,
        },
        "gateway_docker_stats_samples": resource_samples,
        "restart_metrics": restart_metrics,
        "recovery": {
            "passed": recovery_passed,
            "output": recovery.stdout.strip(),
            "exit_code": recovery.returncode,
        },
    }
    with (RESULTS / "stage-results.jsonl").open("a", encoding="utf-8") as output:
        output.write(json.dumps(result, sort_keys=True) + "\n")
    print("STAGE_REPORT " + json.dumps(result, sort_keys=True), flush=True)
    if not progress_seen or not recovery_passed or not load_summary:
        return False
    return True


def main():
    RESULTS.mkdir(parents=True, exist_ok=True)
    (RESULTS / "stage-results.jsonl").write_text("", encoding="utf-8")
    command(["build", "gateway"])
    passed = True
    try:
        selected_stages = tuple(int(value) for value in
                                os.environ.get("BACKEND_TEST_STAGES", "").split(",")
                                if value) or STAGES
        for stage in selected_stages:
            passed = run_stage(stage) and passed
    finally:
        command(["down"], check=False)
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"backend test orchestration failed: {error}", file=sys.stderr)
        raise SystemExit(1)