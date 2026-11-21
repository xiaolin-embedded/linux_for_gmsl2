# gmsl-reg

`gmsl-reg` 是 GMSL2 链路的用户态寄存器读写工具，类似 `cci_debug`。它直接通过
`/dev/i2c-N` 访问解串器、加串器和相机模组上的 I2C 设备。

## SG2-IMX390C 拓扑

根据模组数据手册，SG2-IMX390C-5200-GMSL2 的链路是：

```text
IMX390 (0x21) -> GW5200 ISP (0x6d) -> MAX9295A (0x41) -> GMSL2 -> MAX96724 (0x27)
```

| 目标 | 7-bit 地址 | 数据手册 8-bit | 说明 |
|---|---|---|---|
| 解串器 | `0x27` | - | MAX96724 |
| 加串器 | `0x41 + link` | `0x80` | MAX9295A；驱动 probe 后 Link A 为 `0x41` |
| ISP | `0x6d` | `0xDA` | GW5200，相机侧控制/ISP |
| EEPROM | `0x51` | `0xA2` | 模组信息 EEPROM |
| Sensor | `0x21` | `0x42` | IMX390；**该模组上被 GW5200 挡住，不能直读** |

MAX96724 会把非自身地址的 I2C 事务转发到当前使能的 GMSL link。当前单相机配置
下，驱动 probe 后 Link A 保持使能，所以 `0x27 / 0x41 / 0x6d / 0x51` 在同一条
I2C 总线上可见。IMX390 在 GW5200 的本地 I2C 总线上，默认不会出现在这条总线上；
需要 ISP 厂商提供 pass-through 接口或寄存器说明后才能访问。工具保留 `sensor`
别名（`0x21`），在直连 sensor 的模组上可直接使用。

## 构建

在 Pi 上直接编译：

```bash
cd tools/gmsl
make
sudo make install
```

交叉编译：

```bash
make CC=aarch64-linux-gnu-gcc
```

工具只依赖 Linux `i2c-dev`，不需要 `i2c-tools`。运行用户需要在 `i2c` 组中，
或者使用 `sudo`。

## 使用

```bash
# 自动从 sysfs 找到 max96724 驱动所在总线，探测所有可见设备
gmsl-reg detect

# 扫描总线（默认 0x08..0x77）
gmsl-reg scan
gmsl-reg scan 0x20 0x70

# 读寄存器
gmsl-reg read des 0x0000
gmsl-reg read ser 0x000d
gmsl-reg read isp 0x0000 8
gmsl-reg read sensor 0x0000

# 写寄存器（会影响运行中的链路，确认后再执行）
gmsl-reg write des 0x0006 0xf1
gmsl-reg write ser 0x0318 0x5e

# dump 16 字节；结束地址可选
gmsl-reg dump des 0x0000 0x003f
gmsl-reg dump ser 0x0300 0x033f
gmsl-reg dump eeprom 0x0000 0x003f

# 查看 link lock 和 0x0006 路由掩码
gmsl-reg links

# 交互模式
gmsl-reg -i
```

常用选项：

```text
-b, --bus N        指定 I2C 总线；默认从 sysfs 驱动名自动识别
-l, --link N       选择 link 0..3，仅影响 ser 别名地址（默认 0）
-g, --reg-bits N   8 或 16 位寄存器地址（默认 16）
-t, --retries N    NACK 重试次数（默认 2）
-i, --interactive  交互 shell
```

`ser` 在驱动 probe 前可能仍在 `0x40`，此时可直接用原始地址：

```bash
gmsl-reg read 0x40 0x000d
```

## 注意事项

- 工具绕过驱动的互斥和 runtime PM，**写寄存器前先停止出流**；读操作一般
  安全，但会与驱动访问共享总线。
- 当前板子的 `sensor @0x21` 预期会 NACK，这是模组拓扑决定的，不是链路故障：
  IMX390 在 GW5200 ISP 后面。可用 `isp` 目标访问 GW5200。
- `-l` 只改变 `ser` 别名地址，不修改解串器的 I2C 转发路由。当前单相机板
  验证的是 Link A；多 link 同时访问时，需要后续增加安全的 link
  monopolize/debugfs 接口。
- 遇到 `NACK`/`Remote I/O error` 时，先确认 `gmsl-reg links` 显示目标 link
  locked，再确认模块供电和地址。
- GW5200 的寄存器语义需要厂商资料；工具只保证 I2C 读写通道可用。

## 后续增强

- 增加 `link A|B|C|D` 命令，安全地切换 MAX96724 的 I2C 转发目标；
- 增加寄存器批量读写和位域修改（read-modify-write）；
- 驱动侧提供 debugfs 后端，由驱动处理 runtime PM、link 路由和并发访问，
  工具只做薄封装；
- 如果 GW5200 提供 sensor pass-through，增加 `sensor` 的 ISP 转发命令。
