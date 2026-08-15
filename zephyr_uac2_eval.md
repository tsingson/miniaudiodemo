# Zephyr UAC2 支持成熟度评估（面向 STM32F401 / STM32H7B0）

调研日期：2026-08-14
调研方式：`git fetch origin main` 同步 `/Users/qinshen/go/zephyrproject/zephyr` 到最新 upstream main（`bfa1218a9e6`，提交时间 2026-08-14），并与项目当前实际使用的 `v4.4.2` 标签逐项比对；同时用 GitHub 网页检索了 `zephyrproject-rtos/zephyr` 的相关 issue / PR。本文只做只读调研，未修改 `zephyr/` checkout 的当前签出状态（仍停留在 `v4.4.2` 供本项目构建使用）。

## 1. 结论摘要（TL;DR）

- Zephyr 的 UAC2（USB Audio Class 2）实现基于新一代 `device_next`/UDC 协议栈：`subsys/usb/device_next/class/usbd_uac2.c` + `include/zephyr/usb/class/usbd_uac2.h` + `dts/bindings/usb/uac2/*`。协议/描述符层相对成熟，2024-02 至今持续有 double-buffering、多采样率、反馈端点等修复，且这些修复**均已包含在项目当前使用的 v4.4.2**。
- STM32F401（`st,stm32-otgfs`）与 STM32H7B0（继承自 h7a3 的 `st,stm32-otghs`，跑 Full-Speed 模式）**共用同一个新驱动 `drivers/usb/udc/udc_stm32.c`**，都支持等时（isochronous）端点，理论上都可以跑 UAC2。
- 但官方**没有任何 STM32 板卡的 UAC2 样例或 CI 覆盖**——`samples/subsys/usb/uac2_*` 只在 nRF5340/nRF54H20 上测试，`tests/subsys/usb/uac2` 只在 `native_sim`/`qemu_cortex_m3` 上跑描述符单元测试。STM32 上的 UAC2 完全是社区自行验证，没有上游回归测试兜底。
- **关键风险**：`udc_stm32.c` 从未实现 `HAL_PCD_ISOOUTIncompleteCallback()`，等时 OUT 端点一旦丢失一个（micro）frame 就会被 OTG 核心禁用且永不重新武装，表现为 UAC2 播放（host→device）方向"收到几个零长度包后彻底静默"。这正是 PR [#113622](https://github.com/zephyrproject-rtos/zephyr/pull/113622)（**截至调研时仍未合并**，包括最新 main 分支）要修复的问题，且该 PR 明确验证于 STM32U575 OTG-FS 的 UAC2+CDC-ACM 组合设备。本项目 `src/uac2/usb_uac2_device.c` 中已有的 `g_zero_packets` 计数器，统计的正好是这个已知上游缺陷的症状。
- 结论：Zephyr 主线目前对 STM32F401/H7B0 的 UAC2 支持处于"**协议层可用、驱动层等时恢复能力不完整**"的阶段，建议将 PR #113622 的补丁手动 cherry-pick 到本项目使用的 Zephyr checkout 上再评估播放稳定性，而不是假设上游默认状态足够稳定用于量产。

## 2. Zephyr UAC2 实现现状

### 2.1 代码组成（origin/main @ bfa1218a9e6）

```
include/zephyr/usb/class/usbd_uac2.h              UAC2 class 公共 API
subsys/usb/device_next/class/usbd_uac2.c          UAC2 class 实现（描述符生成、feedback、双缓冲等）
subsys/usb/device_next/class/usbd_uac2_macros.h   描述符宏
subsys/usb/device_next/class/Kconfig.uac2         CONFIG_USBD_AUDIO2_CLASS 等选项
dts/bindings/usb/uac2/*.yaml                      zephyr,uac2 / uac2-clock-source / uac2-input-terminal 等绑定
samples/subsys/usb/uac2_explicit_feedback/        显式反馈（explicit feedback）示例
samples/subsys/usb/uac2_implicit_feedback/        隐式反馈（implicit feedback）示例
tests/subsys/usb/uac2/                            纯描述符单元测试（native_sim/qemu）
doc/services/connectivity/usb/device_next/api/uac2_device.rst   API 文档（仅链接到头文件，无专门的成熟度说明）
```

本项目 `boards/stm32f401rct6_uac2.overlay` 中使用的 `zephyr,uac2` / `zephyr,uac2-clock-source` 等 compatible 与上述绑定完全匹配，说明此前的接入方式是正确的。

### 2.2 依赖的底层 USB 协议栈

UAC2 只存在于新的 `device_next`（`CONFIG_USB_DEVICE_STACK_NEXT`）+ UDC（USB Device Controller）驱动模型下，与旧的 `CONFIG_USB_DEVICE_STACK`（`drivers/usb/device/usb_dc_stm32.c`，本项目默认 `play_mcu.c`/CDC ACM 路径仍在用这套旧栈）是两条独立代码路径，不能混用。

### 2.3 STM32 UDC 驱动与两块目标芯片的映射

| 芯片 | USB 外设 | DT compatible | 驱动 | 速度 | 官方板卡 |
|---|---|---|---|---|---|
| STM32F401 | OTG FS | `st,stm32-otgfs`（`dts/arm/st/f4/stm32f4.dtsi`） | `drivers/usb/udc/udc_stm32.c`（`DT_DRV_COMPAT st_stm32_otgfs`） | Full-Speed only | 无官方样例板启用 USB；本项目自定义 overlay |
| STM32H7B0 | OTG HS（跑 FS 模式，嵌入式 PHY） | `st,stm32-otghs`（继承自 `dts/arm/st/h7/stm32h7a3.dtsi`，`stm32h7b0.dtsi` `#include` 它） | 同一个 `drivers/usb/udc/udc_stm32.c`（`DT_DRV_COMPAT st_stm32_otghs`） | Full-Speed only（`maximum-speed = "full-speed"`） | **有官方板卡** `boards/weact/mini_stm32h7b0`，其 `.dts` 已经把 `&usbotg_hs` 配置为 `zephyr_udc0`（PA11/PA12 + `cdc_acm_serial.dtsi`），说明官方已验证过基本 USB 枚举 |

两块芯片都落在同一份驱动实现上，意味着后续遇到的坑（或修复）基本是共享的：F401 上验证过的问题大概率在 H7B0 上重现，反之亦然。

`udc_stm32.c` 对等时端点的核心支持（`ep_cfg->caps.iso`、`STM32_PCD_EP_TYPE_ISOC` 等）在 2023-2024 年间随驱动初次上线就已存在；2026-03 的 `implement isochronous support on ST USB IP` 提交（`0dd0a8ff91a`）只针对**另一种更老的非 OTG 外设**（`st,stm32-usb`，如 F103/L1 那类），与 F401/H7B0 的 OTG FS/HS 无关，不需要额外关注。

## 3. 已知问题 / Issue 追踪（GitHub, zephyrproject-rtos/zephyr）

| # | 标题 | 状态 | 与本项目相关性 |
|---|---|---|---|
| [#113622](https://github.com/zephyrproject-rtos/zephyr/pull/113622) (PR) | `drivers: usb: udc: stm32: recover isochronous OUT on incomplete transfer` | **Open，未合并**（截至 2026-08-14，含最新 main 都未包含此提交） | **高**：直接解释"UAC2 播放收到几个包后静默"的现象，作者在 STM32U575 OTG-FS 上用 UAC2+CDC-ACM+DFU 复合设备验证修复有效；同一驱动路径覆盖 F401(OTG FS)/H7B0(OTG HS-FS) |
| [#102720](https://github.com/zephyrproject-rtos/zephyr/issues/102720) | `STM32N6 UDC: UAC2 explicit feedback broken after HS support commit (regression)` | Open，低优先级，assignee 为 ST 员工 | 中：症状与 #113622 相关（同样是 OUT 方向静默），但触发条件是 High-Speed 相关回归，F401/H7B0 均为 Full-Speed only，暴露面较小；ST 工程师仍在跟进 |
| [#93924](https://github.com/zephyrproject-rtos/zephyr/issues/93924) | `STM32 UDC shim driver does not pass Linux tests (testusb)` | Open，中优先级，长期跟踪（2025-07 至今） | 中：反映 STM32 UDC 驱动整体上尚未通过 Linux USB 一致性测试（`testusb`），部分子测试已通过修复关闭（PR #94001），但 High-Speed 下 `TEST 13 (set/clear halts)` 仍未修好；不直接卡 UAC2 全速播放，但说明驱动整体的边界情况覆盖仍不完整 |
| [#103324](https://github.com/zephyrproject-rtos/zephyr/issues/103324) | `USB device_next assigns same endpoint number to different transfer types (CDC + UAC2 composite)` | 已关闭（修复） | 低（已修复）：若未来做 CDC+UAC2 复合设备需注意此前有过端点号冲突问题 |
| [#114249](https://github.com/zephyrproject-rtos/zephyr/issues/114249) | `usbd_event_carrier() links net_buf into ep_events via buf->node, aliases the endpoint k_fifo link` | Open，中优先级，3 周内新开 | 低-中：device_next 核心 buffer 管理的一个新发现问题，非 STM32 专属，但可能影响所有基于 device_next 的 class（含 UAC2） |
| [#84741](https://github.com/zephyrproject-rtos/zephyr/issues/84741) | Coverity: usbd_uac2.c 整数溢出 | 已关闭 | 低（已修复，v4.4.2 已包含） |
| [#84664](https://github.com/zephyrproject-rtos/zephyr/issues/84664) | `buf_release_cb may be called with NULL buffer` | 已关闭 | 低（已修复） |

## 4. CI / 官方测试覆盖

- `samples/subsys/usb/uac2_explicit_feedback/tests.yaml` 与 `uac2_implicit_feedback/tests.yaml`：`platform_allow` 仅 `nrf5340dk/nrf5340/cpuapp`、`nrf54h20dk/nrf54h20/cpuapp`。
- `tests/subsys/usb/uac2/tests.yaml`：`platform_allow` 仅 `native_sim`、`qemu_cortex_m3`，且只覆盖描述符生成逻辑（`src/uac2_desc.c`），不做硬件收发验证。
- 结论：**没有任何 STM32 板卡出现在 UAC2 相关的官方 CI/twister 配置里**。STM32 + UAC2 的可用性完全依赖社区反馈（上表 issue）与自行硬件验证，上游合入新代码时不会自动跑 STM32 回归。

## 5. 与本项目现状的交叉印证

- `src/uac2/usb_uac2_device.c` 已经实现了 `g_zero_packets` / `g_invalid_packets` 计数器，行为模式与 #113622 描述的"host→device 播放收到零长度包后闲置"完全吻合——本项目此前遇到的调试信号很可能就是这个上游已知且尚未合并修复的问题，而不是本项目自身实现的 bug。
- `play_uac2.md` / `uac2_pcm5102_macos_dev_log.md` 中提到的 "Windows UAC2 driver 对 wMaxPacketSize 的非标准要求" 与上游 `CONFIG_USBD_UAC2_FS_WINDOWS_WORKAROUND` 选项描述一致，说明既有认知是准确的。
- 项目当前固定使用的 `v4.4.2` 已经包含 2024-2025 年的双缓冲/反馈端点等修复，属于相对新的稳定点；缺口主要集中在 2026 年才开的 #113622（等时 OUT 恢复）与仍在跟踪的 #93924（Linux 一致性测试）。

## 6. 成熟度评级

| 维度 | 评级 | 说明 |
|---|---|---|
| UAC2 协议/描述符层（`usbd_uac2.c`） | 较成熟 | 2024 年初上线，此后持续修复 buffer 泄漏、双缓冲、多采样率、feedback 端点大小等问题；v4.4.2 已包含 |
| STM32 UDC 驱动等时端点基础支持 | 中等 | 端点配置、双缓冲、SOF 均已实现，F401/H7B0 共用同一驱动 |
| STM32 UDC 驱动等时端点**异常恢复** | **不成熟** | 缺失 ISO OUT incomplete-transfer 重新武装逻辑（#113622 未合并），真实场景下 UAC2 播放会在丢帧后永久静默 |
| STM32 官方样例/板级验证 | 缺失 | 无 STM32 UAC2 官方 sample overlay/conf；H7B0 有官方板卡但只验证到 USB CDC ACM 枚举层面，未验证 UAC2 |
| 官方 CI 覆盖 | 无 | 仅 nRF 硬件 + native_sim 描述符单测 |
| 社区活跃度 | 良好 | ST 员工（mathieuchopstm 等）持续在 issue/PR 中跟进 STM32 USB 问题，响应及时 |

综合结论：**Zephyr 主线目前把 UAC2 在 STM32F401/H7B0 上的支持带到了"能跑通协议交互，但缺乏生产级等时传输容错"的阶段**，适合继续做原型/联调，但在没有应用 #113622 补丁之前，不建议假设播放链路能长时间稳定运行。

## 7. 后续实施：已将 #113622 补丁下沉到本项目（`src/uac2/`）

评估之后，判断"vendor 补丁"的成功概率**高**（已实测编译通过，细节见下），已完成实现：

- `src/uac2/udc_stm32.c`：以本项目 `ZEPHYR_BASE` 实际固定的 `v4.4.2` 版本 `drivers/usb/udc/udc_stm32.c` 为基线，验证 PR #113622 的 diff（89 行，仅新增两个 HAL 回调函数）可以**无冲突**直接应用到 v4.4.2（`patch -p1 --dry-run` 通过），随后把主线版本用的 `stm32_pcd_handle_t`/`stm32_status_t`（v4.4.2 尚不存在的 HAL2 抽象）替换回 v4.4.2 自带的 `PCD_HandleTypeDef`/`HAL_StatusTypeDef`，其余 1400+ 行与 Zephyr 原文件逐字节一致。
- `hpcd2data()`/`struct udc_stm32_data` 是文件内 `static`/私有定义，没有暴露在任何头文件里，因此**不能**只加一个小文件去补一个回调——必须整份 vendor 这个驱动文件。这是唯一现实的做法（已在 `src/uac2/README.md` 里写明原因，避免以后有人尝试"只加个小文件"却发现编译不过）。
- `CMakeLists.txt`：仅在 `MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN` 分支追加 `src/uac2/udc_stm32.c` 到 `target_sources`，并把 `${ZEPHYR_BASE}/drivers/usb/udc` 加入 include 路径（给私有的 `"udc_common.h"` 用；`stm32_usb_common.h` 已经是 Zephyr 全局 include，不用额外加）。
- `prj_uac2.conf`：新增 `CONFIG_UDC_STM32=n`，让 Zephyr 不再编译它自己那份 `udc_stm32.c`（否则两份文件都定义同一个 `DEVICE_DT_INST_DEFINE`，会链接冲突）。
- 新增项目根 `./Kconfig`（此前项目没有应用级 Kconfig，默认走 `${ZEPHYR_BASE}/Kconfig`）：补一个隐藏符号 `MINIAUDIO_UAC2_UDC_STM32_FIX`，只在 `USB_DEVICE_STACK_NEXT=y && UDC_STM32=n`（即只有 UAC2 硬件接收固件）时生效，用 `select` 把 `CONFIG_UDC_STM32` 原本会 `select` 的选项（`USE_STM32_LL_USB`/`USE_STM32_HAL_PCD`/`USE_STM32_HAL_PCD_EX`/`UDC_DRIVER_HAS_HIGH_SPEED_SUPPORT`/`STM32_USB_COMMON`/`PINCTRL`）重新选上——这些符号本身没有 prompt，不能直接在 `.conf` 里赋值，只能靠 `select`。同时给 `UDC_STM32_STACK_SIZE`/`UDC_STM32_THREAD_PRIORITY`/`UDC_STM32_MAX_QMESSAGES`/`UDC_STM32_OTG_RXFIFO_BASELINE_SIZE`（原本定义在 `if UDC_STM32` 块里，`UDC_STM32=n` 后它们会变成未定义宏）补上和上游一致的默认值（512/8/8/600）。
- 当前已验证构建：`./r.sh build-uac2-stm32`（FLASH 92.14%/RAM 93.78%）、`./r.sh build-uac2-implicit`（FLASH 92.12%/RAM 93.57%）、`./r.sh build-uac2-null`（FLASH 29.95%/RAM 49.15%）、`./r.sh build-uac2-null-implicit`（FLASH 29.95%/RAM 49.04%）。四个目标均成功链接，`src/uac2/udc_stm32.c` 被正确编入且 Zephyr 自带的同名文件未被重复编译。资源比例会随诊断和 pipeline 功能变化，以上数字只对应本次文档复查时的构建。
- 回归验证：默认 Zephyr、PCM5102A、LCD、CMSIS 测试入口及 host 应用/单元测试均已通过。当前源码按职责归入 `src/dspeq/`、`src/pcm5102/`、`src/lcd/`、`src/uac2/` 和 `src/httpctl/`。

**后续硬件验证结果**：STM32 已能在 macOS 枚举并通过 PCM5102A 实际播放音频；接收计数、`out_delivered` 和 `audio_drop` 已用于长时间观察。当前残余问题不是 #113622 所描述的端点永久静默，而是 HSI/PLLI2S 与主机之间约 2.5-3% 的时钟漂移；详细诊断见 `uac2_pcm5102_macos_dev_log.md`。等 Zephyr 上游把 #113622（或等价修复）合入并在本项目升级到的版本中验证稳定后，可按 `src/uac2/README.md` 的步骤删除 vendored 驱动并改回 Zephyr 自带实现。

## 8. 建议（原始调研阶段的建议，第 7 节已落地第 1 项）

1. ~~优先验证 #113622 补丁~~ **已完成**：见第 7 节，已 vendor 到 `src/uac2/`，并完成编译及硬件播放验证。
2. 补丁验证有效后，评估是否要固定（vendor/pin）这个 commit，或等待其正式合入 main 后再升级 Zephyr 版本；升级前先确认 `v4.4.2` → 目标 commit 之间没有引入其他破坏性变更（如 #102720 相关的 HS 检测逻辑变化，虽然 F401/H7B0 是 FS-only，理论上不受影响，但建议做一次全量回归）。
3. 继续关注 [#93924](https://github.com/zephyrproject-rtos/zephyr/issues/93924)（Linux testusb 一致性）与 [#114249](https://github.com/zephyrproject-rtos/zephyr/issues/114249)（device_next buffer 链接问题），两者都可能在长时间大数据量传输/USB Hub 场景下暴露。
4. 由于官方没有 STM32 UAC2 样例，建议保留本项目已经跑通的 `src/uac2/uac2_audio_stream.c` + `src/uac2/usb_uac2_device.c` 组合，连同调试计数器（zero/invalid packets、checksum）作为事实上的参考实现，并在遇到新的静默/丢帧现象时，第一时间对照上游 issue 列表（尤其是 area: USB + platform: STM32 标签）而不是默认归因于本项目的 I2S/DSP 管线。
5. H7B0 虽然有官方板卡定义（`weact/mini_stm32h7b0`），但官方只验证了 CDC ACM；建议在把 UAC2 移植到 H7B0 时，先复用 F401 上已验证的 `src/uac2/uac2_audio_stream.c` 和 `src/uac2/usb_uac2_device.c` 逻辑（`src/uac2/udc_stm32.c` 同时覆盖两者），重点检查 H7B0 特有的时钟树（`STM32_SRC_HSI48 USB_SEL`）与 RXFIFO 尺寸（`CONFIG_UDC_STM32_OTG_RXFIFO_BASELINE_SIZE`）是否需要针对更大 I2S 缓冲区调整。

## 9. 参考链接

- UAC2 class 实现：https://github.com/zephyrproject-rtos/zephyr/blob/main/subsys/usb/device_next/class/usbd_uac2.c
- STM32 UDC 驱动：https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/usb/udc/udc_stm32.c
- 官方 UAC2 示例（仅 nRF）：https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/subsys/usb/uac2_explicit_feedback
- PR #113622（等时 OUT 恢复，未合并）：https://github.com/zephyrproject-rtos/zephyr/pull/113622
- Issue #102720（STM32N6 UAC2 explicit feedback 回归）：https://github.com/zephyrproject-rtos/zephyr/issues/102720
- Issue #93924（STM32 UDC 不通过 Linux testusb）：https://github.com/zephyrproject-rtos/zephyr/issues/93924
- Issue #103324（CDC+UAC2 复合设备端点冲突，已修复）：https://github.com/zephyrproject-rtos/zephyr/issues/103324
- Issue #114249（device_next buffer 链接问题）：https://github.com/zephyrproject-rtos/zephyr/issues/114249
- WeAct STM32H7B0 官方板卡：https://github.com/zephyrproject-rtos/zephyr/tree/main/boards/weact/mini_stm32h7b0
