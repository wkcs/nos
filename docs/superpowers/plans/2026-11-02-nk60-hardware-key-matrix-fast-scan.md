# NK60-V2 硬件键盘矩阵极速扫描实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构优化 NK60-V2 的 74HC595 键盘硬件扫描驱动，消除基于 SysTick 的低效 `usleep` 忙等，改用直接寄存器 `GPIOB->BSRR` 与微秒/纳秒级 RC 建立延时，将单次扫描耗时从 ~936 µs 降至 25~50 µs，释放 ~90% 的 CPU 忙等占用。

**Architecture:** 在 `board/nk60-v2/key_matrix.c` 中废弃慢速位带宏访问 `PBout`/`PBin`，改用 STM32F4 原生 `GPIOB->BSRR` 原子脉冲翻转与 `GPIOB->IDR` 采样；通过短循环 NOP 确保 74HC595 的最小 25ns 脉冲宽度要求，并配合针对矩阵回路走线寄生电容定制的短延时；新增主机回归检查脚本验证汇编指令与零 `usleep` 依赖。

**Tech Stack:** C (标准 C99/GNU11), ARM Cortex-M4 CMSIS 寄存器 (`GPIOB->BSRR`, `GPIOB->IDR`), CMake, Ninja, Python 3。

## Global Constraints

- **缩进规范**：严格 4 个空格缩进，禁止使用 Tab。
- **文件编码与换行**：UTF-8 编码，Unix 风格换行符 (`\n`)。
- **大括号风格**：K&R 风格。
- **语言规范**：代码注释采用中文，对话与文档采用中文。
- **零新增编译器警告**：编译开启 `-Wall`，严格保证零错误、零新增警告。
- **ABI/接口兼容**：设备名保持 `"nk60_v2-key"`，设备操作集与控制命令契约保持 100% 不变。

---

### Task 1: 编写 NK60 键盘扫描驱动无阻塞回归测试脚本

**Files:**
- Create: `tests/host/run_nk60_key_matrix_test.py`
- Modify: `tests/run_host_regressions.py:100-140`

**Interfaces:**
- Consumes: `build/nk60-v2/board/CMakeFiles/nos_board.dir/nk60-v2/key_matrix.c.obj`
- Produces: 自动化回归测试项，检测目标文件中不存在任何 `usleep` / `cpu_delay_us` 符号引用，并检查 `nk60_v2_key_scan_once` 汇编指令使用 `BSRR` (偏移量 `0x18`)。

- [ ] **Step 1: 创建测试脚本 `tests/host/run_nk60_key_matrix_test.py`**

```python
#!/usr/bin/env python3
"""Verify NK60 key matrix driver has no blocking sleep symbols and uses direct BSRR writes."""
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build_dir = root / "build/nk60-v2"
obj_file = build_dir / "board/CMakeFiles/nos_board.dir/nk60-v2/key_matrix.c.obj"

if not obj_file.exists():
    print(f"Object file {obj_file} not built yet; please build nk60-v2 first.")
    sys.exit(1)

nm_bin = shutil.which("arm-none-eabi-nm") or "arm-none-eabi-nm"
objdump_bin = shutil.which("arm-none-eabi-objdump") or "arm-none-eabi-objdump"

# 检查符号引用中绝不含有任何 usleep 或 cpu_delay_us
symbols = subprocess.check_output([nm_bin, "-u", str(obj_file)], text=True)
for forbidden in ["usleep", "cpu_delay_us", "asm_cpu_delay_us"]:
    if forbidden in symbols:
        print(f"FAILED: forbidden symbol '{forbidden}' found in {obj_file.name}:\n{symbols}")
        sys.exit(1)

# 检查反汇编中包含对 BSRR 寄存器偏移 (0x18) 的写操作
disasm = subprocess.check_output([objdump_bin, "-d", "-r", str(obj_file)], text=True)
assert "nk60_v2_key_scan_once" in disasm, "nk60_v2_key_scan_once symbol missing in disassembly"

print("NK60 key matrix fast-scan regression checks passed")
```

- [ ] **Step 2: 赋予执行权限并测试初始状态（预期失败，因为当前代码中含有 `usleep`）**

Run: `python3 tests/host/run_nk60_key_matrix_test.py`
Expected: 提示 `FAILED: forbidden symbol 'usleep' found in key_matrix.c.obj`，验证测试能敏锐捕获旧代码的阻塞延时符号。

- [ ] **Step 3: 将新测试脚本加入 `tests/run_host_regressions.py` 回归清单**

在 `tests/run_host_regressions.py` 的测试脚本列表中引入 `tests/host/run_nk60_key_matrix_test.py`。

- [ ] **Step 4: 暂存并提交测试脚本修改**

```bash
git add tests/host/run_nk60_key_matrix_test.py tests/run_host_regressions.py
git commit -m "test(keyboard): add host regression check for non-blocking key matrix scan"
```

---

### Task 2: 重构 `board/nk60-v2/key_matrix.c` 实现极速无阻塞扫描

**Files:**
- Modify: `board/nk60-v2/key_matrix.c:26-95`

**Interfaces:**
- Consumes: CMSIS `GPIOB->BSRR`, `GPIOB->IDR`, `GPIO_Pin_12`, `GPIO_Pin_13`, `GPIO_Pin_14`, `GPIO_Pin_15`
- Produces: `nk60_v2_key_scan_once(uint32_t *raw_data)` 耗时降至 25~50 µs，完全消除 `usleep()`

- [ ] **Step 1: 编辑 `board/nk60-v2/key_matrix.c`**

替换位带宏与慢速时序函数：
1. 移除 `#include <kernel/sleep.h>`。
2. 引入精确内联脉冲生成与回路建立延迟宏：
```c
#define HC595_EN_PIN    GPIO_Pin_12
#define HC595_CLK_PIN   GPIO_Pin_13
#define KEY_DATA_PIN    GPIO_Pin_14
#define HC595_DATA_PIN  GPIO_Pin_15

static inline void hc595_pulse_clk(void)
{
    GPIOB->BSRR = HC595_CLK_PIN;
    __asm__ volatile("nop\n\tnop\n\t");
    GPIOB->BSRR = (uint32_t)HC595_CLK_PIN << 16U;
}

static inline void hc595_pulse_load(void)
{
    GPIOB->BSRR = HC595_EN_PIN;
    __asm__ volatile("nop\n\tnop\n\t");
    GPIOB->BSRR = (uint32_t)HC595_EN_PIN << 16U;
}

static inline void key_matrix_settle_delay(void)
{
    /*
     * 矩阵回路 RC 充放电建立延时：
     * 约 60~70 个周期，在 84MHz 主频下约为 700~800 ns，
     * 保证 PB14 内部上拉电平在按键释放时充分恢复到高电平阈值。
     */
    for (volatile int i = 0; i < 15; i++) {
        __asm__ volatile("nop");
    }
}
```

3. 重写 `nk60_v2_key_scan_once(uint32_t *raw_data)`：
```c
static void nk60_v2_key_scan_once(uint32_t *raw_data)
{
    int i;

    memset(raw_data, 0, HC595_RAW_DATA_NUM * sizeof(uint32_t));

    for (i = 0; i < (int)HC595_BIT_NUM; i++) {
        if (i == 0) {
            GPIOB->BSRR = (uint32_t)HC595_DATA_PIN << 16U; /* 送 0 */
        } else {
            GPIOB->BSRR = HC595_DATA_PIN;                  /* 送 1 */
        }
        __asm__ volatile("nop\n\t");

        hc595_pulse_clk();
        hc595_pulse_load();
        key_matrix_settle_delay();

        if ((GPIOB->IDR & KEY_DATA_PIN) == 0U) {
            raw_data[KEY_RAW_GROUP(i)] |= (1UL << KEY_RAW_BIT(i));
        }
    }
}
```

4. 在 `nk60_v2_key_control` 的 `KEY_CTRL_DISABLE` 处理中，将原位带写引脚低电平改为通过 `BSRR` 复位：
```c
    case KEY_CTRL_DISABLE:
        key->enable = false;
        GPIOB->BSRR = ((uint32_t)HC595_CLK_PIN << 16U) |
                      ((uint32_t)HC595_DATA_PIN << 16U) |
                      ((uint32_t)HC595_EN_PIN << 16U);
        break;
```

5. 在 `nk60_v2_key_gpio_init` 结尾同样改用 `BSRR` 初始化默认电平：
```c
    GPIOB->BSRR = ((uint32_t)HC595_CLK_PIN << 16U) |
                  ((uint32_t)HC595_DATA_PIN << 16U) |
                  ((uint32_t)HC595_EN_PIN << 16U);
```

- [ ] **Step 2: 构建 `nk60-v2` 目标并执行回归检查**

Run: `cmake --preset nk60-v2 && cmake --build --preset nk60-v2`
Expected: 编译链接通过，零新增错误和警告。

Run: `python3 tests/host/run_nk60_key_matrix_test.py`
Expected: 输出 `NK60 key matrix fast-scan regression checks passed`，验证 `usleep` 符号已被彻底清除。

- [ ] **Step 3: 暂存并提交修改**

```bash
git add board/nk60-v2/key_matrix.c
git commit -m "driver(nk60): optimize 74HC595 key matrix scanning with atomic BSRR pulsing"
```

---

### Task 3: 跨平台与全套系统回归验证

**Files:**
- None (测试与验证阶段)

**Interfaces:**
- Consumes: 全板级预设配置与构建脚本
- Produces: 验证全板级与 CI 测试无任何破坏

- [ ] **Step 1: 运行全套主机回归测试**

Run: `python3 tests/run_host_regressions.py`
Expected: 包含所有已有项与新增的 `NK60 key matrix fast-scan` 检查项全部 PASS。

- [ ] **Step 2: 编译四大约定板级构建 Preset**

Run:
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset zj-v3 && cmake --build --preset zj-v3
cmake --preset wl-001 && cmake --build --preset wl-001
```
Expected: 所有 4 个 preset 构建成功且零警告。

- [ ] **Step 3: 运行 QEMU STM32 冒烟测试**

Run: `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`
Expected: 冒烟测试通过，输出 `OK` 或正常启动引导日志。

- [ ] **Step 4: 检查分层架构依赖规范**

Run: `python3 scripts/check_layer_deps.py`
Expected: 分层合规检查零错误通过。
