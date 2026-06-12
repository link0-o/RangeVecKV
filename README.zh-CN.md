# RangeVecKV 中文说明

RangeVecKV 是一个 C++17 实现的分布式 KV + 语义检索引擎。项目同时保留两条数据路径：

- 纯 KV 路径：直接读写 RocksDB 或本地 WAL/snapshot fallback，不触发 AI embedding 和向量索引。
- 语义检索路径：文档写入时生成向量，查询时使用文本或图片 embedding 做向量检索。
- 生产写入路径：优先使用 typed Protobuf BRPC，JSON REST 继续作为调试和兼容入口。

当前生产化目标是用一套 C++ 服务串起 BRPC、Protobuf、RocksDB、FAISS、ONNX Runtime、etcd、spdlog、yaml-cpp、oneTBB 和 Docker Compose。

## 核心能力

- `/v1/kv`、`/v1/kv/batch`：纯 KV 增删查和批量写入，绕过 AI 和向量索引。
- `KvWriteService`：生产优先的 Protobuf BRPC 写入接口，覆盖单条 KV、批量 KV 和语义文档写入；JSON REST 继续保留兼容。
- `/v1/documents`：语义文档写入和删除，同时维护 KV 与向量索引。
- `/v1/search`：中文文本检索、图片检索、跨模态检索。
- `/v1/router`：根据 collection/key 查看固定 slot 分片和 owner 归属。
- `/healthz`、`/metrics`、`/openapi.yaml`：健康检查、Prometheus 指标、OpenAPI 文档，其中 health 会暴露 etcd discovery、CPU/磁盘、线程池等运行状态。
- KV/向量一致性补偿：语义写入先落 KV，再持久化 vector index outbox，由后台任务生成 embedding 并更新向量索引，失败可保留重试。
- 分布式迁移基础能力：固定 slot 分片路由、异步 best-effort 数据迁移、迁移任务 WAL、延迟删除。
- 写入幂等与冲突保护：记录包含 `version`、`updated_at_unix_ms`、`mutation_id`，旧版本不会覆盖新版本，重复 mutation 可视为成功。

## 项目架构

```text
                                +--------------------------------+
                                | 客户端 / SDK / 压测工具         |
                                +---------------+----------------+
                                                |
                                                v
                                +--------------------------------+
                                | 网关运行时                     |
                                | BRPC Protobuf / BRPC HTTP      |
                                | fallback HTTP                  |
                                +---------------+----------------+
                                                |
                                                v
                                +--------------------------------+
                                | InProcessGatewayServer         |
                                | 鉴权 / Trace / 指标 / 文档      |
                                +---------------+----------------+
                                                |
                    +---------------------------+---------------------------+
                    |                           |                           |
                    v                           v                           v
        +-----------------------+   +-----------------------+   +-----------------------+
        | 集群控制面            |   | 纯 KV 接口            |   | 语义检索接口          |
        | etcd / 静态节点       |   | /v1/kv                |   | /v1/documents         |
        | slot 分片 + owner ring|   | /v1/kv/batch          |   | /v1/search            |
        +-----------+-----------+   +-----------+-----------+   +-----------+-----------+
                    |                           |                           |
        远端 owner  |                           |                           |
                    v                           v                           v
        +-----------------------+   +-----------------------+   +-----------------------+
        | 返回 Unavailable      |   | KV 存储               |   | 向量索引 Outbox       |
        | 携带目标 endpoint     |   | RocksDB / WAL fallback|   | 持久化重试队列        |
        +-----------------------+   +-----------+-----------+   +-----------+-----------+
                                                |                           |
                                                |                           v
                                                |               +-----------------------+
                                                |               | Embedding 服务        |
                                                |               | ONNX / deterministic  |
                                                |               +-----------+-----------+
                                                |                           |
                                                |                           v
                                                |               +-----------------------+
                                                |               | 向量索引              |
                                                |               | FAISS / brute force   |
                                                |               +-----------+-----------+
                                                |                           |
                                                v                           v
                                +-----------------------------------------------+
                                | 持久化                                        |
                                | Protobuf WAL / snapshot / RocksDB value codec |
                                +-----------------------------------------------+

        ring 变化
             |
             v
        +-----------------------+       POST /internal/migration/records
        | 数据迁移              |------------------------------------+
        | best-effort 重试      |                                    |
        | 延迟删除              |--------------------------------+   |
        +-----------------------+                                |   |
                                                                 v   v
                                                        +-----------------------+
                                                        | KV 存储 + Outbox      |
                                                        | 迁移写入路径          |
                                                        +-----------------------+

                                +--------------------------------+
                                | 运维观测                       |
                                | healthz / Prometheus / spdlog  |
                                | CPU / 磁盘 / 线程池状态        |
                                +--------------------------------+
```

## 请求链路

纯 KV 单条写入 `POST /v1/kv`：

```text
Client
  -> Gateway Runtime
  -> API key / Bearer 鉴权 + Trace ID
  -> InProcessGatewayServer::PutKvRecord
  -> 固定 slot 分片 + owner ring 判断
  -> 本节点 owner: KV Store Put
  -> RocksDB value / WAL fallback 写入 Protobuf record
  -> 返回 200 OK
```

生产推荐的纯 KV 单条写入 `KvWriteService.PutKv`：

```text
Client / SDK / 压测工具
  -> BRPC Protobuf KvWriteService.PutKv
  -> RequestContext 鉴权 + Trace ID
  -> InProcessGatewayServer::PutKvRecord
  -> 固定 slot 分片 + owner ring 判断
  -> 本节点 owner: KV Store Put
  -> RocksDB Get-before-Put 校验 + WAL 写入
  -> 返回 KvWriteResponse
```

纯 KV 批量写入 `POST /v1/kv/batch`：

```text
Client
  -> Gateway Runtime
  -> 解析 records[]
  -> 校验 batch 大小、空 key、重复 collection:key
  -> 对每条记录做 owner 判断
  -> KvStore BatchPut
  -> RocksDB WriteBatch / WAL fallback 批量校验后写入
  -> 返回 record_count
```

纯 KV 读取 `GET /v1/kv`：

```text
Client
  -> Gateway Runtime
  -> 鉴权 + Trace ID
  -> key 查询: 先做 slot/owner 判断，再 KV Store Get
  -> 无 key 范围查询: 只扫描本节点本地 collection，KV Store Range
  -> 返回单条 record 或 items[]
```

语义文档写入 `POST /v1/documents`：

```text
Client
  -> Gateway Runtime
  -> 鉴权 + Trace ID
  -> owner 判断
  -> KV Store Put 先落主数据
  -> Vector Index Outbox 写入待索引任务
  -> 请求路径触发一次 DrainOnce，后台 worker 继续兜底
  -> 生成 embedding
  -> Vector Index Upsert
  -> 失败任务保留并重试
```

语义检索 `GET|POST /v1/search`：

```text
Client
  -> Gateway Runtime
  -> search 限流 + Trace ID
  -> Embedding Service 生成 query/image embedding
  -> Vector Index TopK 检索
  -> KV Store MultiGet 回填 title/body/metadata
  -> timeout 或后端不可用时走降级检索
  -> 返回 hits[]
```

远端 owner 和迁移链路：

```text
普通客户端写入
  -> 路由发现 owner 是远端节点
  -> 返回 Unavailable + 目标 endpoint
  -> 当前不做普通写请求自动转发

ring 变化 / 节点变化
  -> DataMigrationManager 扫描本地 KV
  -> 找到新 owner 已非本节点的记录
  -> POST /internal/migration/records 到目标节点
  -> 目标节点本地写入 KV / Outbox
  -> 源节点延迟删除，删除前重新确认 owner
```

健康检查与观测：

```text
/healthz
  -> 汇总 storage/search/etcd/thread-pool/migration/outbox/CPU/disk 状态

/metrics
  -> Prometheus 文本指标

structured logs
  -> spdlog 文件日志 / fallback logger
```

## 本地快速验证

默认本地构建使用 fallback 后端，不要求先安装 ONNX Runtime、FAISS、RocksDB、etcd 等完整生产依赖。因为内部持久化已经切到生成的 Protobuf，所以本地仍需要基础构建工具、Protobuf 和 nlohmann-json。

Debian/Ubuntu 可先安装：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build protobuf-compiler libprotobuf-dev nlohmann-json3-dev
```

也可以直接使用项目脚本安装本地工具链：

```bash
./scripts/bootstrap_dev_env.sh
export PATH="$HOME/.local/bin:$PATH"
```

然后构建、启动：

```bash
./scripts/build_local.sh
./scripts/run_local_service.sh
```

另开终端执行：

```bash
./scripts/http_smoke.sh http://127.0.0.1:8080
```

如果要显式尝试完整 vcpkg 后端：

```bash
export KVAI_USE_VCPKG_TOOLCHAIN=1
./scripts/build_local.sh
```

## 生产 Compose

先下载固定版本的 Chinese-CLIP ONNX 模型：

```bash
./scripts/download_ai_model.sh
docker compose up --build -d
```

生产配置使用 `.env` 注入 API key、模型路径、tokenizer 路径、etcd 地址等变量。参考 `.env.example`。

Docker 构建支持两种模式：

- `KVAI_DOCKER_OPTIONAL_BACKENDS=ON`：生产模式，启用 vcpkg 依赖，优先使用 BRPC、ONNX Runtime、FAISS、RocksDB、etcd-cpp-api、spdlog、yaml-cpp、oneTBB 等真实后端。
- `KVAI_DOCKER_OPTIONAL_BACKENDS=OFF`：轻量 fallback 模式，用于 CI smoke 或本地快速验证，不要求完整生产依赖；AI 检索通常应切到 deterministic fallback。

CI/CD 中的默认 Docker smoke 使用 fallback 模式保证快速稳定；手动触发的 production Docker job 会构建完整 optional backend 镜像，并验证 HTTP smoke 与 Protobuf BRPC 写入 smoke。

图片检索时，把图片放到宿主机 `images/` 目录，容器内路径使用：

```text
/opt/rangeveckv/images/example.jpg
```

## etcd 服务发现

当 `cluster.discovery_backend=etcd` 时，每个节点会在 `cluster.etcd_prefix` 下注册一个带 lease 的 key，例如 `/rangeveckv/nodes/node-a`。新版本写入 JSON value：

```json
{"version":1,"id":"node-a","host":"rangeveckv-a","port":8080,"healthy":true,"weight":100,"zone":"default"}
```

旧的 `host:port` value 仍然兼容可读。prefix watch 使用明确的 range/prefix 语义，监听配置前缀下的所有节点 key；watch 断开后会重新拉取全量节点并带退避重连。keepalive 和 watch 状态会通过 `/healthz` 的 `etcd_discovery_*` 字段暴露。

可通过 `cluster.data_migration_enabled=true` 开启异步数据迁移。开启后，ring 变化会触发本地 KV 扫描：如果某条记录的新 owner 已经不是本节点，就通过内部 `/internal/migration/records` 接口发送到目标 owner；目标写入成功后，源节点会保留到 `cluster.migration_delete_delay_ms` 到期再清理。迁移任务会写入 `cluster.migration_task_wal_path`，重启后可恢复 pending/delete_pending 状态并继续扫描兜底。路由先计算固定 slot，再把 slot 映射到 owner，`/v1/router` 会返回 `slot_id`。这是最终一致的 best-effort 迁移，不是 Raft/quorum 强一致复制。普通客户端写入仍不会自动远程转发，owner 是远端节点时仍返回 `Unavailable` 并带目标 endpoint。

语义写入采用 KV 与向量索引补偿模式：文档先写入 KV，并把待建索引任务持久化到 `search.vector_index_outbox_path`；请求路径会触发一次 outbox drain 来推进索引，后台 worker 也会继续处理 pending 任务，失败任务保留并重试。每条记录携带 `version`、`updated_at_unix_ms` 和 `mutation_id`，KV 层会拒绝旧版本覆盖，相同 `mutation_id` 的重放视为幂等成功。

## API 示例

纯 KV 写入：

```bash
curl -X POST http://127.0.0.1:8080/v1/kv \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"collection":"kv","key":"user:1","value":"plain kv value"}'
```

纯 KV 查询：

```bash
curl "http://127.0.0.1:8080/v1/kv?collection=kv&key=user:1" \
  -H "x-api-key: <api-key>"
```

纯 KV 批量写入：

```bash
curl -X POST http://127.0.0.1:8080/v1/kv/batch \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"records":[{"collection":"kv","key":"user:2","value":"batch value 1"},{"collection":"kv","key":"user:3","value":"batch value 2"}]}'
```

`/v1/kv/batch` 仍然只写 KV，不触发 embedding 和向量索引；它保留 `version`、`mutation_id` 幂等/冲突保护，单个请求内重复 key 会被拒绝，最大条数由 `gateway.kv_batch_max_records` 控制。

生产客户端建议默认使用 Protobuf BRPC 写入接口，JSON REST 主要用于 curl 调试、兼容旧客户端和轻量验证。Protobuf 写入接口不绕过 owner 路由、version/CAS、`mutation_id` 幂等或 RocksDB WAL。

生产 BRPC 写入接口定义在 `proto/search.proto`：

- `KvWriteService.PutKv`
- `KvWriteService.BatchPutKv`
- `KvWriteService.UpsertDocument`

请求通过 `RequestContext` 携带 `trace_id`、`api_key` 或 `authorization`。服务端复用同一套鉴权、slot/owner 路由、版本校验、幂等写入和 WAL 持久化逻辑；它只是绕过 HTTP path dispatch、JSON 解析和 JSON 响应序列化。

语义文档写入：

```bash
curl -X POST http://127.0.0.1:8080/v1/documents \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"collection":"documents","key":"doc-1","title":"中文向量检索","body":"RangeVecKV 支持中文语义搜索。"}'
```

中文语义搜索：

```bash
curl "http://127.0.0.1:8080/v1/search?q=中文语义搜索&top_k=5" \
  -H "x-api-key: <api-key>"
```

图片搜索：

```bash
curl -X POST http://127.0.0.1:8080/v1/search \
  -H "x-api-key: <api-key>" \
  -H "Content-Type: application/json" \
  -d '{"image_path":"/opt/rangeveckv/images/example.jpg","top_k":5}'
```

## 持久化格式

内部持久化使用生成的 Protobuf message：

- `DocumentRecord`
- `WalEntry`
- `VectorEntry`

WAL、snapshot、brute-force vector index snapshot、FAISS metadata 都使用 framed Protobuf records。JSON REST API 继续保留；生产写入路径推荐使用 typed Protobuf BRPC，减少 HTTP/JSON 解析、响应 dump 和 attachment 拷贝开销。

RocksDB 生产配置保留 WAL 开启，并补充了安全的写入/compaction 参数：background jobs、write buffer、target file size、bytes-per-sync、WAL bytes-per-sync 和 pipelined write。项目没有默认关闭 WAL，也没有跳过 Get-before-Put 的 version/CAS/`mutation_id` 校验。

## 性能与后续优化

README 只保留复现入口和结论边界：JSON HTTP 性能数据可通过 `scripts/http_benchmark.py` 生成；生产 Protobuf BRPC 写入性能可通过构建产物 `kvai_brpc_kv_benchmark` 生成，产物默认写入 `perf_results/` 且不会提交。具体数字受电源模式、Docker 网络、数据目录状态、后端组合、并发和 batch size 影响，建议每次压测都重新记录环境。

示例：

```bash
./build/production/src/gateway/kvai_brpc_kv_benchmark \
  --server 127.0.0.1:8080 \
  --api-key "$KVAI_API_KEY" \
  --mode kv \
  --threads 16 \
  --duration-seconds 20 \
  --output perf_results/brpc_kv.json
```

批量 Protobuf KV 写入可使用：

```bash
./build/production/src/gateway/kvai_brpc_kv_benchmark \
  --server 127.0.0.1:8080 \
  --api-key "$KVAI_API_KEY" \
  --mode kv_batch \
  --batch-size 100 \
  --threads 16 \
  --duration-seconds 20 \
  --output perf_results/brpc_kv_batch.json
```

## 许可证

本项目使用 MIT License，详见 [LICENSE](LICENSE)。
