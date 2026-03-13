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
| `heap ...` | VMM 页分配测试：分配/释放/自动化 selftest |

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

## `heap` — VMM 页分配测试

### `heap status`

查看 VMM alloc 区域和分配状态：

```
szy-kernel > heap status
PMM: 32395 / 32768 pages free (129580 KiB free)
VMM: alloc area ready = yes
Last alloc: (none)
```

### `heap alloc <pages>`

通过 `vmm_alloc_pages()` 分配 N 个连续虚拟页，并验证每页可读写：

```
szy-kernel > heap alloc 4
[heap] allocating 4 pages...
[heap] OK: vaddr=0xC8000000, PMM 32395 -> 32390 (-5 pages)
[heap] write/read verification: PASS (4 pages)
```

PMM 减少数可能 > 请求页数（额外消耗来自页表分配）。

### `heap free`

释放上一次 `heap alloc` 分配的页：

```
szy-kernel > heap free
[heap] freeing 4 pages at 0xC8000000...
[heap] OK: PMM 32390 -> 32394 (+4 pages)
```

### `heap test`

**自动化 selftest**——一条命令完成分配→写入→读回验证→释放→PMM 一致性检查：

```
szy-kernel > heap test
[heap-test] === vmm_alloc_pages selftest ===
[heap-test] PMM free before: 32395 pages
[heap-test] alloc 4 pages at 0xC8000000, PMM free: 32390 (-5)
[heap-test] (page table overhead: 1 extra pages)
[heap-test] write/read: PASS
[heap-test] free done, PMM free: 32394 (+4)
[heap-test] === ALL PASS ===
```

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

### 3. 验证 VMM 页分配/释放闭环

```
heap test
```

或手动步骤：

```
free
heap alloc 8
free
heap free
free
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
