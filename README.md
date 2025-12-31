# NUMA Bandwidth & Latency Auto Benchmark (Slurm)

本项目提供一套 **基于 Slurm 的 NUMA 架构自动探测 + NUMA 带宽/延迟测试** 的一键化脚本，用于在多 NUMA node 服务器上系统性地测量：

- **CPU 位于某 NUMA node 时**
- **访问绑定到不同 NUMA node 的内存**
- 所对应的 **内存带宽（GiB/s）** 和 **访问延迟（ns）**

典型用途包括：
- 分析服务器 NUMA 拓扑与本地/远端访问差异
- 验证 NUMA 亲和性策略是否生效
- 为性能建模、调度策略或论文实验提供基础数据

---

## 1. 功能概述

该脚本完成以下工作流程：

1. **自动探测服务器 NUMA 架构**
   - NUMA node 数量
   - 每个 NUMA node 对应的 CPU 列表
2. **生成标准化 NUMA 拓扑描述文件**
3. **遍历所有 (cpu_node → mem_node) 组合**
4. 对每种组合执行：
   - 内存带宽测试（多线程、流式访问）
   - 内存延迟测试（随机指针追逐，抑制硬件预取）
5. **输出结构化 CSV 结果文件**
6. 所有步骤通过 **Slurm (`sbatch`) 在计算节点上自动完成**

---

## 2. 目录结构

项目最小目录结构如下：
```
numa_bw_lat_auto/
├── numa_bw_lat_v3.c # NUMA 带宽 / 延迟微基准程序
├── numa_sweep.sbatch # Slurm 作业脚本（核心逻辑）
├── submit_numa_sweep.sh # 一键提交脚本
└── README.md

运行后会自动生成输出目录，例如：
numa_out_run_20251231_114055/
├── numa_topology.txt
├── numa_bw_lat_results.csv
├── slurm-<jobid>.out
└── slurm-<jobid>.err

```
---

## 3. 环境要求

### 必须条件
- Linux（x86_64），支持 NUMA
- 多 NUMA node 服务器（≥ 2）
- Slurm 调度系统
- gcc（支持 `-O2 -march=native -pthread`）
- 基本用户态工具：
  - `bash`, `awk`, `sed`, `sort`, `uniq`, `paste`
  - `lscpu`

### 权限说明
- **不需要 root 权限**
- **不依赖 libnuma**
- 使用 `mbind` 与 `sched_setaffinity` 的系统调用

---

## 4. 快速开始

### 4.1 一键提交（推荐）

在项目根目录执行：

```bash
chmod +x submit_numa_sweep.sh
./submit_numa_sweep.sh
```

脚本会：
- 自动创建一个新的输出目录
- 使用 sbatch 提交 Slurm 作业
- 在计算节点上完成所有测试


### 4.2 自定义参数（可选）

可通过环境变量控制测试参数：
```bash
SIZE_MB=2048 \
ITERS=60 \
BW_THREADS=8 \
STRIDE=4096 \
./submit_numa_sweep.sh
```

参数说明：

| 参数        | 含义                        	 | 默认值       |
| ----------- | ------------------------------- | ----------- |
| SIZE_MB     | 每次测试分配的内存大小（MB）       |  1024		|
| ITERS       | 带宽测试循环次数                  |   50		|
|BW_THREADS   | 带宽测试线程数（每个 NUMA node 内）|   4		|
|STRIDE       |延迟测试 stride（字节）            |  4096		|