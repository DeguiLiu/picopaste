# picopaste 设计评审报告

- 日期：2026-09-11
- 评审对象：`docs/design/2026-09-11-picopaste-design.md`（本地提交 `d87a7f6`）
- 评审基线：newosp `main` @ `e1c6692`（0.8.1）、上游 `DeguiLiu/cc-clip` `main` 与 `windows-single-process`
- 评审方式：逐行核对文档引用的 `file:line`；依赖闭包静态分析；本机 sshd + `/usr/lib/openssh/sftp-server` 上跑真实 SFTP v3 交互
- 硬约束（本次评审新增）：**必须使用 newosp**；**状态机必须使用**；允许在 newosp 新建 `windows` 分支

---

## 0. 结论

判定：**架构方向通过；§6「newosp 使用映射」需重写；另有 5 项 P0、6 项 P1 修订。**

### 0.1 最重要的单个结论

文档 §6 把 newosp 的 Windows 工作量写成「补丁集中在 `platform.hpp` 一个头、改 4 处」、表格共 6 处。**实测不成立**：

| §6 表格条目 | 实测 | 判定 |
|---|---|---|
| `mem_pool.hpp` L60/L63 `OSP_TSAN_NO_RACE` | MSVC 既不定义 `__SANITIZE_THREAD__` 也无 `__has_feature` → 已走 `#else` 空宏 | **无效项**，无需改动 |
| `bus.hpp` L654/L706 `__builtin_prefetch` | 两处均已被 `#ifdef __GNUC__` 包裹 | **无效项**，无需改动 |
| `platform.hpp` L149-152 `OSP_LIKELY/UNUSED/PRINTF_FMT` | MSVC 不定义 `__GNUC__`/`__clang__` → 本就走 `#else` 恒等分支 | **非阻塞**，仅提示宏质量 |
| `platform.hpp` L306/L308 `CpuRelax` | MSVC 不定义 `__x86_64__` → 落到 `ThreadYield()`（`std::this_thread::yield`），能编译，仅自旋路径退化 | **非阻塞**，性能项 |
| `toml.hpp` | 76 处 `_MSC_VER`，`TOML_HAS_BUILTIN` 有 MSVC 分支 | 与文档一致，正确 |

真正的问题在表格之外：

1. **会编译失败（不在表中）**：`thread.hpp`（`<pthread.h>`、`<sched.h>`、`pthread_self`、`SCHED_FIFO/SCHED_IDLE`）、`shutdown.hpp`（`<unistd.h>`、`::pipe`、`::sigaction`、`::read/::close`）、`io_poller.hpp:42`（`#include <unistd.h>` 落在 `#if OSP_HAS_NETWORK` 之外）。
2. **编译通过但整文件被平台 `#if` 编空（不在表中）**：`process.hpp`（`process.hpp:50` 起全部内容在 `#if defined(OSP_PLATFORM_LINUX)` 内）、`system_monitor.hpp`（同款，头注释明写 "All monitoring functions return zeros on non-Linux platforms"）。这两个头恰好承担 §6 表格中的「子进程收容」与 §5 的「资源可观测」。
3. **「不使用网络模块」不减少编译面**：依赖闭包实测，文档列出「不使用」的 `socket.hpp`/`shell.hpp`/`io_poller.hpp`/`event_loop.hpp` 仍被 `shell_commands.hpp` 与 `config.hpp` 传递引入并参与编译。

结论：M1 的真实内容是一次 **newosp Windows 平台后端立项**（约 4 个后端文件 + 2 处修补 + CI），而不是 4 行补丁。

### 0.2 值得肯定之处

- **核心技术赌注成立且已在本机实证**：常驻 `ssh -s <host> sftp` + 自实现 SFTP v3 是可行且正确的路径；本机实测 10 个 opcode 完成 200 KB 上传、字节校验、改名、回读，SHA-256 一致（见 §3）。
- §9 失败模式对照表把上游「断了不自知」的根因逐条落到具体机制，是本设计最有价值的部分。
- 「远端零代码」「不做第二条上传代码路径」「失败不写剪贴板不敲键」三条纪律合理。

---

## 1. 结论速览

| 编号 | 级别 | 问题 | 证据 |
|---|---|---|---|
| P0-1 | 必须 | newosp Windows 范围被低估一个数量级 | §2、§2.3 |
| P0-2 | 必须 | 「零拷贝 DIB + 零临时文件 + 多秒上传」三者不可同时成立 | §4.1 |
| P0-3 | 必须 | 「空闲 CPU 为 0 / 不设周期性定时器」与 60 s `system_monitor`、看门狗心跳自相矛盾；且 `system_monitor` 在 Windows 无实现 | §4.2 |
| P0-4 | 必须 | SFTP `RENAME` 非覆盖语义（实测），§8 的 `settings.json` 写回方案不成立；`MKDIR` 已存在返回 FAILURE；远端 `uploads` 无保留策略 | §3.2 |
| P0-5 | 必须 | §10 的 zig/mingw 门禁抓不到 MSVC 阻塞点；需改为 MSVC 冒烟 TU 且 CI 自 M1 起跑 | §5 |
| P1-1 | 建议 | Job Object 32 MB 上限覆盖 `ssh.exe` 的风险 | §4.3 |
| P1-2 | 建议 | `SendInput` 返回值「等于 6」在注入了 Alt/Win 释放事件后不成立 | §4.3 |
| P1-3 | 建议 | 热键重入/上传排队策略未定义 | §4.3 |
| P1-4 | 建议 | 上游引用不可核实（仓库 issues 已禁用），目标仓库 `DeguiLiu/picopaste` 尚未创建 | §4.4 |
| P1-5 | 建议 | `shell_commands.hpp` 拖入 `shell.hpp`/`event_loop.hpp`/`bus.hpp`，应改用 `process.hpp` 或自建 | §2.4 |
| P1-6 | 建议 | 若干实现级补充（ATTRS 位掩码、多级 MKDIR、构建开关） | §4.5 |

---

## 2. newosp 使用评估（本次重点）

### 2.1 文档所列模块的依赖闭包实测

对文档「使用」表中的 16 个模块做传递闭包（`#include "osp/*.hpp"` 递归），共触达 30 个头：

```mermaid
flowchart TD
    subgraph OK["Windows 可直接用（无 POSIX 依赖）"]
        HSM["hsm.hpp + hsm_table.hpp<br/>仅依赖 platform.hpp"]
        VOC["vocabulary.hpp / mem_pool.hpp<br/>spsc_ringbuffer.hpp / breaker.hpp"]
        LOG["log.hpp / async_log.hpp（std 回退）"]
        TOML["toml.hpp（含 _MSC_VER 分支）"]
        CFG["config.hpp（平台保护 + std 回退）"]
        SEM["semaphore.hpp（非 Linux 走 condition_variable）"]
        TMR["timer.hpp / bus.hpp / watchdog.hpp"]
    end
    subgraph FAIL["Windows 编译失败（无平台保护）"]
        TH["thread.hpp:46-47, 402, 242-250<br/>pthread.h / sched.h / pthread_self"]
        SD["shutdown.hpp:44, 135, 221-224, 260<br/>unistd.h / pipe / sigaction"]
        IOP["io_poller.hpp:42<br/>unistd.h 在 #if OSP_HAS_NETWORK 之外"]
    end
    subgraph INERT["Windows 编译通过但功能为空"]
        PRC["process.hpp:50<br/>整文件在 #if OSP_PLATFORM_LINUX 内"]
        SYSM["system_monitor.hpp:87<br/>非 Linux 全部返回 0"]
    end
    SC["shell_commands.hpp"] --> SH["shell.hpp"]
    SC --> NMH["node_manager_hsm.hpp"]
    SC --> BUS["bus.hpp / config.hpp / fault_collector.hpp"]
    NMH --> EL["event_loop.hpp"]
    EL --> IOP
    FC["fault_collector.hpp"] --> TH
    SVC["service_hsm.hpp"] --> FC
    SVC --> HSM
    SYSM -.-> HK["§5 资源可观测"]
    PRC -.-> JC["§7 子进程收容"]

    classDef ok fill:#1f4a2f,stroke:#4aa96c,color:#e8faee
    classDef bad fill:#5f1f1f,stroke:#d94a4a,color:#fae8e8
    classDef inert fill:#4a3a1f,stroke:#d9a441,color:#faf3e8
    class HSM,VOC,LOG,TOML,CFG,SEM,TMR ok
    class TH,SD,IOP bad
    class PRC,SYSM inert
```

### 2.2 三分类明细（全部可复核）

| 模块 | 关键行 | Windows 事实 | 类别 |
|---|---|---|---|
| `hsm.hpp` | 仅 `#include "osp/platform.hpp"` | 无 POSIX 依赖 | 可用 |
| `hsm_table.hpp` | 仅依赖 `hsm.hpp` | 同上 | 可用 |
| `vocabulary.hpp` / `mem_pool.hpp` / `spsc_ringbuffer.hpp` / `breaker.hpp` / `watchdog.hpp` / `log.hpp` | 无 POSIX | 同上 | 可用 |
| `async_log.hpp` | L66 `#if LINUX \|\| MACOS` 包住 `sys/syscall.h`/`unistd.h` | std 回退可用（时间戳降为秒级，见 P1-6） | 可用 |
| `config.hpp` | L68 平台保护 | std 回退可用 | 可用 |
| `semaphore.hpp` | L47/L50 平台保护 | 非 Linux 走 `condition_variable` 回退，语义可用、延迟略高 | 可用 |
| `timer.hpp` / `bus.hpp` | L558、L653/L705 均有平台保护 | 可用 | 可用 |
| `thread.hpp` | L46-47 无条件 `pthread.h`/`sched.h`；L402 `pthread_self()`；L242-250 `pthread_setschedparam`+`SCHED_FIFO/SCHED_IDLE` | **MSVC 编译失败** | 需后端 |
| `shutdown.hpp` | L44 `unistd.h`；L135 `::pipe`；L221/L224 `::sigaction`；L260 `::read` | **MSVC 编译失败** | 需后端 |
| `io_poller.hpp` | L42 `#include <unistd.h>` 位于 L44 `#if OSP_HAS_NETWORK` 之前 | **MSVC 编译失败**，连带 `event_loop.hpp`/`node_manager_hsm.hpp`/`shell_commands.hpp` | 需修补（1 行） |
| `process.hpp` | L50 `#if defined(OSP_PLATFORM_LINUX)` 包住整个文件体 | 编译通过、无任何符号可用 | 需后端 |
| `system_monitor.hpp` | L87 同款；L79 注释「non-Linux 返回 0」 | 编译通过、返回零值 | 需后端 |

### 2.3 状态机（强制要求）评估

要求：状态机必须使用 newosp 的状态机实现。结论：**强制要求可以满足，但需明确选用哪一层。**

| 选择 | 依赖 | Windows 状态 |
|---|---|---|
| `osp::StateMachine<Context, MaxStates>` + `StateConfig` + `Event`/`EventQueue`（`hsm.hpp`） | 仅 `platform.hpp` | **今天就能用**，无需任何移植 |
| `osp::TableHsm`（`hsm_table.hpp`） | `hsm.hpp` | **今天就能用** |
| `osp::HsmService`（`service_hsm.hpp`） | `fault_collector.hpp` → `thread.hpp` + `semaphore.hpp`，并引入 `bus`/`breaker`/`log` | 必须先完成 `thread.hpp` 后端 |

建议：§6 中「生命周期状态机：Init → Connecting → Ready → Degraded → Reconnecting → Stopping」直接建在 `hsm.hpp`（`StateMachine` 或 `TableHsm`）之上，把 `HsmService` 作为可选升级项。理由：状态机是文档里唯一「今天就能在 Windows 跑」的核心模块，不应让它被 `thread.hpp` 的移植阻塞；且 `HsmService` 会把故障总线（`fault_collector` + `bus`）一并拉进编译面。

无论选哪一层，`thread.hpp` 后端都是必需的——文档需要 4 个线程（主消息循环、日志、SFTP 读、上传 worker），而 `osp::Thread`/`ThreadOptions` 是文档选定的线程原语。

### 2.4 依赖面收敛建议

`shell_commands.hpp` 是文档所列模块里依赖最重的一个：它引入 `shell.hpp`（termios/poll/socket）、`node_manager_hsm.hpp`（`event_loop.hpp` → `io_poller.hpp`）、`bus.hpp`、`config.hpp`、`fault_collector.hpp`。

而文档真正需要的只是「创建 `ssh.exe` 子进程并收容到 Job」。建议：

- 让 `osp::SpawnProcess`/`ProcessResult`（`process.hpp`）承担这一职责，并为它写 Windows 后端（`CreateProcessW` + 管道重定向 + Job 关联）；这与 §7 的 Job Object 需求天然重合，一份实现两处受益；
- 不使用 `shell_commands.hpp` / `osp::Shell`，直接消掉 `shell.hpp` + `event_loop.hpp` + `bus.hpp` 三个头的编译面；
- 构建时显式传 `-DOSP_WITH_NETWORK=OFF`（等价 `OSP_HAS_NETWORK=0`），并在 picopaste 侧显式打开 `OSP_CONFIG_TOML=ON`（newosp 默认 OFF）。

### 2.5 修正后的 newosp Windows 工作范围

| # | 文件 | 动作 | 类型 |
|---|---|---|---|
| 1 | `platform.hpp` | `OSP_PLATFORM_WINDOWS` 语义补全：hints 显式 MSVC 分支、`CpuRelax` 用 `_mm_pause()`/`YieldProcessor()`、`CoarseNowNs/Us` 走 `QueryPerformanceCounter` | 打磨（非阻塞） |
| 2 | `io_poller.hpp` | L42 `unistd.h` 移入 `#if OSP_HAS_NETWORK` | 1 行修补 |
| 3 | `thread.hpp` | Win32 后端：`std::thread` + `SetThreadPriority`/`SetThreadAffinityMask`；保持 `osp::Thread`/`ThreadOptions` API 与 RT-Thread 分支不变 | 后端 |
| 4 | `shutdown.hpp` | Win32 后端：`SetConsoleCtrlHandler` + 手动 reset event 取代 `pipe`/`sigaction` | 后端 |
| 5 | `process.hpp` | Win32 后端：`CreateProcessW` + 管道 + Job Object，保持 `ProcessResult` API | 后端 |
| 6 | `system_monitor.hpp` | Win32 后端：`GetProcessMemoryInfo`/`GetSystemTimes`/`GetDiskFreeSpaceEx` | 后端 |
| 7 | `semaphore.hpp`（可选） | 用 `CreateSemaphore`/`ReleaseSemaphore` 取代 `condition_variable` 回退，缩短唤醒延迟；现状不上也可用 | 可选 |
| 8 | `.github/workflows/ci.yml` | 增加 `windows-latest`（MSVC，可选加 clang-cl）作业，跑同一套 `ctest` | CI |
| 9 | 新增测试 | `tests/test_thread|shutdown|process|system_monitor` 在 Windows 通过；新增一个「全头冒烟 TU」（见 §5） | 测试 |
| 10 | 文档 | README 平台支持矩阵、CHANGELOG | 文档 |

### 2.6 `windows` 分支方案

用户已同意新建分支，建议如下：

- 分支名：`windows`，从 `main` 切出；**不直接改 `main`**，避免影响嵌入式/RT-Thread 使用方。
- 提交切分：按上表 1→10 顺序，每步独立可构建、可测试；每个提交只新增 `#if defined(OSP_PLATFORM_WINDOWS)` 分支，**不修改现有 Linux/RT-Thread 代码路径**。
- 门禁（每条都要有）：① 全头冒烟 TU 在 MSVC 下编译通过；② Linux 现有 61 个测试文件全绿（回归）；③ 新增 grep 门禁——`unistd.h`/`pthread.h`/`termios.h`/`fork(` 等 POSIX 符号必须出现在平台 `#if` 内。
- 合并策略：因为全部是新增平台分支，风险低，建议最终合并回 `main`，让 newosp 变成双平台库；在合并前 picopaste 以 `windows` 分支为 submodule/FetchContent 目标。

---

## 3. SFTP 链路本机实测

文档 §10 主张「架构中风险最高的 SFTP 编解码，恰好是唯一能在本机端到端真实验证的部分」。这一主张**成立**，已实测。

### 3.1 实测结果（本机 sshd + `/usr/lib/openssh/sftp-server`）

用 Python 手写 SFTP v3 帧，走 `ssh -o ClearAllForwardings=yes -s localhost sftp`：

```
VERSION v3
REALPATH type=104 -> /home/dgliu
MKDIR .cache/cc-clip-probe                     code=4 (已存在 -> FAILURE)
WRITE chunks=4 bytes=200000 / CLOSE code=0
STAT type=105 flags=15 remote_size=200000 local=200000 match=True
RENAME new-target code=0
VERIFY exists=True sha256_match=True
```

结论：文档需要的 10 个 opcode（`INIT/VERSION/REALPATH/MKDIR/OPEN/WRITE/CLOSE/STAT/RENAME/STATUS`）足以完成整条上传链路；`REALPATH "."` 确实返回 home（文档假设成立）；`STAT` 与本地字节比对可行。

### 3.2 实测暴露的三个协议陷阱（文档未覆盖）

1. **`RENAME` 不是覆盖语义**：把文件改名到一个**已存在**的目标返回 `SSH_FX_FAILURE(4)`。原因是 OpenSSH `sftp-server` 对普通文件用 `link()`+`unlink()` 实现改名，`link()` 遇到已存在目标即失败。
   - 对 §3 的主链路无影响（`clip-<ts>-<rand>.png` 是新名字）。
   - 但 §8「由客户端经 SFTP 读 → 合并 → 写回 `~/.claude/settings.json`」若采用「写临时名 + RENAME 覆盖」的原子手法，**必然失败**。必须改为：先 `REMOVE` 旧文件再 `RENAME`（存在非原子窗口，但已备份）、或直接 `OPEN` 截断覆盖 + 备份文件兜底。
2. **`MKDIR` 已存在返回 `SSH_FX_FAILURE(4)`**：客户端必须把 code 4 当作「已存在，继续」，否则第二次运行即失败。文档只写了 `MKDIR <home>/.cache/picopaste/uploads` 一次，实际需两级分别尝试（`~/.cache` 与 `~/.cache/picopaste/uploads` 都可能缺失或已存在）。
3. **`ATTRS` 必须按位解析**：`STAT` 返回 `flags=15`，`size` 仅在 `flags & SSH_FILEXFER_ATTR_SIZE` 时存在。按固定偏移取 size 会读出垃圾值（本评审第一版探针即因此读出错误 size）。这是编解码层最容易写错的一处，建议单测覆盖各 `flags` 组合。

---

## 4. 设计缺陷清单

### 4.1 P0-2：剪贴板零拷贝与多秒上传互斥

§4 主张两条路径都「不复制 DIB」，§3 时序图又要求在 `OPEN/WRITE` 循环（可能数秒）期间从剪贴板读取数据：

- 快路径「以 `GlobalLock` 锁定后按 64 KB 分块流式读出，零临时文件」意味着整个 SFTP 上传期间必须持有剪贴板打开状态；
- 回退路径「WIC 直接指向 DIB 像素（不复制）」则要求同一段时间内原始 `HGLOBAL` 保持有效。

后果：上传期间剪贴板被长期占锁（其他应用复制会失败/被阻塞）；剪贴板所有者可能已被替换或清空，指针随时可能失效。这与 §5「绝不把 DIB 拷进自己的堆」形成硬冲突——两条纪律不能同时满足。

可选修法（择一，需你拍板）：

1. **有界拷贝 + 显式失败**：把剪贴板数据读入启动期已分配的固定缓冲（如 4–8 MB），超限明确报「图太大，请改用 trzsz 人工通道」。内存预算内、无临时文件、无长占锁；
2. **临时文件兜底**：两条路径统一落临时文件（WIC 编码本就写文件），上传从文件流式读，上传结束即删。代价是多一次磁盘写，但换来最简实现，且与回退路径同构；
3. 保留零拷贝但接受「上传期间占锁」，并在文档中把它写成明确的已知代价。

建议选 2：与现有回退路径代码统一，去掉「快路径/回退路径」这条分叉本身就消除一类失败模式。

### 4.2 P0-3：空闲 CPU 为 0 与定时行为的自相矛盾

§2 与 §5 都写「不设周期性定时器」，§5 又写 `system_monitor` 默认每 60 s 记录一次，§6 还要求 `watchdog.hpp` 做线程心跳检测。心跳与周期性资源采样必然引入定时行为。

同时 `system_monitor.hpp` 在 Windows 上是零值实现（§2.2），即 §5「记录私有提交/工作集/峰值」在 Windows 上**没有任何现成实现**。

建议：明确改写为「不设忙轮询；所有周期性动作一律用可等待计时器（`CreateWaitableTimerEx` + `WaitForMultipleObjects`）或 `ReadFile` 管道等待，空闲时线程全部阻塞在内核对象上」，并说明心跳/采样的周期与开销；同时把 `system_monitor` 的 Windows 后端列入 M0/M4（否则 §5 的实测基线无从谈起）。

### 4.3 P1：Job Object、SendInput 计数、上传重入

- **Job Object 32 MB 上限**：`JOB_OBJECT_LIMIT_JOB_MEMORY` 是 job 内**所有进程**的合计提交上限，而 job 内包含 `ssh.exe`（文档自估约 10 MB）。32 MB 对「客户端 + OpenSSH」合计偏紧，且失败形态是 ssh 分配失败而非可读的错误提示。建议：M1 实测 `ssh.exe` 提交基线；或对 ssh 子进程使用独立 job（只继承 `KILL_ON_JOB_CLOSE`），内存上限只压主进程。
- **`SendInput` 计数**：为实现「释放物理按下的 Alt/Win」需额外注入 up 事件，因此「返回值必须等于 6」应改为「等于 6 + 本次注入的释放事件数」，否则辅助路径会自我误判为失败。
- **上传重入**：热键连按时上传 worker 的排队策略未定义（丢弃 / 合并 / 排队 N 个）。这属于「失败要立刻暴露」的边界，建议明确为「同一时刻仅一个上传，重复触发返回明确的 busy 状态」。

### 4.4 P1-4：引用与仓库现状

- `DeguiLiu/cc-clip` 的 issues 功能**已禁用**，`#80`、`#140` 在该仓库不存在（REST 返回 404）。§4、§9、§13 中的这两处引用属于不可核实引用；技术论点本身（`SendKeys` 会被 Chromium/Electron 终端忽略、SSH banner 污染 stdout）可以保留，但需替换为可核实来源或标注为经验性判断。
- `send.go` 的 `scp` 论证**已核实**：`cmd/cc-clip/send.go` 的 `sshUploadNoForward` 注释明确写「OpenSSH 9.0+ defaults `scp` to the SFTP subsystem」，与文档 §3 表格一致。
- `windows-single-process` 分支**确实存在**。
- 目标仓库 `DeguiLiu/picopaste` **尚未创建**（REST 404）；本地 `~/workspace/picopaste` 只有 1 个提交、仅含本设计文档、未配置 remote。属交付前置项。

### 4.5 P1-6：实现级补充

- `async_log.hpp` 在非 Linux/macOS 走 `std::time`+`localtime` 回退 → 日志时间戳仅秒级精度（`log.hpp:148-153`）。若 Windows 日志要求毫秒，需要在 `platform.hpp` 提供 `FormatTimestamp` 的 Windows 分支。
- `config.hpp` 的 INI 后端（`inicpp`）默认开启，TOML 后端默认关闭 → picopaste 需显式 `-DOSP_CONFIG_TOML=ON`，否则文档 §6 的「toml.hpp 配置解析」不会生效（会静默回退）。
- `tssh/tsshd` 环境下 `-s <host> sftp` 的可用性属未验证假设（文档已给出「子系统缺失时报可操作错误」的兜底，保留即可，但应在 M2 用真实环境验证一次）。

---

## 5. §10 验证策略修订

文档提出用 `zig c++ -target x86_64-windows-gnu` 交叉编译作为 win32 层门禁。该门禁**无法覆盖本设计最大的风险类别**：

- mingw 目标会定义 `__GNUC__` 并自带 `pthread.h`、`unistd.h`、`termios.h`；
- 因此 `thread.hpp`、`shutdown.hpp`（本设计最严重的两个 MSVC 阻塞点）在该门禁下**会全部通过**，给出虚假信心。

建议替换为：

| 层 | 手段 | 位置 |
|---|---|---|
| 平台无关核（SFTP 编解码、状态机、配置、日志轮转、退避） | Catch2 + ASan/UBSan/TSan，TDD | 本机 Linux（保留，可完整闭环） |
| SFTP 客户端 | 对本机真实 `sftp-server` 集成测试，**新增** MKDIR-已存在、RENAME-目标已存在、ATTRS 位掩码三类用例 | 本机 Linux |
| **头文件冒烟 TU** | 一个 `.cpp` 逐个 `#include` 全部 30 个闭包头（`-DOSP_WITH_NETWORK=OFF`），由 MSVC 编译 | **Windows CI（MSVC）** |
| POSIX 泄漏 grep 门禁 | `unistd.h`/`pthread.h`/`fork(` 等必须出现在平台 `#if` 内 | 本机/CI |
| MSVC 编译与测试 | `windows-latest` 作业，跑同一套 `ctest` | CI，**自 M1 起跑** |
| win32 运行行为 | 客户端内置 `selftest`（原设计，保留） | 你的 Windows 机器 |

一句话修订：**协议层可本机闭环；平台层必须靠 MSVC CI，不能靠 mingw 代理。**

---

## 6. 修订后里程碑

| M | 内容 | 完成判据 |
|---|---|---|
| **M0（新增）** | newosp `windows` 分支：`io_poller` 修补 + `thread`/`shutdown`/`process`/`system_monitor` 四个后端 + Windows CI | 全头冒烟 TU 编译通过；Linux 测试全绿；新增测试在 Windows 通过 |
| M1 | picopaste 骨架 + 双平台构建（依赖 M0） | Linux 与 MSVC 均能产出 exe |
| M2 | SFTP v3 编解码 + 集成测试 + **修正 RENAME/MKDIR/ATTRS 语义** | 上传/校验/改名测试全绿，含三类边界用例 |
| M3 | win32 层：剪贴板采集 + WIC + `SendInput` + 焦点守卫 + `selftest` | `selftest` 各项通过（Windows） |
| M4 | 单实例 + Job Object + 托盘 + 热键 + 日志轮转 + HSM 监督（`hsm.hpp`）+ `system_monitor` 后端 | 长跑无增长、无残留、空闲 CPU 为 0；内存基线有实测数字 |
| M5 | 端到端 + hook 注入（改用非覆盖写回）+ 远端 `uploads` 保留策略 + 文档 | 热键粘贴可用；失败路径均有明确报错 |

新增两项交付物：**远端上传目录保留策略**（否则 `~/.cache/picopaste/uploads` 无界增长，与「极致省资源」相悖；若用 SFTP 清理则 opcode 从 10 增至 13：`OPENDIR`/`READDIR`/`REMOVE`），**newosp Windows 后端与分支文档**。

---

## 7. 需你决策

1. **newosp Windows 范围与合并策略**：是否接受 §2.5 的 10 项范围（4 个后端 + 2 处修补 + CI）？该分支最终合并回 `main` 使 newosp 成为双平台库，还是长期独立维护？
2. **状态机层级**：状态机建在 `hsm.hpp`（`StateMachine`/`TableHsm`，Windows 今天即可用），还是必须上 `HsmService`（需先完成 `thread.hpp` 后端，并接受 `bus`/`fault_collector` 进入编译面）？
3. **剪贴板策略**（P0-2）：选有界拷贝、统一临时文件、还是保留零拷贝并接受长占锁？

---

## 附录：证据与复现命令

```bash
# newosp 基线
cd ~/newosp && git log --oneline -1 && git branch -a

# 文档引用的行号核对
sed -n '148,153p;300,312p' include/osp/platform.hpp
sed -n '55,70p' include/osp/mem_pool.hpp
sed -n '650,660p;700,712p' include/osp/bus.hpp
sed -n '42,50p;240,255p;398,404p' include/osp/thread.hpp
sed -n '40,48p;130,140p;218,228p;256,264p' include/osp/shutdown.hpp
sed -n '38,48p' include/osp/io_poller.hpp
sed -n '46,52p' include/osp/process.hpp
sed -n '84,90p' include/osp/system_monitor.hpp

# 依赖闭包与 Windows 阻塞点扫描（脚本见本报告 §2.1/§2.2 的判定逻辑）

# SFTP 实测：见 §3.1 输出（本机 sshd + /usr/lib/openssh/sftp-server）
ssh -o ClearAllForwardings=yes -o BatchMode=yes -s localhost sftp

# 上游核实
gh api repos/DeguiLiu/cc-clip/branches --jq '.[].name'      # windows-single-process 存在
gh api repos/DeguiLiu/cc-clip/issues/140                     # 404（issues 已禁用）
gh api repos/DeguiLiu/picopaste                            # 404（目标仓库未创建）
```
