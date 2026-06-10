# RangeVecKV

中文文档见 [README.zh-CN.md](README.zh-CN.md).

RangeVecKV is a C++17 distributed KV and semantic retrieval engine. It combines a production-oriented KV path with multimodal vector retrieval:

- Pure KV APIs backed by RocksDB or local WAL/snapshot fallback.
- Semantic document search backed by ONNX Runtime embeddings and FAISS or brute-force vector index.
- Chinese text and image retrieval through Chinese-CLIP.
- BRPC HTTP gateway, API key auth, metrics, health checks, structured logging, and etcd-based discovery.

The project keeps two runtime profiles:

- Local profile: lightweight fallback backends for quick build, test, and smoke validation.
- Production profile: BRPC, Protobuf, spdlog, yaml-cpp, RocksDB, FAISS, ONNX Runtime, oneTBB, and etcd when built with the vcpkg toolchain.

## Features

- HTTP gateway: `/healthz`, `/metrics`, `/openapi.yaml`, `/v1/search`, `/v1/router`, `/v1/documents`, `/v1/kv`
- Pure KV path: `POST|PUT /v1/kv`, `GET /v1/kv`, `DELETE /v1/kv`
- Semantic document path: document upsert/delete with embedding and vector index maintenance
- Multimodal retrieval: Chinese text queries, image queries, image-backed documents
- Storage: RocksDB backend plus WAL/snapshot fallback
- Persistence: generated Protobuf messages with framed WAL/snapshot/index records
- Search: FAISS backend plus persistent brute-force fallback
- Routing: static membership, etcd discovery, consistent hashing, owner checks
- Control plane: API key/Bearer auth, rate limiting, trace IDs, read-only mode, graceful shutdown
- Observability: spdlog/fallback logging, Prometheus metrics, health details for backends, etcd discovery state, CPU/disk, and thread pool state

## Architecture

```text
Client
  -> Gateway Runtime (BRPC preferred, fallback HTTP available)
  -> InProcessGatewayServer
      -> Pure KV path
          -> KvStore (RocksDB or WAL/snapshot)
      -> Semantic search path
          -> EmbeddingService (ONNX Runtime or deterministic fallback)
          -> VectorIndex (FAISS or brute-force persistent index)
          -> KvStore

Routing: static/etcd membership + consistent hash
Persistence: Protobuf framed records
Observability: spdlog/fallback logs + Prometheus + healthz
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

Semantic writes use a KV/vector-index compensation outbox. The document is first persisted in KV, then an index task is written to `search.vector_index_outbox_path`; a background worker generates embeddings and updates the vector index, keeping failed tasks for retry. Records carry `version`, `updated_at_unix_ms` and `mutation_id`, so stale mutations are rejected and repeated mutation IDs are idempotent. P1/P2 follow-up production work is tracked in `docs/production_backlog.md`.

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

HTTP remains JSON; Protobuf is used internally for persistence and RPC schema.

## Reindexing

Changing the embedding model or embedding dimension requires reindexing existing semantic documents:

```bash
docker compose stop rangeveckv
docker compose run --rm rangeveckv \
  --config /opt/rangeveckv/share/rangeveckv/config/server.prod.yaml \
  --collection documents --reindex
docker compose up -d
```

Pure KV records stored through `/v1/kv` intentionally bypass embedding and vector index updates. They are not returned by semantic search unless separately written or reindexed through the document path.

## Current Boundaries

- TLS/mTLS is still a configuration placeholder.
- Fallback HTTP runtime is for local validation; production should use BRPC.
- Deterministic embedding is a development fallback, not a production retrieval model.
- High semantic-search QPS depends on model choice, ONNX Runtime threading, batching/cache strategy, and hardware acceleration.

## License

RangeVecKV is released under the [MIT License](LICENSE).
