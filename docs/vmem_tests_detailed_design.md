# vmem_tests 详细设计

## 1. 目标与范围

`vmem_tests` 用于验证基于 dma-buf 与 Level Zero IPC 的 GPU 显存跨进程、跨设备访问路径，重点覆盖以下能力：

- 从 Level Zero IPC 句柄提取可用于对端访问的 DMA 地址列表。
- 基于地址列表在内核侧导出新的 dma-buf FD，并回传用户态重新打开 IPC。
- 在双进程（MPI rank=2）场景下完成远端显存写入验证。

本设计仅描述仓库当前实现，不扩展到通用生产级内存管理框架。

## 2. 总体架构

工程分为三层：

- KMD 层：`driver/vmem_drv.c`
- UMD 封装层：`lib/vmem_lib.c` + `lib/vmem_lib.h`
- 测试应用层：`test/test_vmem.c`

三层关系如下：

1. 测试程序通过 Level Zero 分配设备内存并获取 IPC 句柄。
2. UMD 调用 `/dev/vmem` ioctl，把 IPC 句柄和目标 GPU BDF 传入 KMD。
3. KMD 完成 dma-buf attach/map，提取并回传 DMA 地址数组（`pfn_list`）。
4. 对端进程交换 `pfn_list` 后，再次通过 ioctl 让 KMD 导出新的 dma-buf FD。
5. 测试程序使用新 FD 调用 `zeMemOpenIpcHandle` 获得远端可访问指针并执行拷贝。

## 3. 目录与构建

- 顶层 `Makefile`
  - `make driver` 构建内核模块
  - `make lib` 构建静态/动态库
  - `make test` 构建测试程序
- 驱动目录 `driver/`
  - 产物：`vmem_drv.ko`
- 用户库目录 `lib/`
  - 产物：`libvmem.a`, `libvmem.so`
- 测试目录 `test/`
  - 产物：`test_vmem`

运行方式（当前 README）

1. `insmod driver/vmem_drv.ko`
2. `NEOReadDebugKeys=1 RenderCompressedBuffersEnabled=0 mpirun -n 2 test/test_vmem`

## 4. 关键数据结构与协议

定义文件：`driver/vmem_ioctl.h`

### 4.1 `struct pfn_list`

- `nents`：有效段数，当前上限 8。
- `addrs[8]`：每段 DMA 地址。
- `size[8]`：每段长度。

说明：命名沿用了 PFN 语义，但当前实现传输的是可直接用于 peer 访问的 DMA 地址，而非页号。

### 4.2 `struct open_handle_data`

- `ipc_handle`：Level Zero IPC 句柄原始字节区。
- `domain/bus/device/function`：目标 GPU BDF，用于驱动选择 attach 设备。
- `pfn_list`：地址段信息入参与回传。
- `fd`：由 KMD 导出的新 dma-buf FD（`VMEM_IOCTL_GET_IPC_HANDLE` 返回）。

### 4.3 ioctl 命令

- `VMEM_IOCTL_GET_PFN_LIST`（cmd=0）
  - 输入：`ipc_handle` + BDF
  - 输出：`pfn_list`
- `VMEM_IOCTL_GET_IPC_HANDLE`（cmd=1）
  - 输入：`pfn_list`
  - 输出：`fd`

## 5. KMD 详细设计

核心文件：`driver/vmem_drv.c`

### 5.1 设备模型与初始化

- 注册字符设备 `/dev/vmem`。
- 创建 class：`vmem_class`。
- 在 `vmem_init` 中扫描并缓存特定 Intel GPU 设备：
  - vendor 固定 `0x8086`
  - device id `0xe211` 与 `0xe210`
- 设备指针保存在 `vmem_pdevs[]`，模块退出时统一 `pci_dev_put`。

设计意图：将可用于 dma-buf attach 的 GPU 设备提前缓存，后续通过用户传入 BDF 精确匹配。

### 5.2 ioctl 路径 A：句柄转地址列表（GET_PFN_LIST）

步骤如下：

1. 从用户态复制 `open_handle_data`。
2. 从 `ipc_handle.data[0]` 提取 FD 并执行 `dma_buf_get(fd)`。
3. 按用户传入 BDF 在 `vmem_pdevs[]` 中匹配 `pci_dev`。
4. 对匹配设备执行 `dma_buf_dynamic_attach`。
5. 在 `dma_resv_lock` 保护下执行 `dma_buf_map_attachment` 获取 `sg_table`。
6. 遍历 scatterlist，读取 `sg_dma_address` 与 `length`。
7. 执行可选地址区间转换（见 5.4）。
8. 填充 `local_data.pfn_list` 并回写用户态。
9. 释放 map/attach/dmabuf 资源。

边界与保护：

- 若 `sgt_nents` 为 0，返回 `-ENODATA`。
- 若条目超过 8，截断导出并打印 warning。
- 任一 attach/map 失败会按路径逐级释放并返回错误码。

### 5.3 ioctl 路径 B：地址列表转新 FD（GET_IPC_HANDLE）

步骤如下：

1. 从用户态复制 `open_handle_data`。
2. 校验 `pfn_list.nents`（必须 `1..8`）。
3. 分配 `vmem_dmabuf_priv`，保存 `pfn_list` 副本。
4. 调用 `dma_buf_export` 创建导出对象。
5. 调用 `dma_buf_fd` 生成匿名 FD 并回写到 `local_data.fd`。

其中 `exp_info.size` 为各段 `size` 求和，作为导出总长度。

### 5.4 地址区间转换规则

函数：`vmem_translate_bar_dma_addr`

- 输入窗口：`[0x4a800000000, 0x4a800000000 + 0x1000000000)`
- 输出基址：`0x201000000000`
- 命中后：`translated = target + (dma - base)`

设计用途：对特定 BAR 地址区间做固定重映射，适配当前测试平台的地址视图。

### 5.5 导出 dma-buf 的 map/unmap 设计

`vmem_dmabuf_ops.map_dma_buf` 的关键点：

- 不预先存储 `sg_table`，而是在 map 时动态构造。
- 对每个段执行：
  - `sg_set_page(... ZERO_PAGE ...)`
  - `sg_dma_address = pfn_list.addrs[i]`
  - `sg_dma_len = pfn_list.size[i]`
- 注释中明确说明绕过 `dma_map_resource`，直接使用 DMA 地址做 P2P。

`unmap_dma_buf` 仅释放动态构建的 `sg_table`，不做 IOMMU teardown。

设计取舍：

- 优点：路径短、可直接表达跨 PCIe Switch 的地址。
- 风险：对平台拓扑与 IOMMU 配置强依赖，可移植性有限。

## 6. UMD 封装层设计

核心文件：`lib/vmem_lib.c`

### 6.1 职责

- 封装 `/dev/vmem` 的 open/close/read/write/ioctl。
- 缓存并下发目标 GPU BDF（`vmem_init`）。
- 暴露两类核心接口：
  - `vmem_open_handle`：IPC 句柄 -> `pfn_list`
  - `vmem_get_handle`：`pfn_list` -> 新 FD

### 6.2 关键行为

- `vmem_open_handle`
  - 将 `ze_ipc_mem_handle_t` 与 BDF 组成 `open_handle_data`。
  - 发起 `VMEM_IOCTL_GET_PFN_LIST`。
  - 从返回数据复制 `pfn_list`。
- `vmem_get_handle`
  - 将传入 `pfn_list` 写入 `open_handle_data`。
  - 发起 `VMEM_IOCTL_GET_IPC_HANDLE`。
  - 返回新 FD 到调用方。

日志控制：通过环境变量 `VMEM_LOG_INFO` 控制信息输出。

## 7. 测试程序详细流程

核心文件：`test/test_vmem.c`

### 7.1 前置条件

- 必须 `MPI` 双进程（`rank_size == 2`）。
- 要求至少 2 张 Level Zero 设备。
- 每个 rank 使用 `devices[rank]`。

### 7.2 主流程

1. 初始化 MPI 与 Level Zero context。
2. 每个 rank 分配 `send_buf` 与 `recv_buf`（device memory）。
3. 将主机初始化数据写入两个 device buffer。
4. 在 `get_remote_buf_ptr` 中完成“远端缓冲区句柄重建”：
   - `zeMemGetIpcHandle(local_ptr)` 获取本地句柄。
   - 读取本地 GPU BDF 并调用 `vmem_init`。
   - `vmem_open_handle` 获取本地 `pfn_list`。
   - 通过 `MPI_Sendrecv` 交换 `{pid, dma_fd, pfn_list}`。
   - 使用 `pidfd_open + pidfd_getfd` 获取对端原 FD（当前主要用于调试输出）。
   - 使用对端 `pfn_list` 调用 `vmem_get_handle` 获取“新 FD”。
   - 以新 FD 组装 `ze_ipc_mem_handle_t` 并 `zeMemOpenIpcHandle`。
5. 使用 `l0_memcpy(peer_recv_ptr, send_buf, nr_elements*sizeof(int))` 写入对端缓冲区。
6. 双方 barrier 后打印 `recv_buf` 验证数据变化。

### 7.3 数据交换结构

`struct exchange_data` 包含：

- `dma_buf_fd`：原 IPC handle 派生 FD。
- `pid`：进程号。
- `pfn_list`：可重建映射所需地址段。

## 8. 典型时序

### 8.1 句柄解析时序（A 路径）

1. App: `zeMemGetIpcHandle`
2. App -> UMD: `vmem_open_handle`
3. UMD -> KMD: `GET_PFN_LIST`
4. KMD: `dma_buf_get -> attach -> map -> extract sg dma`
5. KMD -> UMD/App: 返回 `pfn_list`

### 8.2 句柄重建时序（B 路径）

1. App: 通过 MPI 收到远端 `pfn_list`
2. App -> UMD: `vmem_get_handle`
3. UMD -> KMD: `GET_IPC_HANDLE`
4. KMD: `dma_buf_export + dma_buf_fd`
5. KMD -> UMD/App: 返回新 FD
6. App: `zeMemOpenIpcHandle(new_fd)`

### 8.3 端到端时序图（Mermaid）

独立文件见：[docs/vmem_tests_sequence_diagram.md](docs/vmem_tests_sequence_diagram.md)。

## 9. 错误处理与可观测性

- KMD 日志统一前缀 `vmem:`，关键阶段打印 attach/map/translate 信息。
- UMD 失败路径使用 `perror` 输出 ioctl 失败原因。
- 测试程序在关键节点打印 rank、pid、fd、BDF 与缓冲区内容。

当前实现以功能验证为主，未引入统计级 telemetry。

## 10. 约束与已知限制

1. `pfn_list` 固定最多 8 段，超出会截断或报错。
2. 设备枚举仅匹配 8086:e211/e210，平台适配范围窄。
3. 地址翻译采用硬编码窗口，依赖特定硬件地址布局。
4. `map_dma_buf` 直接设置 `sg_dma_address`，对 IOMMU/拓扑要求高。
5. 测试程序假设 `rank == device index` 且 `rank_size == 2`。
6. `get_remote_buf_ptr` 中基于 `pidfd_getfd` 得到的 FD 在当前路径中未作为最终 open 句柄使用（最终使用 `vmem_get_handle` 结果）。

## 11. 后续可演进方向

1. 将设备枚举从硬编码 DID 扩展为能力探测或模块参数配置。
2. 将地址翻译规则参数化，避免平台常量写死。
3. 支持可变长度地址段（动态数组）替代固定 8 段。
4. 增加单元化错误码与自检接口（例如 ioctl 查询能力版本）。
5. 扩展测试覆盖：多 rank、多 chunk、双向/并发拷贝与性能统计。

## 12. 代码映射索引

- ioctl 协议定义：`driver/vmem_ioctl.h`
- KMD 主逻辑：`driver/vmem_drv.c`
- UMD 封装：`lib/vmem_lib.c`, `lib/vmem_lib.h`
- 测试主流程：`test/test_vmem.c`
- 构建入口：`Makefile`, `driver/Makefile`, `lib/Makefile`, `test/Makefile`
