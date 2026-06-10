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

The semantic document API `/v1/documents` still writes both KV storage and the
vector index. Do not use `/v1/kv` for semantic documents that should appear in
`/v1/search`.

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

Ring rebuilds update request routing only. Automatic data migration and
remote-owner write forwarding are not enabled yet; remote-owner writes return
`Unavailable` with the owner endpoint so clients or an external gateway can
retry against the owning node.

Fallback mode is intentionally retained so local build/test is not blocked by large production dependencies. It should not be described as the final production performance profile.

## Persistence

WAL, KV snapshot, vector fallback snapshot, and FAISS metadata snapshot are
written as generated Protobuf messages inside framed records. Older text
snapshots remain readable and are rewritten in the Protobuf format on the next
flush.

Logs are not serialized with protobuf; they use spdlog when available and the fallback logger otherwise.
