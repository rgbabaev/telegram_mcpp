import asyncio
from contextlib import asynccontextmanager
import json
import os
from pathlib import Path
import platform
import re
import time


def children():
    result = set()
    for task in Path("/proc/self/task").iterdir():
        try:
            result.update(int(value) for value in (task / "children").read_text().split())
        except FileNotFoundError:
            continue
    return result


def server_pid(previous, executable):
    expected = Path(executable).resolve()
    matches = [pid for pid in children() - previous if Path(f"/proc/{pid}/exe").resolve() == expected]
    if len(matches) != 1:
        raise RuntimeError("Profiling requires a directly launched local executable")
    return matches[0]


def sample(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(") ", 1)[1].split()
    cpu = (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    status = Path(f"/proc/{pid}/status").read_text()
    rss = int(re.search(r"^VmRSS:\s+(\d+)", status, re.MULTILINE).group(1))
    return cpu, rss


@asynccontextmanager
async def measure(pid, phase):
    started = time.monotonic()
    cpu_before, initial_rss = sample(pid)
    peak_rss = initial_rss
    finished = asyncio.Event()

    async def monitor():
        nonlocal peak_rss
        while not finished.is_set():
            peak_rss = max(peak_rss, sample(pid)[1])
            try:
                await asyncio.wait_for(finished.wait(), timeout=0.02)
            except asyncio.TimeoutError:
                continue

    task = asyncio.create_task(monitor())
    try:
        yield
    finally:
        finished.set()
        await task
        cpu_after, final_rss = sample(pid)
        elapsed = time.monotonic() - started
        print(json.dumps({"phase": phase, "elapsed_seconds": round(elapsed, 4),
                          "cpu_seconds": round(cpu_after - cpu_before, 4),
                          "cpu_percent_one_core": round(100 * (cpu_after - cpu_before) / elapsed, 2),
                          "rss_initial_kib": initial_rss, "rss_final_kib": final_rss,
                          "rss_sampled_peak_kib": max(peak_rss, final_rss)}), flush=True)


def describe(build, fixture):
    cpu = Path("/proc/cpuinfo").read_text()
    model = re.search(r"^model name\s*:\s*(.*)", cpu, re.MULTILINE)
    cmake = (Path(__file__).resolve().parents[1] / "cmake/TDLib.cmake").read_text()
    revision = re.search(r'TELEGRAM_MCPP_TDLIB_REVISION "([a-f0-9]+)"', cmake).group(1)
    result = {"os": platform.platform(), "cpu": model.group(1) if model else platform.machine(),
              "logical_cpus": os.cpu_count(), "build": build, "tdlib_revision": revision,
              "backend": "simulated" if fixture else "TDLib saved session", "sampling_seconds": 0.02}
    for name, path in (("cgroup_cpu_max", "/sys/fs/cgroup/cpu.max"),
                       ("cgroup_memory_max", "/sys/fs/cgroup/memory.max")):
        if Path(path).exists():
            result[name] = Path(path).read_text().strip()
    result["host_memory_kib"] = int(re.search(r"^MemTotal:\s+(\d+)", Path("/proc/meminfo").read_text(), re.MULTILINE).group(1))
    print(json.dumps(result), flush=True)
