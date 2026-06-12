# RangeVecKV 编译与测试说明

本文档对应当前仓库的默认本地运行方式。

当前默认本地验证路径使用 `auto` 后端选择；在没有完整生产依赖或模型文件时会自动落到 fallback 后端，不要求先接通 ONNX Runtime、FAISS、RocksDB。

注意：内部持久化已经使用生成的 Protobuf message，所以即使是 fallback 构建，也需要安装 Protobuf 编译器和开发库。

典型 fallback 结果如下：

- `ai.backend=auto` -> deterministic tokenizer embedding
- `search.backend=auto` -> brute_force
- `storage.backend=auto` -> wal
- `cluster.discovery_backend=static`

## 1. 前置条件

建议先准备以下工具：

- `cmake >= 3.24`
- `ninja`
- `g++`
- `ctest`
- `protoc`
- `libprotobuf-dev`
- `nlohmann-json3-dev`

Debian/Ubuntu 可执行：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build protobuf-compiler libprotobuf-dev nlohmann-json3-dev
```

如果本机环境还没准备好，先执行：

```bash
./scripts/bootstrap_dev_env.sh
export PATH="$HOME/.local/bin:$PATH"
```

## 2. 默认本地构建与测试

默认本地构建已经改成 fallback-only 模式，不会自动触发完整 vcpkg manifest 安装。

直接执行：

```bash
./scripts/build_local.sh
```

这条命令会完成三件事：

1. CMake configure 到 `build/local`
2. Ninja 构建
3. `ctest --output-on-failure`

构建成功后，当前测试集应通过以下测试：

- `kvai_kv_store_test`
- `kvai_gateway_pipeline_test`
- `kvai_http_gateway_smoke_test`
- `kvai_cluster_routing_test`
- `kvai_config_loader_test`
- `kvai_persistence_codec_test`
- `kvai_auth_test`
- `kvai_thread_pool_test`

在启用 BRPC/生产依赖的构建中，还会额外覆盖：

- `kvai_brpc_kv_write_service_test`

如果你只想单独重跑测试：

```bash
ctest --test-dir build/local --output-on-failure
```

如果只跑单个测试：

```bash
ctest --test-dir build/local -R kvai_kv_store_test --output-on-failure
ctest --test-dir build/local -R kvai_gateway_pipeline_test --output-on-failure
```

## 3. 启动本地服务

构建完成后启动服务：

```bash
./scripts/run_local_service.sh
```

等价命令：

```bash
./build/local/src/gateway/kvai_server --config ./config/server.yaml --serve
```

正常启动后会监听：

```text
http://127.0.0.1:8080
```

## 4. HTTP 冒烟测试

另开一个终端执行：

```bash
./scripts/http_smoke.sh http://127.0.0.1:8080
```

脚本会检查这些接口：

- `/healthz`
- `/v1/router`
- `/v1/search`
- `/v1/kv`
- `/metrics`

如果只看健康检查：

```bash
./scripts/healthcheck.sh http://127.0.0.1:8080
```

## 5. 打包验证

如果要验证安装树和 TGZ 打包：

```bash
./scripts/package_local.sh
```

注意这一步比默认本地构建更重，因为它更接近完整打包流程。

## 6. Docker 验证

如果要走容器路径：

```bash
docker build -t rangeveckv:local .
docker run --rm -p 8080:8080 rangeveckv:local
```

然后执行：

```bash
./scripts/http_smoke.sh http://127.0.0.1:8080
```

## 7. 真实后端构建说明

如果你确实要尝试把真实后端一起编进来，再显式开启：

```bash
export KVAI_USE_VCPKG_TOOLCHAIN=1
./scripts/build_local.sh
```

这条路径会尝试接入完整 vcpkg toolchain，并可能触发 ONNX Runtime、FAISS、RocksDB 等大依赖构建。

这不是默认本地验证路径，也不建议作为第一次编译测试方式。

生产构建通过后，可以单独验证 typed Protobuf BRPC 写入路径：

```bash
./build/production/src/gateway/kvai_brpc_kv_benchmark \
  --server 127.0.0.1:8080 \
  --api-key "$KVAI_API_KEY" \
  --mode kv \
  --threads 2 \
  --duration-seconds 2 \
  --warmup-seconds 0 \
  --output perf_results/brpc_kv_smoke.json
```

这条命令只做轻量 smoke；正式性能测试请固定生产 Docker/native 模式、电源模式、数据目录状态、并发、batch size 和后端组合。

## 8. 常见问题

### 8.1 `run_local_service.sh` 报找不到 `kvai_server`

说明构建还没成功完成。

先重新执行：

```bash
./scripts/build_local.sh
```

### 8.2 Git 左侧出现大量 10k+ 文件

通常是中间产物进入了 Git 视图，主要包括：

- `build/`
- `dist/`
- `_CPack_Packages/`
- `vcpkg_installed/`
- `data/`
- `*.tar.gz`

仓库现在已经补了 `.gitignore`，重新刷新 Git 视图即可。

### 8.3 默认本地构建为什么不直接编真实后端

因为当前仓库保留本地可运行 fallback 基线。真实后端适配入口已经存在，但不应阻塞默认本地编译、测试和启动。
