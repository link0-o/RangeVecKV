# RangeVecKV 中文说明

RangeVecKV 是一个 C++17 实现的分布式 KV + 语义检索引擎。项目同时保留两条数据路径：

- 纯 KV 路径：直接读写 RocksDB 或本地 WAL/snapshot fallback，不触发 AI embedding 和向量索引。
- 语义检索路径：文档写入时生成向量，查询时使用文本或图片 embedding 做向量检索。

当前生产化目标是用一套 C++ 服务串起 BRPC、Protobuf、RocksDB、FAISS、ONNX Runtime、etcd、spdlog、yaml-cpp、oneTBB 和 Docker Compose。

## 核心能力

- `/v1/kv`：纯 KV 增删查，绕过 AI 和向量索引。
- `/v1/documents`：语义文档写入和删除，同时维护 KV 与向量索引。
- `/v1/search`：中文文本检索、图片检索、跨模态检索。
- `/v1/router`：根据 collection/key 查看一致性哈希路由归属。
- `/healthz`、`/metrics`、`/openapi.yaml`：健康检查、Prometheus 指标、OpenAPI 文档，其中 health 会暴露 etcd discovery、CPU/磁盘、线程池等运行状态。

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

语义写入采用 KV 与向量索引补偿模式：文档先写入 KV，并把待建索引任务持久化到 `search.vector_index_outbox_path`；后台 outbox 负责生成 embedding 并更新向量索引，失败任务保留并重试。每条记录携带 `version`、`updated_at_unix_ms` 和 `mutation_id`，KV 层会拒绝旧版本覆盖，相同 `mutation_id` 的重放视为幂等成功。P1/P2 后续生产增强清单见 `docs/production_backlog.md`。

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

WAL、snapshot、brute-force vector index snapshot、FAISS metadata 都使用 framed Protobuf records。HTTP API 仍然使用 JSON。

## 性能口径

项目里要区分三类 QPS：

- RocksDB 引擎直测：绕过 HTTP 和 JSON，能反映存储引擎能力。
- `/v1/kv` HTTP QPS：包含 HTTP、JSON、路由、Protobuf 编解码、RocksDB。
- `/v1/search` 语义检索 QPS：额外包含 ONNXRuntime 模型推理和向量检索。

当前语义搜索的主要瓶颈在 Chinese-CLIP CPU 推理，不在 RocksDB。

## 许可证

本项目使用 MIT License，详见 [LICENSE](LICENSE)。
