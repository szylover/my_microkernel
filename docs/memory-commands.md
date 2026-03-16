# 内存调试命令速查表

> 在 `szy-kernel >` 提示符下输入以下命令。所有地址支持十进制和十六进制（`0x` 前缀）。

---

## 一览表

| 命令 | 用途 |
|------|------|
| `free` | 快速查看物理内存使用量 |
| `mmap` | 显示 Multiboot2 内存映射（硬件报告的 RAM 区域） |
| `pmm ...` | PMM 物理内存管理：状态/分配/释放/位图 |
| `vmm ...` | VMM 虚拟内存管理：页表查询/映射/取消映射/故障测试 |
| `heap ...` | 内核堆调试：kmalloc/kfree 状态/分配/释放/selftest |
| `vma ...` | VMA 虚拟内存区域：列表/查找/计数/selftest |

---

## `free` — 物理内存用量

```
szy-kernel > free
PMM: free 32395 / 32768 pages
PMM: free 129580KiB, used 1492KiB
```

---

## `mmap` — Multiboot2 内存映射

```
szy-kernel > mmap
Available RAM: 0x00000000 - 0x0009FC00 (0MB)
Available RAM: 0x00100000 - 0x07FE0000 (126MB)
```

显示 BIOS/UEFI 报告的物理内存区域（在 PMM 之前就确定的硬件信息）。

---

## `pmm` — 物理内存管理器

### `pmm state` / `pmm stat`

查看 PMM 初始化状态和页面统计：

```
szy-kernel > pmm state
PMM: base=0x00100000, page_size=4096
PMM: total=32768, free=32395, used=373
PMM: backend=buddy
```

### `pmm alloc [count]`

手动分配 N 个物理页（默认 1，最多 256）：

```
szy-kernel > pmm alloc 3
pmm alloc: 0x07FC1000
pmm alloc: 0x07FC2000
pmm alloc: 0x07FC3000
```

分配的地址会被记录，可用 `pmm freeall` 一次释放。

### `pmm free <addr>`

释放指定物理地址的页：

```
szy-kernel > pmm free 0x07FC1000
```

### `pmm freeall`

释放所有通过 `pmm alloc` 分配的页：

```
szy-kernel > pmm freeall
pmm freeall: freed 3 pages
```

### `pmm dump [start_index] [count]`

显示物理页使用位图（默认从 0 开始显示 64 页，最多 256）：

```
szy-kernel > pmm dump 0 64
########........................  idx   0 addr 0x00100000
................................  idx  32 addr 0x00180000
```

- `#` = 已使用，`.` = 空闲，`?` = 查询失败
- 每行 32 页，对应 128KiB

### `pmm help`

显示所有 pmm 子命令用法。

---

## `vmm` — 虚拟内存管理器（页表）

### `vmm state`

查看分页状态概览：

```
szy-kernel > vmm state
VMM: paging enabled
VMM: 512 / 1024 PDE entries present
VMM: 2048 MiB mapped
```

### `vmm lookup <addr>`

虚拟地址 → 物理地址翻译，显示 PTE 标志位：

```
szy-kernel > vmm lookup 0xC0100000
virt 0xC0100000 -> phys 0x00100000  [P RW S A D]
```

标志位含义：
- `P` = Present（有效）
- `RW` / `RO` = Read-Write / Read-Only
- `U` / `S` = User / Supervisor
- `A` = Accessed（已访问）
- `D` = Dirty（已写入）

### `vmm pd [start] [count]`

查看页目录条目（默认前 16 个，最多 128）：

```
szy-kernel > vmm pd 768 4
PD[768] virt=0xC0000000 pt=0x00101000 [P RW S]
PD[769] virt=0xC0400000 pt=0x00102000 [P RW S]
PD[770] virt=0xC0800000 pt=0x00103000 [P RW S]
PD[771] virt=0xC0C00000 pt=0x00104000 [P RW S]
```

- 每个 PDE 覆盖 4MiB 虚拟地址空间
- PD[0..767] = 用户态（identity mapping 已拆除）
- PD[768..1023] = 内核态（0xC0000000+）

### `vmm pt <pd_index>`

查看某个 PDE 下的所有页表条目：

```
szy-kernel > vmm pt 768
PT[  0] virt=0xC0000000 -> phys=0x00000000 [P RW S]
PT[  1] virt=0xC0001000 -> phys=0x00001000 [P RW S]
...
```

最多显示 64 条（避免串口刷屏）。

### `vmm map <virt> <phys>`

手动建立一个虚拟→物理页映射：

```
szy-kernel > vmm map 0x00500000 0x00200000
```

两个地址都必须 4KiB 对齐。

### `vmm unmap <virt>`

解除一个虚拟页映射：

```
szy-kernel > vmm unmap 0x00500000
```

### `vmm fault`

**故意触发 Page Fault**，用于测试 #PF handler：

```
szy-kernel > vmm fault
```

会读取未映射地址 `0xDEAD0000` → 触发 #PF → 打印错误码和 CR2 → halt。**不可恢复**。

---

## `heap` — 内核堆 (kmalloc/kfree)

堆后端为 first-fit 空闲链表，虚拟地址区间 `0xE0000000 - 0xF0000000` (256MiB)。
初始 64KiB，按需 grow。

### `heap status`

查看堆统计信息和追踪的分配：

```
szy-kernel > heap status
Heap backend: first_fit
Heap size:    65536 bytes (64 KiB)
Used:         0 bytes (0 allocs)
Free:         65520 bytes (1 blocks)
Tracked slots: 0 / 8
```

- `Heap size` = 已提交区域（heap_brk - heap_start）
- `Used` = 已分配字节数（含 header 开销）
- `Free` = 空闲字节数
- `Tracked slots` = `heap alloc` 命令记录的指针（最多 8 个）

### `heap alloc <bytes>`

调用 `kmalloc(bytes)` 分配内存，写入并读回验证：

```
szy-kernel > heap alloc 64
[heap] alloc: req=64 aligned=64
[heap] alloc: found block @ 0xe0000000 (size=65520, need=64)
[heap] alloc: split -> used 64 + free 65440 @ 0xe0000050
[heap] alloc: return 0xe0000010
[heap] OK: kmalloc(64) = 0xe0000010
[heap] write/read 64 bytes: PASS
```

关键验证点：
- 返回地址应在 `0xE0000000` 区间内
- `split` 说明大块被拆分成"已分配 + 新空闲块"
- write/read PASS 说明映射的物理页可正常读写

可以连续多次 alloc（最多 8 个 slot）：

```
szy-kernel > heap alloc 128
szy-kernel > heap alloc 256
```

### `heap free`

调用 `kfree()` 释放最后一个 slot（LIFO 顺序）：

```
szy-kernel > heap free
[heap] free: ptr=0xe00000a0 block=0xe0000090 size=256
[heap] free: merge forward 0xe0000090 + 0xe0000190
[heap] free: done (merged 1 adjacent blocks)
[heap] kfree(0xe00000a0) (256 bytes)
[heap] OK
```

`merge forward` 说明释放的块和后面的空闲块合并了。

### `heap test`

**自动化 selftest**——4 个测试一条命令跑完：

```
szy-kernel > heap test
[heap-test] === kmalloc/kfree selftest ===
[heap-test] test 1: basic alloc/free
[heap-test] kmalloc(64)  = 0xe0000010
[heap-test] write/read 64 bytes: PASS
[heap-test] kfree: OK
[heap-test] test 2: multiple allocs (32, 128, 256, 1024)
[heap-test] kmalloc(  32) = 0xe0000010
[heap-test] kmalloc( 128) = 0xe0000040
[heap-test] kmalloc( 256) = 0xe00000d0
[heap-test] kmalloc(1024) = 0xe00001e0
[heap-test] no overlap: PASS
[heap-test] test 3: free all, check stats
[heap-test] after free: alloc_count=0, free_bytes=65520
[heap-test] alloc_count == 0: PASS
[heap-test] test 4: reuse after free
[heap-test] re-alloc kmalloc(64) = 0xe0000010
[heap-test] reuse: PASS
[heap-test] === ALL PASS ===
```

| 测试 | 验证内容 | 失败说明 |
|------|---------|---------|
| test 1 | 基本 alloc(64) → 写 64 字节 → 读回 → free | alloc 或 free 逻辑有 bug |
| test 2 | 连续 4 次 alloc 不同大小，检查地址不重叠 | 拆分逻辑有问题，块互相覆盖 |
| test 3 | 全部 free 后 alloc_count == 0 | free 没正确标记 is_free |
| test 4 | free 后重新 alloc 同大小，验证空间复用 | 合并（coalescing）逻辑有 bug |

---

## `vma` — 虚拟内存区域管理

VMA 跟踪内核所有已注册的虚拟内存区域，供 Page Fault handler 判断访问是否合法。

### `vma list`

按地址排序列出所有 VMA：

```
szy-kernel > vma list
VMA regions (2):
  [ 0] 0xC0000000 - 0xD0000000  256 MiB  rwx  direct-map
  [ 1] 0xE0000000 - 0xE0010000   64 KiB  rw-  kheap
```

字段含义：
- 地址范围：[start, end) 左闭右开
- 大小：自动选择 B / KiB / MiB 单位
- 权限：`r`=读 `w`=写 `x`=执行，`-`=无
- 名称：区域用途

### `vma find <addr>`

查找包含指定地址的 VMA：

```
szy-kernel > vma find 0xC0100000
0xC0100000 -> VMA [0xC0000000, 0xD0000000) 'direct-map'
  size: 262144 KiB, offset: 0x100000, flags: rwx

szy-kernel > vma find 0xDEAD0000
0xDEAD0000 -> no VMA (unmapped / untracked)
```

### `vma count`

显示后端信息和 VMA 数量：

```
szy-kernel > vma count
VMA backend: rbtree
VMA count:   2
```

### `vma test`

自动化 selftest——4 个测试一条命令跑完：

```
szy-kernel > vma test
[vma-test] === VMA selftest ===
[vma-test] test 1: add + find
[vma-test] PASS
[vma-test] test 2: find outside
[vma-test] PASS
[vma-test] test 3: overlap detection
[vma-test] PASS
[vma-test] test 4: remove + find
[vma-test] PASS
[vma-test] === ALL PASS ===
```

| 测试 | 验证内容 |
|------|----------|
| test 1 | add 后 find 能命中 |
| test 2 | find 范围外地址返回 NULL |
| test 3 | 重叠区间被拒绝 |
| test 4 | remove 后 find 不再命中 |

---

## 常用调试组合

### 1. 检查整体内存状态

```
free
pmm state
vmm state
```

### 2. 验证 PMM 分配/释放闭环

```
pmm state
pmm alloc 5
pmm state
pmm freeall
pmm state
```

预期：第三次 `pmm state` 的 free 数应恢复到第一次。

### 3. 验证 kmalloc/kfree 闭环

一条命令自动测试：

```
heap test
```

或手动步骤（观察堆状态变化）：

```
heap status              # 初始: 0 allocs, ~65520 free
heap alloc 64            # 分配 64 字节
heap alloc 128           # 再分配 128 字节
heap status              # 应该: 2 allocs
heap free                # 释放 128 字节 (LIFO)
heap free                # 释放 64 字节
heap status              # 应恢复: 0 allocs, ~65520 free
```

### 4. 验证合并逻辑

```
heap alloc 32            # slot[0]
heap alloc 32            # slot[1]
heap alloc 32            # slot[2]
heap free                # 释放 slot[2]，应和尾部空闲块 merge forward
heap free                # 释放 slot[1]，应 merge forward (吃 slot[2]+尾部)
heap free                # 释放 slot[0]，应 merge forward → 恢复为 1 个大空闲块
heap status              # 应该: 0 allocs, 1 free block, ~65520 free
```

### 4. 查看内核页表映射

```
vmm pd 768 8
vmm pt 768
vmm lookup 0xC0100000
```

### 5. 查看物理页使用位图

```
pmm dump 0 128
```

### 6. 测试 Page Fault handler

```
vmm fault
```

**警告**：会 halt，之后需要重启 QEMU。

---

## 自动化测试

在宿主机终端运行：

```bash
make test                    # 基础 smoke test
TEST_MMAP=1 make test        # + mmap 命令输出验证
TEST_PMM=1 make test         # + PMM alloc/free 闭环验证
TEST_HEAP=1 make test        # + VMM 页分配 selftest
```

全部一起跑：

```bash
TEST_MMAP=1 TEST_PMM=1 TEST_HEAP=1 make test
```
