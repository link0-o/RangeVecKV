# RangeVecKV Operations Guide

## Runtime Modes

- `config/server.yaml`: local development defaults, API key disabled, demo seed enabled
- `config/server.prod.yaml`: production-like defaults, API key enabled, demo seed disabled, environment variable expansion enabled

## Environment Variables

- `KVAI_API_KEY`: API key used when `gateway.require_api_key=true`
- `KVAI_CLUSTER_NODES`: static cluster membership string, format `node-a@10.0.0.1:8080,node-b@10.0.0.2:8080`
- `KVAI_TLS_MODE`: TLS mode flag exposed in health output; current code supports `disabled` placeholder
- `KVAI_AI_BACKEND`: embedding backend; production Compose uses `onnxruntime`
- `KVAI_MODEL_PATH`: ONNX embedding model path
- `KVAI_TOKENIZER_PATH`: WordPiece vocabulary path used by the ONNX adapter

## CI/CD Profiles

The default GitHub Actions path keeps the build lightweight:

- local fallback build and `ctest`
- fallback Docker Compose smoke through `./scripts/http_smoke.sh`
- fallback package artifacts on tags or manual runs

The manual `production_docker=true` workflow path builds the full optional
backend image through Docker/vcpkg, starts etcd plus RangeVecKV, then verifies:

- HTTP compatibility through `./scripts/http_smoke.sh`
- typed Protobuf BRPC writes through `kvai_brpc_kv_benchmark`

This keeps every pull request fast while preserving a heavier production smoke
for release checks or dependency changes.

## Local Service Lifecycle

Build and test:

```bash
./scripts/build_local.sh
```

Start service:

```bash
./scripts/run_local_service.sh
```

Smoke test:

```bash
./scripts/http_smoke.sh http://127.0.0.1:8080
```

Health check only:

```bash
./scripts/healthcheck.sh http://127.0.0.1:8080
```

## Compose Stack

Copy `.env.example` to `.env`, set `KVAI_API_KEY`, then run:

```bash
./scripts/stack_local.sh up
```

Endpoints:

- Application: `http://127.0.0.1:8080`
- etcd: `http://127.0.0.1:2379`

## Runtime Backend Posture

RangeVecKV keeps a dual-mode runtime:

- Local fallback mode remains the default first-run path.
- Production mode prefers BRPC, ONNX Runtime, FAISS, RocksDB, etcd, spdlog, yaml-cpp, and protobuf-backed persistence when those dependencies are available.

Current backend entries:

- `ai.backend=auto|deterministic|onnxruntime`
- `search.backend=auto|brute_force|faiss`
- `storage.backend=auto|wal|rocksdb`
- `cluster.discovery_backend=static|etcd`

## ONNX Multimodal Embedding Model

Download the pinned Chinese CLIP ONNX model and vocabulary before starting
the production Compose stack:

```bash
./scripts/download_ai_model.sh
docker compose up --build -d
```

The model directory is mounted read-only at `/opt/rangeveckv/models`. Production
uses a 512-dimensional index at `/opt/rangeveckv/data/kvai-cnclip.index`.
Documents indexed with a different embedding model or dimension must be
re-ingested or re-indexed.

```bash
docker compose stop rangeveckv
docker compose run --rm rangeveckv \
  --config /opt/rangeveckv/share/rangeveckv/config/server.prod.yaml \
  --collection documents --reindex
docker compose up -d
```

The current adapter supports BERT/MiniLM-style text inputs and Chinese CLIP
multimodal inputs. For Chinese CLIP, text queries use `text_embeds`; image
queries and documents with `metadata.image_path`, `metadata.image_reference`,
or `metadata.image_uri` use `image_embeds`. Image loading supports local
JPEG/PNG paths mounted into the container, for example
`/opt/rangeveckv/images/example.jpg`.

## Pure KV API

Use `/v1/kv` for storage operations that must bypass AI embedding and vector
index maintenance:

```bash
curl -X POST http://127.0.0.1:8080/v1/kv \
  -H "x-api-key: ${KVAI_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"collection":"kv","key":"user:1","value":"plain kv value"}'

curl "http://127.0.0.1:8080/v1/kv?collection=kv&key=user:1" \
  -H "x-api-key: ${KVAI_API_KEY}"

curl -X DELETE "http://127.0.0.1:8080/v1/kv?collection=kv&key=user:1" \
  -H "x-api-key: ${KVAI_API_KEY}"
```

For high-throughput pure KV ingestion, use `POST /v1/kv/batch`. The endpoint
keeps the same version and `mutation_id` checks as single-record writes,
rejects duplicate keys in one request, and is capped by
`gateway.kv_batch_max_records`:

```bash
curl -X POST http://127.0.0.1:8080/v1/kv/batch \
  -H "x-api-key: ${KVAI_API_KEY}" \
  -H "Content-Type: application/json" \
  -d '{"records":[{"collection":"kv","key":"user:2","value":"batch value 1"},{"collection":"kv","key":"user:3","value":"batch value 2"}]}'
```

The semantic document API `/v1/documents` still writes both KV storage and the
vector index. Do not use `/v1/kv` for semantic documents that should appear in
`/v1/search`.

## Production Protobuf BRPC Writes

Production write clients should prefer the typed Protobuf BRPC service in
`proto/search.proto`:

- `KvWriteService.PutKv`
- `KvWriteService.BatchPutKv`
- `KvWriteService.UpsertDocument`

The RPC path carries auth and trace metadata through `RequestContext`, then
reuses the same gateway methods as JSON REST. It preserves API key/Bearer auth,
fixed slot owner checks, version/CAS validation, `mutation_id` idempotency,
RocksDB WAL writes and the vector outbox behavior. Its purpose is to remove
HTTP REST dispatch, JSON body parsing, JSON response serialization and request
attachment copies from the hot write path.

Example production benchmark command:

```bash
./build/production/src/gateway/kvai_brpc_kv_benchmark \
  --server 127.0.0.1:8080 \
  --api-key "${KVAI_API_KEY}" \
  --mode kv \
  --threads 16 \
  --duration-seconds 20 \
  --output perf_results/brpc_kv.json
```

Batch KV benchmark:

```bash
./build/production/src/gateway/kvai_brpc_kv_benchmark \
  --server 127.0.0.1:8080 \
  --api-key "${KVAI_API_KEY}" \
  --mode kv_batch \
  --batch-size 100 \
  --threads 16 \
  --duration-seconds 20 \
  --output perf_results/brpc_kv_batch.json
```

Keep exact QPS and latency numbers with the local benchmark artifacts rather
than in long-lived docs. Always record power mode, Docker/native mode, optional
backend set, data directory freshness, concurrency and batch size beside the
result.

## RocksDB Write Tuning

Production config exposes safe RocksDB knobs:

- `storage.rocksdb_max_background_jobs`
- `storage.rocksdb_write_buffer_size_mb`
- `storage.rocksdb_max_write_buffer_number`
- `storage.rocksdb_target_file_size_mb`
- `storage.rocksdb_bytes_per_sync`
- `storage.rocksdb_wal_bytes_per_sync`
- `storage.rocksdb_enable_pipelined_write`

The default posture keeps `WriteOptions.disableWAL=false` and does not use blind
puts or `KeyMayExist` shortcuts. Single-record writes still read the current
record before writing so stale versions cannot overwrite newer data and repeated
`mutation_id` values remain idempotent.

The project-level vcpkg overlay builds `onnx` with
`ONNX_DISABLE_STATIC_REGISTRATION=ON`, as required by the vcpkg
`onnxruntime` port. Keep `vcpkg-configuration.json` and
`vcpkg-overlay/ports/onnx` together when building outside Docker; otherwise
ONNX Runtime may emit duplicate operator schema registration warnings.

When etcd discovery is enabled, set `cluster.advertise_host` to an address that
other RangeVecKV nodes can reach. The Compose production configuration uses the
`rangeveckv` service name while continuing to listen on `0.0.0.0`.

Each node registers one lease-backed key under `cluster.etcd_prefix`, such as
`/rangeveckv/nodes/node-a`. New nodes write JSON values with `version`, `id`,
`host`, `port`, `healthy`, `weight`, and `zone`; older `host:port` values are
still accepted while rolling upgrades are in progress. Prefix watch uses an
explicit etcd range over the configured prefix and reconnects with backoff after
watch failures. Keepalive and watch status are exposed through `/healthz`
`etcd_discovery_*` details.

Set `cluster.data_migration_enabled=true` to enable asynchronous data
migration after ring rebuilds. A node scans local KV collections, sends records
that now belong to a remote owner to `/internal/migration/records` on that owner,
and keeps the source copy until `cluster.migration_delete_delay_ms` expires.
Pending and delayed-delete tasks are persisted in
`cluster.migration_task_wal_path`, so a restart can recover task state and then
rescan local KV data as a safety net. Routing first maps `collection:key` to a
fixed slot (`cluster.slot_count`) and then resolves the slot owner from the
consistent-hash ring; `/v1/router` exposes `slot_id`.
Migration is best-effort and eventually consistent; it is not a Raft/quorum
replication protocol. Remote-owner write forwarding for ordinary client requests
is still disabled, so those writes return `Unavailable` with the owner endpoint.

Semantic document writes use a persistent vector index outbox. KV is updated
first, then the pending index task is written to
`search.vector_index_outbox_path`; the request path triggers one outbox drain
and a background worker continues computing embeddings and updating the vector
index. Failed tasks remain pending for retry. Record
`version`, `updated_at_unix_ms`, and `mutation_id` fields provide stale-write
rejection and idempotent retries.

Fallback mode is intentionally retained so local build/test is not blocked by large production dependencies. It should not be described as the final production performance profile.

## Persistence

WAL, KV snapshot, vector fallback snapshot, and FAISS metadata snapshot are
written as generated Protobuf messages inside framed records. Older text
snapshots remain readable and are rewritten in the Protobuf format on the next
flush.

Logs are not serialized with protobuf; they use spdlog when available and the fallback logger otherwise.
