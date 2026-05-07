# RangeVecKV Operations Guide

## Runtime Modes

- `config/server.yaml`: local development defaults, API key disabled, demo seed enabled
- `config/server.prod.yaml`: production-like defaults, API key enabled, demo seed disabled, environment variable expansion enabled

## Environment Variables

- `KVAI_API_KEY`: API key used when `gateway.require_api_key=true`
- `KVAI_CLUSTER_NODES`: static cluster membership string, format `node-a@10.0.0.1:8080,node-b@10.0.0.2:8080`
- `KVAI_TLS_MODE`: TLS mode flag exposed in health output; current code supports `disabled` placeholder
- `KVAI_MODEL_PATH`: model artifact path reserved for ONNX adapter integration

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
- MinIO API: `http://127.0.0.1:9000`
- MinIO Console: `http://127.0.0.1:9001`

## Current Production Gaps

- `ai.backend=deterministic` is still the default runtime backend; ONNX adapter integration remains to be wired
- `search.backend=brute_force` is still the default runtime backend; FAISS adapter integration remains to be wired
- `storage.backend=wal` is still the default runtime backend; RocksDB adapter integration remains to be wired
- `cluster.discovery_backend=static` is currently implemented; etcd discovery is reserved in manifest and deployment topology but not yet bound to runtime code