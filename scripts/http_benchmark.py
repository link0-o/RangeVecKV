#!/usr/bin/env python3

import argparse
import http.client
import json
import os
import statistics
import threading
import time
import urllib.parse
from pathlib import Path


def percentile(values, pct):
    if not values:
        return 0.0
    index = int(round((pct / 100.0) * (len(values) - 1)))
    return values[max(0, min(index, len(values) - 1))]


def make_payload(mode, sequence, batch_size):
    now_ms = int(time.time() * 1000)
    key = f"bench-{mode}-{sequence}"
    mutation_id = f"bench-{mode}-{sequence}"
    if mode == "kv":
        return "/v1/kv", {
            "collection": "bench_kv",
            "key": key,
            "value": f"pure kv benchmark payload {sequence}",
            "metadata": {"kind": "benchmark", "mode": "kv"},
            "mutation_id": mutation_id,
            "version": now_ms,
        }
    if mode == "kv_batch":
        records = []
        base = sequence * batch_size
        for offset in range(batch_size):
            item = base + offset
            records.append({
                "collection": "bench_kv",
                "key": f"bench-kv-batch-{item}",
                "value": f"pure kv batch benchmark payload {item}",
                "metadata": {"kind": "benchmark", "mode": "kv_batch"},
                "mutation_id": f"bench-kv-batch-{item}",
                "version": now_ms,
            })
        return "/v1/kv/batch", {"records": records}
    return "/v1/documents", {
        "collection": "bench_documents",
        "key": key,
        "title": f"Vector Benchmark {sequence}",
        "body": (
            "RangeVecKV production benchmark document with semantic vector indexing "
            f"and deterministic unique payload number {sequence}"
        ),
        "metadata": {"kind": "benchmark", "mode": "vector"},
        "mutation_id": mutation_id,
        "version": now_ms,
    }


def open_connection(parsed_url, timeout):
    return http.client.HTTPConnection(parsed_url.hostname, parsed_url.port or 80, timeout=timeout)


def request_once(conn, api_key, mode, sequence, batch_size):
    path, payload = make_payload(mode, sequence, batch_size)
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Content-Length": str(len(body)),
        "Connection": "keep-alive",
    }
    if api_key:
        headers["X-API-Key"] = api_key

    try:
        conn.request("POST", path, body=body, headers=headers)
        response = conn.getresponse()
        response.read()
        if 200 <= response.status < 300:
            return True, None, conn
        return False, f"http_{response.status}", conn
    except Exception as exc:  # noqa: BLE001 - benchmark should count all transport failures.
        try:
            conn.close()
        except Exception:
            pass
        return False, type(exc).__name__, None


def run_benchmark(args):
    parsed_url = urllib.parse.urlparse(args.base_url)
    if parsed_url.scheme != "http" or not parsed_url.hostname:
        raise SystemExit("--base-url must be an http URL such as http://127.0.0.1:8080")

    sequence = 0
    sequence_lock = threading.Lock()
    all_latencies = []
    all_latencies_lock = threading.Lock()
    errors = {}
    errors_lock = threading.Lock()
    successes = 0
    successes_lock = threading.Lock()

    def next_sequence():
        nonlocal sequence
        with sequence_lock:
            sequence += 1
            return sequence

    def worker(stop_time):
        nonlocal successes
        local_latencies = []
        local_errors = {}
        local_successes = 0
        conn = open_connection(parsed_url, args.timeout_seconds)
        while time.perf_counter() < stop_time:
            item = next_sequence()
            start = time.perf_counter_ns()
            ok, error, conn = request_once(conn, args.api_key, args.mode, item, args.batch_size)
            elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000.0
            if ok:
                local_successes += 1
                local_latencies.append(elapsed_ms)
            else:
                local_errors[error or "unknown"] = local_errors.get(error or "unknown", 0) + 1
                if conn is None:
                    conn = open_connection(parsed_url, args.timeout_seconds)
        try:
            conn.close()
        except Exception:
            pass

        with successes_lock:
            successes += local_successes
        with all_latencies_lock:
            all_latencies.extend(local_latencies)
        with errors_lock:
            for error, count in local_errors.items():
                errors[error] = errors.get(error, 0) + count

    if args.warmup_seconds > 0:
        warmup_stop = time.perf_counter() + args.warmup_seconds
        warmup_threads = [threading.Thread(target=worker, args=(warmup_stop,)) for _ in range(args.threads)]
        for thread in warmup_threads:
            thread.start()
        for thread in warmup_threads:
            thread.join()
        with successes_lock:
            successes = 0
        with all_latencies_lock:
            all_latencies.clear()
        with errors_lock:
            errors.clear()

    start_time = time.perf_counter()
    stop_time = start_time + args.duration_seconds
    threads = [threading.Thread(target=worker, args=(stop_time,)) for _ in range(args.threads)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed_seconds = time.perf_counter() - start_time

    latencies = sorted(all_latencies)
    result = {
        "mode": args.mode,
        "base_url": args.base_url,
        "threads": args.threads,
        "duration_seconds": elapsed_seconds,
        "successes": successes,
        "records_per_success": args.batch_size if args.mode == "kv_batch" else 1,
        "record_successes": successes * (args.batch_size if args.mode == "kv_batch" else 1),
        "errors": errors,
        "qps": successes / elapsed_seconds if elapsed_seconds > 0 else 0.0,
        "records_per_second": (successes * (args.batch_size if args.mode == "kv_batch" else 1)) / elapsed_seconds if elapsed_seconds > 0 else 0.0,
        "latency_ms": {
            "avg": statistics.fmean(latencies) if latencies else 0.0,
            "p50": percentile(latencies, 50),
            "p90": percentile(latencies, 90),
            "p95": percentile(latencies, 95),
            "p99": percentile(latencies, 99),
            "max": latencies[-1] if latencies else 0.0,
        },
    }
    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description="RangeVecKV HTTP write benchmark")
    parser.add_argument("--mode", choices=("kv", "kv_batch", "vector"), required=True)
    parser.add_argument("--batch-size", type=int, default=100)
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--api-key", default=os.environ.get("KVAI_API_KEY", ""))
    parser.add_argument("--threads", type=int, default=16)
    parser.add_argument("--duration-seconds", type=float, default=30.0)
    parser.add_argument("--warmup-seconds", type=float, default=5.0)
    parser.add_argument("--timeout-seconds", type=float, default=10.0)
    parser.add_argument("--output", default="")
    args = parser.parse_args()
    if args.threads < 1:
        raise SystemExit("--threads must be >= 1")
    if args.duration_seconds <= 0:
        raise SystemExit("--duration-seconds must be > 0")
    if args.batch_size < 1:
        raise SystemExit("--batch-size must be >= 1")
    run_benchmark(args)


if __name__ == "__main__":
    main()
