#!/usr/bin/env python3
"""Repeatable HTTP concurrency benchmark using only the Python standard library."""

import argparse
import concurrent.futures
import http.client
import json
import os
import statistics
import time
import urllib.parse


def process_metrics(pid):
    if pid is None:
        return None
    result = {"rss_kib": None, "open_fds": None}
    try:
        with open(f"/proc/{pid}/status", encoding="ascii") as status:
            for line in status:
                if line.startswith("VmRSS:"):
                    result["rss_kib"] = int(line.split()[1])
                    break
        result["open_fds"] = len(os.listdir(f"/proc/{pid}/fd"))
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        pass
    return result


def percentile(values, percentage):
    if not values:
        return None
    index = max(0, min(len(values) - 1,
                       int((len(values) - 1) * percentage + 0.5)))
    return round(values[index] * 1000, 3)


def request_once(parsed, path, timeout):
    connection_type = (http.client.HTTPSConnection if parsed.scheme == "https"
                       else http.client.HTTPConnection)
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    started = time.perf_counter()
    connection = connection_type(parsed.hostname, port, timeout=timeout)
    try:
        connection.request("GET", path, headers={"Connection": "close"})
        response = connection.getresponse()
        response.read()
        return response.status, time.perf_counter() - started, None
    except Exception as error:  # Benchmark output must record transport failures.
        return None, time.perf_counter() - started, type(error).__name__
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="server base URL, e.g. http://127.0.0.1:9000")
    parser.add_argument("--path", default="/health")
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument("--concurrency", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--server-pid", type=int)
    parser.add_argument("--output", help="also write the JSON report to this path")
    args = parser.parse_args()
    parsed = urllib.parse.urlsplit(args.url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        parser.error("url must be an http:// or https:// server URL")
    if args.requests <= 0 or args.concurrency <= 0 or args.timeout <= 0:
        parser.error("requests, concurrency and timeout must be positive")

    before = process_metrics(args.server_pid)
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=args.concurrency) as executor:
        results = list(executor.map(
            lambda _: request_once(parsed, args.path, args.timeout),
            range(args.requests)))
    elapsed = time.perf_counter() - started
    after = process_metrics(args.server_pid)
    latencies = sorted(item[1] for item in results)
    statuses = {}
    failures = {}
    for status, _, error in results:
        if status is not None:
            statuses[str(status)] = statuses.get(str(status), 0) + 1
        if error is not None:
            failures[error] = failures.get(error, 0) + 1
    report = {
        "url": urllib.parse.urlunsplit((parsed.scheme, parsed.netloc,
                                        args.path, "", "")),
        "requests": args.requests,
        "concurrency": args.concurrency,
        "elapsed_seconds": round(elapsed, 6),
        "requests_per_second": round(args.requests / elapsed, 2),
        "latency_ms": {
            "mean": round(statistics.fmean(latencies) * 1000, 3),
            "p50": percentile(latencies, 0.50),
            "p95": percentile(latencies, 0.95),
            "p99": percentile(latencies, 0.99),
            "max": round(latencies[-1] * 1000, 3),
        },
        "status_counts": statuses,
        "failure_counts": failures,
        "server_process_before": before,
        "server_process_after": after,
    }
    output = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    print(output, end="")
    if args.output:
        with open(args.output, "w", encoding="utf-8") as destination:
            destination.write(output)
    return 0 if not failures else 2


if __name__ == "__main__":
    raise SystemExit(main())
