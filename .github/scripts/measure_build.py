import json
import os
from pathlib import Path
import subprocess
import sys
import time


def process_tree_rss_kib(root_pid):
    processes = {}
    root_rss_kib = 0
    output = subprocess.check_output(["ps", "-eo", "pid=,ppid=,rss="], text=True)
    for line in output.splitlines():
        pid, parent_pid, rss_kib = map(int, line.split())
        if pid == root_pid:
            root_rss_kib = rss_kib
        processes.setdefault(parent_pid, []).append((pid, rss_kib))

    pending = [root_pid]
    total = root_rss_kib
    while pending:
        parent_pid = pending.pop()
        for pid, rss_kib in processes.get(parent_pid, []):
            total += rss_kib
            pending.append(pid)
    return total


def runner_memory_kib():
    fields = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        name, value = line.split(":", 1)
        fields[name] = int(value.split()[0])
    return fields


def report_metrics(metrics):
    print("CI_BUILD_METRICS=" + json.dumps(metrics), flush=True)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as output:
            output.write("| Build metric | Value |\n|---|---:|\n")
            for name, value in metrics.items():
                output.write(f"| {name} | {value} |\n")
            output.write("\nRSS samples sum child processes every 0.2s; shared pages may be counted multiple times. Available memory covers the runner.\n")


def main():
    started = time.monotonic()
    peak_rss_kib = 0
    minimum_available_kib = runner_memory_kib()["MemAvailable"]
    with subprocess.Popen(sys.argv[1:]) as process:
        while True:
            peak_rss_kib = max(peak_rss_kib, process_tree_rss_kib(process.pid))
            minimum_available_kib = min(minimum_available_kib, runner_memory_kib()["MemAvailable"])
            exit_code = process.poll()
            if exit_code is not None:
                break
            time.sleep(0.2)

    report_metrics({
        "elapsed_seconds": round(time.monotonic() - started, 2),
        "sampled_peak_process_tree_rss_mib": round(peak_rss_kib / 1024, 2),
        "minimum_runner_available_memory_mib": round(minimum_available_kib / 1024, 2),
        "runner_total_memory_mib": round(runner_memory_kib()["MemTotal"] / 1024, 2),
        "logical_cpus": os.cpu_count(),
        "exit_code": exit_code,
    })
    return exit_code if exit_code >= 0 else 128 - exit_code


if __name__ == "__main__":
    sys.exit(main())
