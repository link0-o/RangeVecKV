# RangeVecKV

中文文档见 [README.zh-CN.md](README.zh-CN.md).

RangeVecKV is a C++17 distributed KV and semantic retrieval engine. It combines a production-oriented KV path with multimodal vector retrieval:

- Pure KV APIs backed by RocksDB or local WAL/snapshot fallback.
- Semantic document search backed by ONNX Runtime embeddings and FAISS or brute-force vector index.
- Chinese text and image retrieval through Chinese-CLIP.
- Production typed Protobuf BRPC writes with JSON REST kept for compatibility and debugging.
- BRPC HTTP gateway, API key auth, metrics, health checks, structured logging, and etcd-based discovery.

The project keeps two runtime profiles:

- Local profile: lightweight fallback backends for quick build, test, and smoke validation.
- Production profile: BRPC, Protobuf, spdlog, yaml-cpp, RocksDB, FAISS, ONNX Runtime, oneTBB, and etcd when built with the vcpkg toolchain.

## Features

- HTTP gateway: `/healthz`, `/metrics`, `/openapi.yaml`, `/v1/search`, `/v1/router`, `/v1/documents`, `/v1/kv`, `/v1/kv/batch`
- Pure KV path: `POST|PUT /v1/kv`, `POST /v1/kv/batch`, `GET /v1/kv`, `DELETE /v1/kv`
- Production write RPC: `KvWriteService` provides typed Protobuf BRPC writes for single KV, batch KV, and document upserts while JSON REST remains compatible.
- Semantic document path: document upsert/delete with embedding and vector index maintenance
- KV/vector compensation outbox: semantic writes persist KV first, then durable index tasks update embeddings/vector index with retry
- Multimodal retrieval: Chinese text queries, image queries, image-backed documents
- Storage: RocksDB backend plus WAL/snapshot fallback
- Persistence: generated Protobuf messages with framed WAL/snapshot/index records
- Search: FAISS backend plus persistent brute-force fallback
- Routing: static membership, etcd discovery, fixed slot partitioning, owner-ring checks
- Migration safety: asynchronous best-effort data migration, migration task WAL, delayed delete, version/mutation idempotency
- Control plane: API key/Bearer auth, rate limiting, trace IDs, read-only mode, graceful shutdown
- Observability: spdlog/fallback logging, Prometheus metrics, health details for backends, etcd discovery state, CPU/disk, and thread pool state

## Architecture

```text
                                +--------------------------------+
                                | Clients / SDKs / Benchmarks    |
                                +---------------+----------------+
                                                |
                                                v
                                +--------------------------------+
                                | Gateway Runtime                |
                                | BRPC Protobuf / BRPC HTTP      |
                                | fallback HTTP                  |
                                +---------------+----------------+
                                                |
                                                v
                                +--------------------------------+
                                | InProcessGatewayServer         |
                                | auth / trace / metrics / docs  |
                                +---------------+----------------+
                                                |
                    +---------------------------+---------------------------+
                    |                           |                           |
                    v                           v                           v
        +-----------------------+   +-----------------------+   +-----------------------+
        | Cluster Control       |   | Pure KV API           |   | Semantic APIs         |
        | etcd / static nodes   |   | /v1/kv                |   | /v1/documents         |
        | slot partition + ring |   | /v1/kv/batch          |   | /v1/search            |
        +-----------+-----------+   +-----------+-----------+   +-----------+-----------+
                    |                           |                           |
        remote owner|                           |                           |
                    v                           v                           v
        +-----------------------+   +-----------------------+   +-----------------------+
        | Unavailable + endpoint|   | KV Store              |   | Vector Outbox         |
        | no client forwarding  |   | RocksDB / WAL fallback|   | durable retry queue   |
        +-----------------------+   +-----------+-----------+   +-----------+-----------+
                                                |                           |
                                                |                           v
                                                |               +-----------------------+
                                                |               | Embedding Service     |
                                                |               | ONNX / deterministic  |
                                                |               +-----------+-----------+
                                                |                           |
                                                |                           v
                                                |               +-----------------------+
                                                |               | Vector Index          |
                                                |               | FAISS / brute force   |
                                                |               +-----------+-----------+
                                                |                           |
                                                v                           v
                                +-----------------------------------------------+
                                | Persistence                                   |
                                | Protobuf WAL / snapshot / RocksDB value codec |
                                +-----------------------------------------------+

        ring change
             |
             v
        +-----------------------+       POST /internal/migration/records
        | Data Migration        |------------------------------------+
        | best-effort retry     |                                    |
        | delayed delete        |--------------------------------+   |
        +-----------------------+                                |   |
                                                                 v   v
                                                        +-----------------------+
                                                        | KV Store + Outbox     |
                                                        | migration apply path  |
                                                        +-----------------------+

                                +--------------------------------+
                                | Operations                     |
                                | healthz / Prometheus / spdlog  |
                                | CPU / disk / thread pool state |
                                +--------------------------------+
```

## Request Flows

Pure KV write `POST /v1/kv`:

```text
Client
  -> Gateway Runtime
  -> API key / Bearer auth + Trace ID
  -> InProcessGatewayServer::PutKvRecord
  -> Fixed slot partition + owner-ring check
  -> Local owner: KV Store Put
  -> RocksDB value / WAL fallback writes Protobuf record
  -> 200 OK
```

Production pure KV write `KvWriteService.PutKv`:

```text
Client / SDK / Benchmark
  -> BRPC Protobuf KvWriteService.PutKv
  -> RequestContext auth + Trace ID
  -> InProcessGatewayServer::PutKvRecord
  -> Fixed slot partition + owner-ring check
  -> Local owner: KV Store Put
  -> RocksDB Get-before-Put validation + WAL write
  -> KvWriteResponse
```

Pure KV batch write `POST /v1/kv/batch`:

```text
Client
  -> Gateway Runtime
  -> Parse records[]
  -> Validate batch size, empty keys and duplicate collection:key
  -> Owner check for every record
  -> KvStore BatchPut
  -> RocksDB WriteBatch / WAL fallback validate-before-write
  -> Return record_count
```

Pure KV read `GET /v1/kv`:

```text
Client
  -> Gateway Runtime
  -> Auth + Trace ID
  -> Key lookup: slot/owner check, then KV Store Get
  -> Range lookup: local collection scan through KV Store Range
  -> Return one record or items[]
```

Semantic document write `POST /v1/documents`:

```text
Client
  -> Gateway Runtime
  -> Auth + Trace ID
  -> Owner check
  -> KV Store Put persists the source record first
  -> Vector Index Outbox stores durable indexing task
  -> Request path triggers one DrainOnce; background worker keeps retrying
  -> Generate embedding
  -> Vector Index Upsert
  -> Failed tasks remain retryable
```

Semantic search `GET|POST /v1/search`:

```text
Client
  -> Gateway Runtime
  -> Search rate limit + Trace ID
  -> Embedding Service builds query/image embedding
  -> Vector Index TopK
  -> KV Store MultiGet fills title/body/metadata
  -> Timeout/backend failures use degraded fallback search
  -> Return hits[]
```

Remote owner and migration flow:

```text
Ordinary client write
  -> Route owner is remote
  -> Return Unavailable + target endpoint
  -> No automatic client-write forwarding yet

Ring/node change
  -> DataMigrationManager scans local KV
  -> Finds records whose new owner is remote
  -> POST /internal/migration/records to target node
  -> Target writes KV / Outbox locally
  -> Source performs delayed delete after rechecking owner
```

Health and observability:

```text
/healthz
  -> storage/search/etcd/thread-pool/migration/outbox/CPU/disk status

/metrics
  -> Prometheus text metrics

structured logs
  -> spdlog file logger / fallback logger
```

## Repository Layout

```text
config/       Development and production configuration
docs/         OpenAPI, operations, and testing notes
proto/        RPC and persistence protobuf schema
scripts/      Build, model download, Docker, smoke, and packaging helpers
src/ai/       Embedding service and ONNX/fallback implementations
src/core/     KV store, persistence codec, WAL/snapshot support
src/gateway/  HTTP/BRPC gateway, auth, routing, semantic service
src/infra/    Config, logging, metrics, thread pool, discovery, routing
src/search/   Vector index abstraction and FAISS/fallback implementations
test/         Unit and smoke tests
```

## Local Build

Default local builds do not require the full production dependency set. They do require the common build tools plus Protobuf and nlohmann-json because persistence records are generated from `proto/search.proto`.

On Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build protobuf-compiler libprotobuf-dev nlohmann-json3-dev
```

You can also let the repository bootstrap script install the local toolchain:

```bash
./scripts/bootstrap_dev_env.sh
export PATH="$HOME/.local/bin:$PATH"
```

Then build and test:

```bash
./scripts/build_local.sh
```

Start the local service:

```bash
./scripts/run_local_service.sh
```

Run the HTTP smoke test:

```bash
./scripts/http_smoke.sh http://127.0.0.1:8080
```

To attempt a full dependency build through vcpkg:

```bash
export KVAI_USE_VCPKG_TOOLCHAIN=1
./scripts/build_local.sh
```

## Production Compose

Download the pinned Chinese-CLIP ONNX model, then build and start the stack:

```bash
./scripts/download_ai_model.sh
docker compose up --build -d
```

Production Compose expects `.env` values such as `KVAI_API_KEY`, `KVAI_MODEL_PATH`, `KVAI_TOKENIZER_PATH`, `KVAI_AI_BACKEND`, and etcd/discovery settings. Use `.env.example` as the template.

Docker builds support two profiles:

- `KVAI_DOCKER_OPTIONAL_BACKENDS=ON`: production profile with vcpkg dependencies, preferring BRPC, ONNX Runtime, FAISS, RocksDB, etcd-cpp-api, spdlog, yaml-cpp, and oneTBB.
- `KVAI_DOCKER_OPTIONAL_BACKENDS=OFF`: lightweight fallback profile for CI smoke and local verification; use deterministic embedding when ONNX Runtime is not present.

The default CI Docker smoke path uses the fallback profile for speed and reproducibility. The manual production Docker job builds the full optional-backend image and verifies both HTTP smoke and Protobuf BRPC write smoke.

The model is mounted read-only at:

```text
/opt/rangeveckv/models/chinese-clip-vit-base-patch16/
```

Images for image embedding/search should be placed under the host `images/` directory and referenced inside the container as:

```text
/opt/rangeveckv/images/example.jpg
```

## etcd Discovery

When `cluster.discovery_backend=etcd`, each node registers a lease-backed key under `cluster.etcd_prefix`, for example `/rangeveckv/nodes/node-a`. New registrations write a JSON value:

```json
{"version":1,"id":"node-a","host":"rangeveckv-a","port":8080,"healthy":true,"weight":100,"zone":"default"}
```

Older `host:port` values remain readable. Prefix watching uses an explicit range over the configured prefix; watch disconnects are reloaded and reconnected with backoff. Keepalive/watch state is exposed in `/healthz` through `etcd_discovery_*` fields.

Optional data migration is available through `cluster.data_migration_enabled=true`. When enabled, ring changes trigger an asynchronous best-effort scan: records that now belong to a remote owner are sent to that owner through the internal `/internal/migration/records` endpoint, then kept locally until `cluster.migration_delete_delay_ms` expires. Migration tasks are persisted in `cluster.migration_task_wal_path`, and routing now maps `collection:key` to a fixed slot before resolving the slot owner; `/v1/router` returns `slot_id`. This is eventual consistency, not Raft/quorum replication. Ordinary client writes are still not remote-forwarded; remote-owner writes return `Unavailable` with the target node endpoint.

Semantic writes use a KV/vector-index compensation outbox. The document is first persisted in KV, then an index task is written to `search.vector_index_outbox_path`; the request path triggers one outbox drain, and the background worker keeps processing pending tasks for retry. Records carry `version`, `updated_at_unix_ms` and `mutation_id`, so stale mutations are rejected and repeated mutation IDs are idempotent.

## API Examples

Pure KV write:

```bash
curl -X POST http://127.0.0.1:8080/v1/kv \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"collection":"kv","key":"user:1","value":"plain kv value"}'
```

Pure KV read:

```bash
curl "http://127.0.0.1:8080/v1/kv?collection=kv&key=user:1" \
  -H "x-api-key: <api-key>"
```

Pure KV batch write:

```bash
curl -X POST http://127.0.0.1:8080/v1/kv/batch \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"records":[{"collection":"kv","key":"user:2","value":"batch value 1"},{"collection":"kv","key":"user:3","value":"batch value 2"}]}'
```

Semantic document upsert:

```bash
curl -X POST http://127.0.0.1:8080/v1/documents \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"collection":"documents","key":"doc-1","title":"中文向量检索","body":"RangeVecKV 支持中文语义搜索。"}'
```

Text search:

```bash
curl "http://127.0.0.1:8080/v1/search?q=中文语义搜索&top_k=5" \
  -H "x-api-key: <api-key>"
```

Image search:

```bash
curl -X POST http://127.0.0.1:8080/v1/search \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"image_path":"/opt/rangeveckv/images/example.jpg","top_k":5}'
```

## Persistence

RangeVecKV uses generated Protobuf messages for internal persistence:

- `DocumentRecord`
- `WalEntry`
- `VectorEntry`

The records are stored in a simple length-framed stream with a `KVAI-PB1` magic header. RocksDB values, WAL/snapshot records, brute-force vector snapshots, and FAISS metadata use this codec. Older text persistence formats remain readable and are rewritten in the Protobuf format on the next flush.

JSON REST remains compatible. Production write clients should prefer typed Protobuf BRPC to reduce HTTP/JSON parsing, response serialization, and request attachment copies.

The RocksDB production defaults keep WAL enabled and add safe tuning knobs for background jobs, write buffers, target file size, bytes-per-sync, WAL bytes-per-sync and pipelined writes. RangeVecKV does not disable WAL by default and does not skip the get-before-put validation used for version/CAS and `mutation_id` idempotency.

## Reindexing

Changing the embedding model or embedding dimension requires reindexing existing semantic documents:

```bash
docker compose stop rangeveckv
docker compose run --rm rangeveckv \
  --config /opt/rangeveckv/share/rangeveckv/config/server.prod.yaml \
  --collection documents --reindex
docker compose up -d
```

Pure KV records stored through `/v1/kv` or `/v1/kv/batch` intentionally bypass embedding and vector index updates. Batch writes keep the same version and `mutation_id` checks, reject duplicate keys in one request, and are bounded by `gateway.kv_batch_max_records`. They are not returned by semantic search unless separately written or reindexed through the document path.

## Current Boundaries

- TLS/mTLS is still a configuration placeholder.
- Fallback HTTP runtime is for local validation; production should use BRPC.
- Deterministic embedding is a development fallback, not a production retrieval model.
- JSON HTTP benchmark artifacts can be generated with `scripts/http_benchmark.py`; production Protobuf BRPC write benchmarks use `kvai_brpc_kv_benchmark`, for example `./build/production/src/gateway/kvai_brpc_kv_benchmark --server 127.0.0.1:8080 --api-key "$KVAI_API_KEY" --mode kv --threads 16 --duration-seconds 20 --output perf_results/brpc_kv.json`.
- Protobuf batch KV benchmarks use `--mode kv_batch --batch-size 100`. Exact results are environment-specific; record power mode, Docker profile, backend set, data directory state, concurrency and batch size with each run.
- Benchmark and perf outputs under `perf_results/` are ignored by git.

## License

RangeVecKV is released under the [MIT License](LICENSE).
