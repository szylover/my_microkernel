
## 2026-03-17 (Enhanced tests + testing appendix)
- `cmd_pmm.c`：新增 `pmm test` 自动化命令（5 项：alloc/free 往返、页对齐、唯一性、32 页批量压力、PHYS_TO_VIRT 写读验证）。
- `cmd_heap.c`：`heap test` 从 4 项扩展到 8 项（新增：8 字节对齐验证、碎片化压力、1~2048 递增大小、4096 大块分配）。
- `cmd_vma.c`：`vma test` 从 4 项扩展到 9 项（新增：边界半开区间、相邻区域、8 区域批量、移除不存在、count 一致性）。
- `scripts/test.sh`：新增 `TEST_VMA=1` 和 `TEST_PMM_TEST=1` 自动化标志；修复 PMM 输出匹配模式 `PMM: base=` → `PMM: min_base=`。
- 新增 `book/chapters/appendix-testing.tex`：内核测试框架附录（PMM/Heap/VMA 测试原理与代码详解、QEMU 自动化脚本、测试设计原则）。
- `book/main.tex`：附录部分加入 `appendix-testing`。

## 2026-03-16 (Book: add Part 5–7 placeholder chapters)
- `main.tex`：新增 Part 5 POSIX 兼容层（ch21–ch25）、Part 6 移植 GCC 与 Vim（ch26–ch29）、Part 7 网络（ch30–ch33），Part 4 补充 ch19 磁盘文件系统、ch20 设备文件。
- 新增 15 个占位章节文件（ch19–ch33），每个包含本章目标和背景知识 TODO 框架。
- `.github/copilot-instructions.md`：同步更新项目文件结构目录树。

## 2026-03-16 (Book restructure: split chapters + extract figures)
- 长章节 ch02–ch11 按 `\section` 拆分为子文件，原 `.tex` 变为骨架文件（`\chapter` + `\input`），子文件存放在 `chapters/chNN-name/secNN-slug.tex`。
- 60 个 TikZ 图片从章节源码提取到 `book/figures/chNN/figNN-slug.tex`，章节引用改为 `\input{figures/chNN/figNN-slug}`。
- 修复 15 个 TikZ 图的文字/图形重叠问题：增大 bitcell 宽度、parbox 尺寸、节点间距、text width 约束等。
- `preamble.tex`: 全局 `bitcell` 最小宽度 0.7cm → 1.0cm。
- `book/Makefile`: 依赖项增加 `chapters/**/*.tex` 和 `figures/**/*.tex`。
- `.github/copilot-instructions.md`: 新增 PDF 检查项到 Merge 前置检查清单；新增 Book/LaTeX 图片管理规范；补充 `book/` 项目文件结构。

## 2026-03-16 (Stage C-5/C-6 VMA rbtree + maple-tree backends)
- 新增 `kernel/mm/vma_rbtree.c`：红黑树 VMA 后端，静态节点池 256 + NIL 哨兵 + freelist 回收，O(log n) 全操作（插入修复 3 case + 删除修复 4 case），迭代中序遍历 dump。
- 新增 `kernel/mm/vma_maple.c`：Maple Tree (B+ tree 变体) VMA 后端，扇出度 10，树高 ≤ 3，叶节点分裂/上提，节点池 64 + VMA 池 256。
- `vma.h`：新增 `vma_maple_get_ops()` 声明。
- `kconfig.h`：新增 `KCONFIG_VMA_BACKEND`（0=sorted-array, 1=rbtree, 2=maple-tree）。
- `kmain.c`：VMA 后端选择改为 `#if KCONFIG_VMA_BACKEND` 三路分支。
- `book/chapters/ch11-vma.tex`：新增红黑树后端完整章节（节点结构、旋转代码、插入/删除修复、区间查找）；新增 Maple Tree 后端章节（B+ tree 节点结构、查找、分裂图解、与红黑树对比）；更新 dispatch 图为三后端；新增 kconfig 切换章节。
- 三后端均通过 `vma test` 全部 4 项自测（add+find / find-outside / overlap / remove+find）。
- 里程碑 C-5 (VMA 红黑树) ✅、C-6 (内存子系统集成测试) ✅，里程碑 C 全部完成。

## 2026-03-16 (Stage 9 VMA sorted-array backend)
- 新增 `kernel/mm/vma_sorted_array.c`：排序数组 VMA 后端（`vma_ops_t` 实现），静态数组 256 条目（恰好 1 页），二分查找 `find` $O(\log n)$，有序插入/删除 $O(n)$。
- 抽取 `sa_lower_bound()` / `sa_upper_bound()` 二分查找辅助函数，`sa_add`/`sa_remove`/`sa_find` 共用。
- `vma.h`：新增 `vma_sorted_array_get_ops()` 后端声明。
- `kmain.c`：注册 sorted-array 为默认 VMA 后端（替换之前注释的 rbtree）。
- `book/chapters/ch11-vma.tex`：新增「排序数组后端」完整章节（数据结构、插入/查找/删除图解、二分查找辅助函数代码、复杂度分析），更新 dispatch 图和练习题。
- `book/preamble.tex`：consolebox 终端样式修复——默认输出文字色调亮（`#B0BEC5`），显式重置 keyword/comment/string/identifier 样式为亮灰，深色背景上不再有黑色不可见文字。
- Stage 9 VMA 状态从「🚧 接口」更新为 ✅。

## 2026-03-16 (book fix + README)
- 修复 `book/chapters/ch07-pmm.tex` 第 390 行损坏内容（上次编辑残留 JSON 元数据和双反斜杠）。
- 修复 `book/chapters/ch08-vmm.tex`：TikZ 样式名 `step` 与内置关键字冲突，改为 `flowstep`；TikZ 节点多行文本添加 `align=center`。
- `book/preamble.tex`：新增 `patterns` 和 `shapes.geometric` TikZ 库，解决后续章节编译错误。
- `book/preamble.tex`：代码框 `frame=single` 改为 `frame=l`（左侧竖线），跨页不再出现不封闭矩形。
- `book/preamble.tex`：行号样式从 `\tiny\color{gray}` 改为 `\scriptsize\color{black!40}`，更清晰。
- `book/preamble.tex`：新增 `\codefile{}` 命令，用于在代码块前标注源文件路径。
- `book/main.tex`：添加 `\raggedbottom`，解决代码密集页面间距被异常拉大的问题。
- `book/main.tex`：封面改为 O'Reilly 风格（红色标题色带 + 留白 + 底部作者）。
- 为 ch02–ch11 共 98 处代码块添加 `\codefile{}` 源文件路径标注。
- 以上修复使 book PDF 两遍编译成功（153 页），目录正常生成。
- `README.md`：新增「配套书籍」段落（章节总览表 + PDF 构建命令）。
- 新增 `LICENSE`：MIT 许可证。

## 2026-03-16
- 新增 `include/kernel/vma.h`：VMA 可插拔后端接口（`vma_ops_t` 函数指针表 + `vm_area_t` 描述结构 + 权限标志），和 pmm_ops_t/heap_ops_t 同一套模式。
- 新增 `kernel/mm/vma.c`：VMA dispatch 层，通过 `g_vma_ops` 转发到注册的后端。
- 新增 `kernel/cmds/cmd_vma.c`：`vma` shell 命令（`list`/`find`/`count`/`test` 子命令），含 selftest 验证 add/find/remove/overlap。
- `vmm.h` 新增 `vmm_direct_map_end()` 声明，供 VMA 注册直接映射区边界。
- `vmm.c`：新增 `g_direct_map_end` 跟踪直接映射区上界；`vmm_alloc_pages`/`vmm_free_pages` 集成 VMA 自动注册/移除。
- `idt.c`：Page Fault handler 集成 VMA 查询，打印故障地址所属 VMA 及权限匹配诊断。
- `kmain.c`：引导末尾初始化 VMA 子系统并注册 `direct-map` 和 `kheap` 两个启动区域。
- `shell.c`：注册 `vma` 命令。
- 更新 `roadmap.md`：Stage 9 状态标记为「接口」。
- 更新 `memory-commands.md`：新增 `vma` 命令文档。
- **注意**：本次提交只含公共接口 + dispatch + cmd，第一个具体后端（vma_rbtree.c）在下一个 commit。

## 2026-03-13
- 新增 `include/kernel/kconfig.h`：内核编译期配置集中管理（PMM 后端 / 堆后端 / 初始堆大小），修改配置只需编辑此文件。
- 新增 `src/kernel/mm/heap_slab.c`：slab 分配器堆后端（7 级 cache 32B~2048B，bitmap 管理空闲 slot，配合 buddy PMM）。
- `kmalloc.h` 新增 `heap_slab_get_ops()` 后端声明。
- `kmain.c` 重构：PMM / 堆后端选择改为读取 `kconfig.h` 宏，不再硬编码。
- `kmalloc.c`：初始堆页数改为读取 `KCONFIG_HEAP_INITIAL_PAGES`。
- `roadmap.md` 新增 Stage 8b（Slab 分配器）。
- 阶段 8（kmalloc 完成）：新增 `src/kernel/mm/heap_first_fit.c`，first-fit 空闲链表堆后端（双向链表 block_header，支持向前/向后合并）。
- `vmm.h` 新增堆虚拟地址区间常量：`KHEAP_START` (0xE0000000) / `KHEAP_MAX_SIZE` (256MiB) / `KHEAP_END`。
- `kmalloc.h` 新增 `heap_first_fit_get_ops()` 后端声明。
- `kmalloc.c`：`kmalloc_init()` 实现——在 KHEAP_START 映射初始 16 页 (64KiB) 物理页，调用后端 init 建立空闲链表。
- `kmain.c`：引导末尾注册 first-fit 后端并调用 `kmalloc_init()`。
- `cmd_heap.c` 重写：从调 `vmm_alloc_pages` 改为调 `kmalloc/kfree`，单位从"页"变为"字节"，支持 8 个 slot 追踪，selftest 覆盖 alloc/write/read/free/stats/reuse。
- 更新 `docs/memory-commands.md`：heap 命令文档改为 kmalloc 语义，新增合并逻辑调试组合。
- 重构：将内存管理文件从 `kernel/core/` 拆分到 `kernel/mm/`（pmm/pmm_bitmap/pmm_buddy/vmm/kmalloc/mmap），core/ 只保留入口和基础设施（kmain/shell/printk/console）。

## 2026-03-12
- 合并 `pmm_bitmap.h` + `pmm_buddy.h` 为 `pmm_backends.h`：所有 PMM 后端统一在一个头文件声明，切换后端无需加删 #include。
- `kmain.c`：头文件分组整理（libc / arch / core内存 / core基础设施 / drivers）。
- 新增 `pmm_buddy.c`：buddy system 物理内存分配器后端（Linux 风格），支持 order 0..10 的 2^n 页分裂/合并。
- `kmain.c`：注册 buddy 为默认 PMM 后端（`pmm_register_backend(pmm_buddy_get_ops())`）。
- `boot.asm`：扩展 boot PSE 映射从 16MiB 到 1GiB（256 × 4MiB PSE 页循环），支持 buddy 在 vmm_init 前分配高地址页。
- `vmm.c`：vmm_init 映射范围扩展到覆盖 PMM 全部物理页（查询 `pmm_managed_base()`/`pmm_total_pages()`），不再硬编码 16MiB。
- PMM 可插拔后端重构：新增 `pmm_ops_t` 函数指针表接口（类似 Linux `file_operations`），支持插拔式物理内存分配器后端。
- 新增 `pmm.c`：薄 dispatch 层，通过 `g_pmm_ops` 转发所有 PMM 调用到注册的后端。
- 原 `pmm.c` 重命名为 `pmm_bitmap.c`：bitmap 分配器成为第一个后端，函数改名为 `pmm_bitmap_*`，末尾导出 `pmm_ops_t` 操作表。
- 新增 `pmm_backends.h`：统一声明所有 PMM 后端的 get_ops() 函数。
- `pmm.h` 扩展：新增 `pmm_ops_t` 定义、`pmm_register_backend()`、`pmm_backend_name()`。
- 所有 PMM 调用者（vmm.c、cmd_pmm.c、cmd_free.c、kmain.c）零改动。
- roadmap 更新：新增 Stage 9 VMA（红黑树管理），ktmalloc 明确为空闲链表，后续 stage 重编号。
- 阶段 7（拆除 Identity Mapping）：新增 `vmm_unmap_identity()` 清除 PD[0..767]，低地址空间不再可访问。
- PMM 修复：bitmap 指针、selftest 内存访问、mb2 info 解引用全部改用 `PHYS_TO_VIRT()`，不再依赖 identity mapping。
- 更新 `kmain.c`：`vmm_init()` 后将 `g_mb2_info` 从物理地址转为虚拟地址，随后调用 `vmm_unmap_identity()`。
- 更新 `mmap.c`：添加 `vmm.h` include，`g_mb2_info` 已在 kmain 中转为虚拟地址。
- 更新 `vmm.h`：声明 `vmm_unmap_identity()`。

## 2026-03-11
- 阶段 3（VMM 起步）：新增 `src/include/kernel/vmm.h`（页目录/页表类型、PDE/PTE flags、vmm_init/vmm_map_page/vmm_unmap_page API）。
- 阶段 3（VMM 实现）：新增 `src/kernel/core/vmm.c`（identity mapping 0–16MiB + 开启分页）、`src/kernel/arch/paging_flush.asm`（CR3 加载、CR0.PG 置位、invlpg TLB 刷新）。
- 更新 `kmain.c`：在 pmm_init() 后调用 vmm_init() 开启分页。

## 2026-03-10
- 新增 `linker.ld`：i386/Multiboot2 内核链接脚本（1MiB 链接地址，保留 `.multiboot_header`）。
- 新增 `.gitignore`：忽略常见构建产物与编辑器文件。
- 新增 `docs/howto.md`：构建 ISO、QEMU 启动与常见问题排查。
- 更新 `README.md`：增加 Quick Start 与 how-to 入口。
- 阶段 1（1~4）：加入最小 C 内核入口 `kmain`、COM1 串口输出，并从 `_start` 传入 Multiboot2 magic/info 指针并打印 tag 列表。
- 阶段 2（GDT 起步）：新增 GDT 初始化（平坦 code/data 段）与 `lgdt`/far jump 刷新段寄存器，并在串口输出 before/after 便于验证。
- 更新 `docs/agent.md`：追加“学习级注释规范”。
- 阶段 2（IDT 起步）：新增 IDT 初始化与 0..31 异常 ISR stubs，串口打印异常向量/错误码/EIP，并用 `int3` 做自检。
- 修复 `src/boot/boot.asm`：补全 Multiboot2 header（长度/校验和/结束 tag），便于 GRUB 识别。
- 调整 `linker.ld`：将 `.multiboot_header` 放入可加载的 `.text` 段最前，并增加常见段与对齐。
- 更新 `Makefile`：使用 `grub-mkrescue` 生成 ISO，并用 QEMU `-cdrom` 启动。
- 阶段 2（IRQ 起步）：新增 8259 PIC remap/mask/EOI、IRQ0..15 汇编 stubs，并在 IDT 安装 0x20..0x2F。
- 阶段 2（Keyboard 起步）：新增 IRQ1 键盘 scancode->ASCII（含 Shift）与环形缓冲区，对外提供 `keyboard_getc/keyboard_try_getc`。
- 阶段 2（Shell 起步）：新增最小交互式 shell（`szy-kernel >`，行编辑 + 命令表），当前仅实现 `cls`（其余命令按 docs 规划逐步加回）。
- 新增 `docs/shell.md`：记录 cmd 风格 shell 的 UX/分层/命令规划（help/info/cls/mmap/cpu）与阶段落地路线。
- 阶段 2（Command 模块化）：新增 `src/include/kernel/cmd.h` 命令接口；将 `cls` 拆为独立命令模块（`src/kernel/cmds/cmd_cls.c`），shell 通过注册表调用。
- 阶段 2（Console 输入层）：新增 `src/include/kernel/console.h`/`src/kernel/core/console.c`，统一键盘（IRQ1 缓冲）与串口（轮询）输入；shell 只依赖 console。
- 阶段 2（Shell 命令）：新增 `shutdown` 命令模块（`src/kernel/cmds/cmd_shutdown.c`），尝试触发 QEMU/Bochs 关机，失败则 halt。
- 阶段 2（Shell 命令）：新增 `cmds` 命令模块（`src/kernel/cmds/cmd_cmds.c`），列出当前所有可用命令。
- 开发体验：新增 `.clangd`（clangd fallback 编译参数）、`make compdb`/`scripts/compdb.sh`（通过 bear 生成 `compile_commands.json`），并在 `docs/agent.md` 补充 VS Code + clangd 配置说明。
- 更新 `docs/agent.md`：明确 clangd/`compile_commands.json` 的“每次改动同步更新”规则，并补充变更记录同步建议。
