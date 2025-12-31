#!/usr/bin/env bash
#SBATCH -J numa_sweep
#SBATCH -N 1
#SBATCH --exclusive
#SBATCH --cpus-per-task=64
#SBATCH --time=01:00:00
#SBATCH -o slurm-%j.out
#SBATCH -e slurm-%j.err
#SBATC -p your_partition

set -euo pipefail

# ---------------- user-tunable params via env ----------------
: "${SIZE_MB:=1024}"     # 测试内存大小 (MB)
: "${ITERS:=50}"         # 带宽循环次数
: "${STRIDE:=4096}"      # 延迟 stride
: "${BW_THREADS:=4}"     # 每次 BW 使用的线程数（从该 NUMA node 的 CPU 列表里取前 BW_THREADS 个）
: "${OUTDIR:=$PWD/numa_out_${SLURM_JOB_ID}}"

mkdir -p "$OUTDIR"

TOPO_OUT="$OUTDIR/numa_topology.txt"
RES_OUT="$OUTDIR/numa_bw_lat_results.csv"

echo "OUTDIR=$OUTDIR"
echo "Writing topology to $TOPO_OUT"
echo "Writing results  to $RES_OUT"

# ---------------- step1: detect NUMA topology ----------------
# 依赖 lscpu (通常都有). 用 lscpu -p=CPU,NODE 构建每个 node 的 CPU 列表。
# 输出格式与示例接近：
# NUMA:
#   NUMA node(s):           N
#   NUMA node0 CPU(s):      ...
#   NUMA node1 CPU(s):      ...
#
# 同时生成内部映射文件供后续使用。
MAP_TXT="$OUTDIR/cpu_node_map.tsv"  # CPU \t NODE

lscpu -p=CPU,NODE | awk -F, 'BEGIN{OFS="\t"} $1 !~ /^#/ {print $1,$2}' > "$MAP_TXT"

# node 集合
NODES=($(awk '{print $2}' "$MAP_TXT" | sort -n | uniq))
NUMA_N=${#NODES[@]}

{
  echo "NUMA:"
  printf "  NUMA node(s):           %d\n" "$NUMA_N"
} > "$TOPO_OUT"

# 把某个 node 的 CPU 列表压缩成 0-13,28-41 形式（尽量贴近示例）
compress_ranges() {
  # stdin: sorted cpu list (one per line)
  awk '
  BEGIN{first=1; start=-1; prev=-1;}
  function flush(){
    if (start==-1) return;
    if (!first) printf ",";
    if (start==prev) printf "%d", start;
    else printf "%d-%d", start, prev;
    first=0;
  }
  {
    x=$1+0;
    if (start==-1){start=x; prev=x; next;}
    if (x==prev+1){prev=x; next;}
    flush(); start=x; prev=x;
  }
  END{flush(); print "";}
  '
}

for n in "${NODES[@]}"; do
  CPU_LIST_COMPRESSED=$(awk -v node="$n" '$2==node {print $1}' "$MAP_TXT" | sort -n | compress_ranges | tr -d '\n')
  printf "  NUMA node%d CPU(s):      %s\n" "$n" "$CPU_LIST_COMPRESSED" >> "$TOPO_OUT"
done

# ---------------- step2: compile benchmark ----------------
echo "Compiling numa_bw_lat_v3.c ..."
gcc -O2 -march=native -pthread -o "$OUTDIR/numa_bw_lat_v3" "$SLURM_SUBMIT_DIR/numa_bw_lat_v3.c"

# ---------------- step3: run sweep ----------------
# 结果输出 CSV：
# cpu_node,mem_node,cpu_rep,bw_threads,bw_GiB_s,lat_ns
echo "cpu_node,mem_node,cpu_rep,bw_threads,bw_GiB_s,lat_ns" > "$RES_OUT"

# helper: get CPUs of a node as a comma-list (first K)
cpulist_first_k() {
  local node="$1"
  local k="$2"
  awk -v node="$node" '$2==node {print $1}' "$MAP_TXT" | sort -n | head -n "$k" | paste -sd, -
}

# helper: get representative CPU (first CPU of node)
cpu_rep_of_node() {
  local node="$1"
  awk -v node="$node" '$2==node {print $1}' "$MAP_TXT" | sort -n | head -n 1
}

# 实际 threads 不能超过 node 的 CPU 数
cpu_count_of_node() {
  local node="$1"
  awk -v node="$node" '$2==node {c++} END{print c+0}' "$MAP_TXT"
}

for cpu_node in "${NODES[@]}"; do
  CPU_REP=$(cpu_rep_of_node "$cpu_node")
  CNT=$(cpu_count_of_node "$cpu_node")
  T="$BW_THREADS"
  if (( T > CNT )); then T="$CNT"; fi
  if (( T < 1 )); then T=1; fi

  CPU_LIST=$(cpulist_first_k "$cpu_node" "$T")

  for mem_node in "${NODES[@]}"; do
    echo "Running cpu_node=$cpu_node(mem cpu_rep=$CPU_REP) -> mem_node=$mem_node threads=$T ..."

    # 运行一次 both，输出中抓 bw_GiB_per_sec 和 lat_ns_per_load
    OUT=$("$OUTDIR/numa_bw_lat_v3" \
      --cpu "$CPU_REP" \
      --memnode "$mem_node" \
      --size-mb "$SIZE_MB" \
      --iters "$ITERS" \
      --stride "$STRIDE" \
      --threads "$T" \
      --cpulist "$CPU_LIST" \
      --mode both)

    BW=$(echo "$OUT" | awk -F= '/bw_GiB_per_sec/ {print $2}' | tail -n 1)
    LAT=$(echo "$OUT" | awk -F= '/lat_ns_per_load/ {print $2}' | tail -n 1)

    # 容错：若解析失败给空
    BW=${BW:-}
    LAT=${LAT:-}

    echo "$cpu_node,$mem_node,$CPU_REP,$T,$BW,$LAT" >> "$RES_OUT"
  done
done

echo "Done."
echo "Topology: $TOPO_OUT"
echo "Results : $RES_OUT"
