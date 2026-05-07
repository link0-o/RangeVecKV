# 📘 分布式 KV + AI 语义检索引擎 V1.0

## 🎯 项目定位
面向生产环境的分布式键值存储引擎，内置自然语言/图像语义检索能力。  
**核心目标**：在保障 KV 高吞吐、低延迟、强一致的前提下，将 AI 推理作为标准化黑盒组件无缝集成，提供可水平扩展、全链路可观测、故障可降级的一体化数据服务。  
**技术主线**：`C++17`（核心逻辑） + `汇编`（仅限热点路径 SIMD/零拷贝） + `BRPC/gRPC`（内部通信） + `ONNX Runtime`（AI 推理） + `FAISS`（向量检索） + `RocksDB`（持久化）。

## 🏗️ 架构与数据流
```
# RangeVecKV

## 项目状态

RangeVecKV 当前版本不是“功能演示”，而是一个已经完成工程化交付闭环的单机产品基线：

- 运行面已经具备常驻 HTTP 服务、结构化日志、Prometheus 指标、`/healthz`、OpenAPI、API Key 鉴权、限流、优雅停机、安装与打包、Docker 交付、CI、Compose 栈编排。
- 数据面已经具备文档写入、删除、查询、静态集群路由、一致性哈希 owner 判定、只读模式、降级检索和本地持久化恢复。
- 配置面已经具备 YAML 配置、环境变量展开、开发配置与生产配置模板分离。

当前默认运行后端仍然是工程化 fallback 实现，而不是完整接通的生产三方库：

- `ai.backend=deterministic`
- `search.backend=brute_force`
- `storage.backend=wal`
- `cluster.discovery_backend=static`

这不是文档遗漏，而是当前仓库的真实交付边界。README 以下内容已经按这个现实状态重写，避免把“目标架构”误写成“已完成实现”。

补充说明：代码层已经补入 `onnxruntime`、`faiss`、`rocksdb` 的真实后端工厂与适配入口；当对应依赖和模型文件可用时可切换启用，否则保持 fallback 运行。

## 目标定位

项目目标仍然是面向生产环境的分布式 KV + AI 语义检索引擎：

- 核心逻辑：C++17
- 依赖管理：vcpkg manifest
- 交付链路：CMake、Docker、GitHub Actions、CPack
- 目标后端：BRPC/gRPC、ONNX Runtime、FAISS、RocksDB、etcd、MinIO/S3

本仓库当前承担的角色是：

1. 先把服务骨架、工程化能力、交付能力和运行控制面做完整。
2. 再把真实三方后端逐个接入，不推翻已有接口与部署面。

## 当前已实现能力

### 服务接口

- `GET /healthz`：健康检查与服务状态
- `GET /metrics`：Prometheus 文本指标
- `GET /openapi.yaml`：OpenAPI 文档
- `GET/POST /v1/search`：语义检索
- `GET /v1/router`：一致性哈希路由结果
- `POST /v1/documents`：写入文档
- `DELETE /v1/documents`：删除文档

### 工程能力

- 非 root 运行
- YAML 配置解析与环境变量展开
- API Key 鉴权
- 固定窗口限流
- trace_id 注入
- Prometheus 指标导出
- 优雅停机
- 单元测试与集成测试
- Docker 镜像构建
- CPack 安装包生成
- GitHub Actions 构建、测试、打包、容器冒烟

### 当前运行后端

| 能力域 | 当前默认实现 | 说明 |
|---|---|---|
| AI 推理 | Deterministic Embedding | 已接入统一接口与超时/降级路径，代码已补入 ONNX Runtime 适配入口 |
| 向量检索 | Brute-force Persistent Index | 已支持持久化恢复与过滤，代码已补入 FAISS 适配入口 |
| KV 存储 | WAL + Snapshot Store | 已支持 Put/Get/Delete/Range/Recover，代码已补入 RocksDB 适配入口 |
| 服务发现 | Static Cluster Membership | 已支持一致性哈希与 owner 判定，未接 etcd |

## 目标架构与当前实现的关系

### 目标生产架构

```text
Client -> Gateway -> AI Worker -> Vector Worker -> KV Worker -> Response
                        |             |               |
                        |             |               +-> RocksDB
                        |             +-> FAISS
                        +-> ONNX Runtime

Gateway/Workers <-> etcd
Object Storage <-> MinIO / S3
```

### 当前仓库实现

```text
Client -> HTTP Gateway Runtime -> InProcessGatewayServer -> EmbeddingService
                                                      -> VectorIndex
                                                      -> KvStore

Routing: static nodes + consistent hash
Delivery: CMake + CPack + Docker + CI + Compose
Observability: logs + metrics + healthz + OpenAPI
```

## 目录结构

```text
.
├── config/                 # 开发/生产配置模板
├── docs/                   # OpenAPI 与运维文档
├── proto/                  # Proto 接口定义
├── scripts/                # 构建、打包、启动、健康检查、栈管理脚本
├── src/
│   ├── ai/                 # Embedding 接口与 fallback 实现
│   ├── core/               # KV 抽象与 WAL/snapshot 实现
│   ├── gateway/            # HTTP 运行时、鉴权、路由、服务编排
│   ├── infra/              # 配置、日志、指标、路由基础设施
│   └── search/             # 向量索引抽象与 fallback 实现
├── test/                   # 回归测试与冒烟测试
├── Dockerfile
├── docker-compose.yml
├── vcpkg.json
└── CMakeLists.txt
```

## 配置文件

### 开发配置

- `config/server.yaml`
- `config/model.yaml`

特点：

- API Key 默认关闭
- demo seed 默认开启
- 适合本地开发和快速验证

### 生产样例配置

- `config/server.prod.yaml`
- `config/model.prod.yaml`

特点：

- API Key 默认开启
- demo seed 默认关闭
- 支持 `${ENV_VAR}` 环境变量展开
- 适合 Docker / Compose / CI 场景

关键环境变量示例见 `.env.example`。

## 本地构建与验证

### 1. 构建与测试

```bash
./scripts/build_local.sh
```

### 2. 启动服务

```bash
./scripts/run_local_service.sh
```

### 3. HTTP 冒烟

```bash
./scripts/http_smoke.sh http://127.0.0.1:8080
```

### 4. 健康检查

```bash
./scripts/healthcheck.sh http://127.0.0.1:8080
```

### 5. 打包

```bash
./scripts/package_local.sh
```

更完整的本地编译、测试、启动和冒烟说明见 [docs/testing.md](docs/testing.md)。

## Docker 与 Compose

### 单镜像构建

```bash
docker build -t rangeveckv:local .
docker run --rm -p 8080:8080 rangeveckv:local
```

### 本地栈编排

```bash
cp .env.example .env
./scripts/stack_local.sh up
```

Compose 栈当前包含：

- `rangeveckv`
- `etcd`
- `minio`

说明：

- 运行态已预留 etcd 与对象存储编排位置。
- 当前应用运行时仍使用 `static` 路由后端，不会直接消费 etcd。
- MinIO 当前主要用于补齐部署拓扑，不参与本地 fallback 数据面。

## CI/CD

当前 CI 工作流已经覆盖：

1. 系统依赖安装
2. cmake/ninja 自举
3. vcpkg bootstrap
4. configure/build/test
5. install/package
6. docker build
7. 容器启动与 HTTP 冒烟
8. 产物上传

工作流定义见 `.github/workflows/ci.yml`。

## 已验证结果

当前仓库已经通过以下本地验证路径：

- 本地构建成功
- 回归测试通过
- HTTP 服务启动成功
- `/healthz`、`/metrics`、`/v1/search`、`/v1/router`、`/v1/documents` 路径可用
- 安装树生成成功
- TGZ 包生成成功

## 仍待真实后端接入的部分

下面这些是“产品化后续接入”，不是“仓库没工程化”：

1. ONNX Runtime 真实推理适配器
2. FAISS 真实 ANN 索引适配器
3. RocksDB 真实持久化适配器
4. etcd 动态服务发现与配置热更新
5. BRPC/gRPC 正式网关实现
6. TLS 双向认证真正接线

这些能力在当前仓库中已经有：

- 配置位
- 部署位
- 抽象接口位
- 健康输出位
- 文档与脚本位

也就是说，后续接入是“替换后端实现”，不是“重做工程骨架”。

## 设计红线

1. 不在 KV 中存大文件，图片/视频走 MinIO/S3。
2. AI 推理必须保留超时与降级路径。
3. 向量索引必须可恢复。
4. 依赖必须通过 vcpkg manifest 锁定。
5. 汇编只能用于热点路径，不允许替换 STL 或通用内存分配器。

## 运维文档

补充运维说明见 `docs/operations.md`。
┌─────────────────────────────────────────────────────────────┐
│  接入层  │  Nginx/Envoy (TLS/限流/路由) │  OpenAPI 文档       │
├──────────────────────────────────────────┼──────────────────┤
│  网关层  │  BRPC/gRPC Gateway            │  Auth + Retry      │
├──────────────────────────────────────────┼──────────────────┤
│  计算层  │  AI Inference Worker (ONNX)   │  Vector Worker (FAISS)│
│          │  KV Storage Worker (RocksDB)  │  Metadata Router     │
├──────────┼────────────────────────────────┼──────────────────┤
│  协调层  │  etcd (路由表/配置/服务发现)  │  MinIO/S3 (原始文件)│
├──────────┼────────────────────────────────┼──────────────────┤
│  工程层  │  vcpkg manifest + CMake       │  Prometheus + OTel  │
│          │  Docker + GitHub Actions      │  ASan/TSan + GTest  │
└─────────────────────────────────────────────────────────────┘

请求链路：
Client → [Gateway] → trace_id 注入 → [AI Worker] 文本/图 → 向量
                                      ↓
                              [Vector Worker] ANN Top-K IDs
                                      ↓
                              [KV Worker] RocksDB GetBatch
                                      ↓
                              [Gateway] → Client JSON
```

## 📦 技术栈与依赖清单
| 模块 | 组件 | 版本策略 | 作用 | 降级/备选 |
|------|------|----------|------|-----------|
| **语言/构建** | `C++17`, `CMake 3.24+` | 根 `CMakeLists.txt` | 核心逻辑、编译链路 | - |
| **依赖管理** | `vcpkg (manifest)` | `vcpkg.json` 提交仓库 | 全量依赖哈希锁定，禁止 `apt`/源码直编 | `conan` |
| **RPC/网关** | `BRPC`（主） / `gRPC`（备） | vcpkg 包 | 节点通信、HTTP/gRPC 网关、内置 bvar 监控 | `Drogon` |
| **AI 推理** | `ONNX Runtime C++` | vcpkg: `onnxruntime` | 加载 CLIP/ViT 模型，脱离 Python，CPU/GPU 自动切换 | `TensorRT`（V2） |
| **向量检索** | `FAISS C++` | vcpkg: `faiss` | IVF/HNSW 索引，百万级毫秒级 Top-K | `hnswlib` |
| **KV 存储** | `RocksDB` | vcpkg: `rocksdb` | LSM-Tree 持久化，列族隔离，WAL 崩溃恢复 | - |
| **协调/配置** | `etcd v3.5+` | vcpkg: `etcd-cpp-apiv3` | 服务发现、路由表、配置中心、Raft 一致性 | - |
| **工程基础** | `spdlog`, `yaml-cpp`, `nlohmann/json`, `GoogleTest` | vcpkg | 异步日志、配置解析、JSON 交互、单元测试 | - |

## 📝 需求规格
### 功能需求（V1 必达）
| 模块 | 需求项 | 验收标准 |
|------|--------|----------|
| **KV 存储** | `Put/Get/Delete/Range`；列族隔离；批量写入；崩溃恢复 | P99 写 < 5ms，P99 读 < 3ms，支持 100GB+ |
| **AI 推理** | 文本/图 → 512/768 维向量；模型热加载；CPU/GPU 切换 | 单次推理 < 30ms，支持 Batch，内存无泄漏 |
| **向量检索** | ANN Top-K；动态增删；索引持久化；重启恢复 | 百万级检索 < 10ms，召回率 > 90% |
| **分布式路由** | 一致性哈希分片；节点自动发现；故障剔除 | 扩缩容无需停机，宕机自动切换 |
| **语义查询** | `/v1/search` 接口；自然语言输入 → 向量化 → 检索 → 返回 | 端到端 P99 < 50ms，支持过滤条件 |

### 非功能需求（工业级底线）
- ✅ **可用性**：99.9% SLA，优雅停机（SIGTERM 捕获/刷盘/释放索引/等请求）
- ✅ **可观测性**：结构化日志、Prometheus 指标、trace_id 透传、`/healthz` 端点
- ✅ **安全性**：TLS 双向认证、API Key 鉴权、速率限制、配置加密
- ✅ **容错降级**：AI 超时 → 降级关键词匹配；索引未就绪 → 返回缓存/503；KV 只读模式

### 明确边界
- ❌ **不实现模型训练/调参/损失函数**：AI 视为黑盒推理服务
- ❌ **不重写 Raft/共识算法**：直接使用 `etcd`
- ❌ **KV 不存大文件**：图片/视频走 `MinIO/S3`，KV 仅存路径与元数据
- ✅ **汇编使用范围**：仅限 SIMD 向量计算、零拷贝序列化、无锁队列热点路径。禁止替换 STL 或手写通用分配器。

## 🤖 AI Agent 协同开发协议
### 角色定位
- **人类工程师**：架构设计、模块边界、并发模型、内存生命周期、汇编热点、异常恢复、CI/CD、最终 Code Review。
- **AI Agent**：接口封装、调用模板、CMake 配置、单元测试骨架、日志/指标接入、配置解析、Proto 代码生成。

### 强制约束
1. 语言/构建：`C++17`，`CMake`，依赖通过 `vcpkg.json` 管理。
2. 禁止隐式内存分配：所有动态对象使用智能指针或对象池，禁止裸 `new/delete`。
3. 禁止阻塞调用：AI 推理、网络 IO、磁盘读写必须走线程池或异步队列。
4. 错误路径完整：网络断开/模型未加载/存储损坏必须有 `Status` 返回与降级策略。
5. 不擅自引入依赖：未列在 `vcpkg.json` 的库一律拒绝。

### 标准 Prompt 模板（直接复制给 AI）
```
你是一名 C++ 系统工程师。请严格按以下约束生成代码：
1. 语言/构建：C++17，CMake 3.24+，依赖通过 vcpkg manifest 管理
2. 目标：实现 {模块名}
3. 要求：
   - 使用 spdlog 异步日志，日志级别可配置
   - 错误统一返回 std::expected 或自定义 Status 结构体
   - 禁止硬编码路径/端口，全部从 yaml-cpp 解析
   - 内存分配明确，禁止裸 new/delete，使用智能指针或对象池
   - 生成对应 GTest 用例，覆盖正常/异常/边界路径
4. 输出：仅包含 .h / .cpp / CMakeLists.txt / 编译命令，不输出解释性文本
```

### 代码审查清单（合并前必过）
- [ ] 无隐式内存分配/泄漏（`valgrind`/`ASan` 通过）
- [ ] 无阻塞调用阻塞主事件循环
- [ ] 错误路径完整（网络断开/模型未加载/存储损坏均有 fallback）
- [ ] 无未定义行为（数组越界/野指针/数据竞争 TSan 通过）
- [ ] 依赖未越界（不擅自引入未声明组件）

## 📂 项目目录结构
```
kvai-engine/
├── proto/                 # gRPC/BRPC 接口定义
├── src/
│   ├── core/              # KV 核心（RocksDB 封装、列族管理、WAL）
│   ├── ai/                # AI 推理（ONNX 会话管理、Batch 队列、超时降级）
│   ├── search/            # 向量检索（FAISS 索引封装、持久化、内存/磁盘切换）
│   ├── gateway/           # 网关层（BRPC/gRPC 服务实现、鉴权、限流、trace_id）
│   └── infra/             # 基础设施（spdlog 接入、yaml 配置、Prometheus 指标、etcd 客户端）
├── test/                  # GTest 单元测试 + 集成测试
├── config/                # yaml 配置模板（server.yaml, model.yaml）
├── scripts/               # 构建/压测/CI/健康检查脚本
├── vcpkg.json             # 依赖清单（必须提交）
└── CMakeLists.txt         # 根构建配置
```

## 🛠️ 构建与运行（vcpkg manifest 模式）
```bash
# 1. 初始化 vcpkg
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg

# 2. 配置构建（自动拉取 vcpkg.json 依赖）
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)

# 3. 运行测试
cd build && ctest --output-on-failure

# 4. 启动服务
./build/src/kvai_server --config ../config/server.yaml --model ../models/clip.onnx
```

## 📅 研发里程碑与 CI/CD
| 阶段 | 周期 | 交付物 | 验收指标 |
|------|------|--------|----------|
| **Phase 1** | W1-2 | 单机闭环（ONNX+FAISS+RocksDB+BRPC） | `/search` 可用，P99 < 50ms |
| **Phase 2** | W3-4 | etcd 路由 + 一致性哈希分片 + 配置热更新 | 节点扩缩容自动感知 |
| **Phase 3** | W5-6 | Prometheus 指标 + trace_id + 优雅停机 | 监控就绪，ASan/TSan 通过 |
| **Phase 4** | W7+ | Docker 交付 + GitHub Actions CI + OpenAPI | 一键拉起，压测基线达标 |

**CI/CD 流水线**：`push → clang-format/lint → cmake build → ASan/TSan test → docker build → push registry`

## ⚠️ 生产级红线（不可妥协）
1. **禁止在 KV 中存大文件**：图片/视频走 MinIO/S3，KV 仅存路径与元数据。
2. **向量索引必须可恢复**：FAISS 默认内存索引，需定期 `serialize` 到磁盘，启动异步加载。
3. **AI 推理必须带超时与降级**：超时 → 返回缓存或降级为元数据关键词匹配，不可阻塞网关。
4. **依赖必须锁定**：所有第三方库通过 `vcpkg.json` 锁定哈希，禁止动态拉取或系统包混用。
5. **汇编仅限热点路径**：仅允许用于 SIMD 向量计算/零拷贝序列化/无锁队列，禁止替换 STL 或重写内存分配器。
