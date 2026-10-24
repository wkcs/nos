# ESP32-C3 验证记录

<!-- Copyright (C) 2026 NOS contributors -->

实现、命令和硬件边界见 [ESP32_C3.md](ESP32_C3.md)。本批需要冻结源码后重新执行以下命令，并在结束后补录提交身份与实际报告；开发阶段的启动日志不作为最终冻结报告。

```sh
python3 tests/esp32c3.py --output-root out/esp32c3/final-validation
python3 tests/esp32s3.py --output-root out/esp32s3/c3-shared-validation
source /workspace/.nos-env/activate.sh
python3 tests/acceptance.py --include-posix --require-frozen-source
```

专项核对各自真实 machine、RISC-V/Xtensa ELF、正确 SDK port、没有原 `tasks.c`、全部必需标记、退出码、镜像哈希及开始/结束源码身份。正常/回归镜像分开，C3 不链接 OTG 后端；完整验收覆盖公共代码与原有平台。官方模拟器未实现的 RF/USB 及真实外设不会计为通过。
