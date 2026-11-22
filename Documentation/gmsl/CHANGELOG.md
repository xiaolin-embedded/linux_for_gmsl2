# 修改记录

## 2026-11-22

### SG-2M-120A-820-5300-G2 2MP 支持

新增 2MP GMSL 模组支持：

- 实测输出 1920x1080 UYVY，MAX9295A（0x91）、ISP 0x6d、6Gbps；
- 新增 `sg2m_820_desc`、`max96724_sg2m820_board` 和
  `max96724-sg2m820` overlay，CFE 912Mbps；
- 该模组不能使用 AR0820 4K overlay；4K 配置只能收到少量帧后停流；
- 模组依赖 FSYNC：只跑原有 frame-sync 序列时约 3 帧后停止，
  `skip_frame_sync=true` 时完全不启动。

新增 board `fsync_init` 回调，开流前自动配置：

- MAX96724 内部 30Hz FSYNC：
  `0x04a0=0x00`、`0x04a2=0x00`、`0x04a5=0x35`、
  `0x04a6=0xb7`、`0x04a7=0x0c`、`0x04af=0xcf`、
  `0x04b1=0x40`；
- 对应 link 的 FSYNC TX enable：
  `0x0b08 + link*0x100 = 0x10`；
- MAX9295A MFP8：`0x02d6=0x84`、`0x02d7=0xa0`、
  `0x02d8=0x08`。

上板结果：

- Link A，`gmsl_links=1 0 0 0`；
- 1920x1080，30 帧连续抓图通过；
- 第二次 start/stop 再抓 30 帧仍通过；
- 停止手工 `gmsl-reg` 配置后，驱动开流时自动恢复 FSYNC。

变更文件：

```text
drivers/media/i2c/gmsl-core.c
drivers/media/i2c/gmsl-core.h
drivers/media/i2c/gmsl-desc.c
drivers/media/i2c/gmsl-desc.h
drivers/media/i2c/gmsl-v4l2.c
arch/arm/boot/dts/overlays/Makefile
arch/arm/boot/dts/overlays/max96724-sg2m820-overlay.dts
Documentation/gmsl/CHANGELOG.md
Documentation/gmsl/GMSL_RPI5_COMPLETE_GUIDE_CN.md
```

### 8MP AR0820 120A/030A 两颗模组实测

本轮又验证了两颗 8MP AR0820 变体，二者都可以复用现有
`max96724-ar0820` board/overlay，无需新增驱动代码：

| 模组 | 视场 | 输出 | Serializer / ISP | CFE |
|---|---|---|---|---|
| `SG-8M-120A-820-5300-G2` | 120° 广角 | 3840x2160 UYVY | MAX9295A `0x91` / `0x6d` | 1600 Mbps |
| `SG-8M-030A-0820-5300-G2` | 30° 窄视场 | 3840x2160 UYVY | MAX9295A `0x91` / `0x6d` | 1600 Mbps |

共同结果：

- Link A，`gmsl_links=1 0 0 0`；
- EEPROM `0x51` 首字节为 `0x17`；
- 冷启动抓图 10 帧成功；
- PNG 均为 3840x2160；
- CFE 会打印 `DPHY: Datarate 1600 Mbps out of range`，但本次抓图成功；
- AR0820 4K 重复 start/stop 的限制仍然存在，本次只验证冷启动单次抓图。

分模组结果：

- `SG-8M-120A-820-5300-G2`：120° 广角，PNG 15MB，
  `sg8m-cold.png`；
- `SG-8M-030A-0820-5300-G2`：约 30° 窄视场，PNG 9.2MB，
  `sg8m030.png`，画面视角明显更窄。

配置：

```text
dtoverlay=max96724-ar0820
```

结论：这两颗模组不需要像 SG2M-820 那样增加内部 FSYNC 配置；直接使用
AR0820 4K 管线即可。型号中的 `820` 与 `0820` 写法不同，但实测都走
MAX9295A `0x91` + ISP `0x6d` 的 8MP 路径。

## 2026-11-21

### ISX031 3Gbps 模组支持

新增 SG-3M-190A-AA-031（Sony ISX031 + MAX96717F）支持：

- `gmsl-isx031.c/.h`：1920x1536 UYVY，CSI DT `0x1e`；
  - MAX96717F pass-through I2C 使能，sensor 7-bit 地址 `0x1a`；
  - MFP4 输出 24MHz sensor INCK，MFP0 释放 sensor reset；
  - ISX031 内部同步初始化（`0x8AF0=0`），4-lane 30fps 模式
    `0x8A00=0x17`，`0x8A01=0x80` 开流；
- `max96724_isx031_board`：Link A、3Gbps、`phy_clk=0x25`、跳过外部
  frame-sync GPIO 序列；
- `gmsl-core` 增加 `three_gbps` 和 `skip_frame_sync` board 标志；
- `gmsl-max96724` 在 3Gbps 时先写 `0x0010/0x0011=0x11`、打开 link
  `0x0006=0xff`，再用 `0x0018=0x0f` 触发 oneshot，最后重新读取 lock；
- overlay `max96724-isx031`，link-frequencies 288 MHz（CFE 576 Mbps）。

调试要点：

- 该模组 link 一直不锁的根因是 MAX96724 默认 6Gbps，而 MAX96717F 最高
  3Gbps；仅在 3Gbps 且 link enable 打开后 `0x001a` bit3 才置 1。
- CFE 576 Mbps 下，原 PHY clock `0x30` 无法完成帧；改为 `0x25`
  （`0x415/0x418/0x41b/0x41e`）后正常出图。
- sensor 位于 MAX96717F pass-through I2C，probe 时首次访问需要重试；
  已在 sensor 写入路径加入 5 次重试。

上板结果（2026-11-21，Link A）：

- `gmsl_links=1 0 0 0`，MAX96717F 迁移到 `0x41`，sensor `0x1a` ACK；
- `0x7DC8` 帧计数持续递增，serializer `0x0112` PCLK=1，
  deserializer `0x1dc` video_lock=1；
- 1920x1536 UYVY，30 帧抓图成功；PNG 正常，无丢帧错误。

变更文件：

```text
drivers/media/i2c/gmsl-isx031.c
drivers/media/i2c/gmsl-isx031.h
drivers/media/i2c/gmsl-core.c
drivers/media/i2c/gmsl-core.h
drivers/media/i2c/gmsl-desc.c
drivers/media/i2c/gmsl-desc.h
drivers/media/i2c/gmsl-max96724.c
drivers/media/i2c/gmsl-v4l2.c
drivers/media/i2c/Makefile
arch/arm/boot/dts/overlays/Makefile
arch/arm/boot/dts/overlays/max96724-isx031-overlay.dts
Documentation/gmsl/CHANGELOG.md
```

### 双 IMX490 双路验证

两颗 IMX490（serializer 0x41 和 0x62 各一颗）同时接 Link A/B：

- `gmsl_links = 1 1 0 0`，serializer 地址 0x41/0x42，ISP 0x6d ACK；
- 顺序 A→B 抓图均成功；
- 同时启动 csi2_ch0/ch1，各抓 10 帧成功（两个 video 节点均返回 0）；
- 2880x1860 UYVY，两个相机均可出图；相机 90° 安装，PNG 方向需应用旋转。

跨 link 失败根因：启动某一路时 `enable_pipes` 只写当前 link 的 mask，会把另一
路的 MAX96724 pipe 关闭；远端 ISP 停流后，再使能 pipe 也不能恢复。修复为：

- 启动任一路时，pipe mask 使用所有 locked link；
- per-link stop 不再关闭 pipe；
- 抓图脚本在 enable 目标 csi2 channel 前先 disable 所有 csi2→ch link，避免
  CFE 保留 enabled-but-not-streaming 的旧 channel。

注意：AR0820 4K 的 stop/start 仍受 CFE/DMA 重启问题限制，和本次双路修复
不是同一个问题。

### IMX490 模组支持

新增 SG5-IMX490C 系列支持：

- `gmsl-imx490.c/.h`：2880x1860 UYVY，CSI DT `0x1e`；
- `max96724_imx490_board`：PHY clock `0x32`，Link A-D 均可挂 IMX490，
  加串器走 0x91/0xbf/0xc8 自动识别；
- overlay `max96724-imx490`，link-frequencies 456 MHz；
- `gmsl-capture.sh -s 2880x1860` 可直接抓 IMX490。

编译状态：`W=1` 通过。

上板状态（2026-11-21）：通过，但不同模组地址/EEPROM 不同。

- 早期“完全枚举不到”的根因：部分模组的 MAX9295A 出厂地址是 **0x62**，
  驱动原来只探测 0x40/0x41，因此 probe 失败并把 Link A 关闭。
- 修复：`max9295`/`max96717` 和自动探测增加 0x62；发现后迁移到
  `0x41 + link`，若模组不接受迁移则保留 0x62。
- 实测当前 0x62 模组：serializer ID `0x91`，迁移到 0x41 后 ISP `0x6d` ACK；
  EEPROM `0x51` 无响应，按输出判断为 IMX490 5400 系列。
- 2880x1860 UYVY 基线抓图成功：30 帧约 14 fps，PNG 正常；
  相机为 90° 安装，`-r` 只做 180°，图像方向需要在应用侧再旋转。
- 另一颗 IMX490（serializer 0x41、EEPROM 0x32）可正常 2880x1860，
  30 帧约 26.4 fps；两颗模组都能出图。

变更文件：

```text
drivers/media/i2c/gmsl-imx490.c
drivers/media/i2c/gmsl-imx490.h
drivers/media/i2c/gmsl-core.h
drivers/media/i2c/gmsl-max96724.c
drivers/media/i2c/gmsl-desc.c
drivers/media/i2c/gmsl-desc.h
drivers/media/i2c/gmsl-v4l2.c
drivers/media/i2c/Makefile
arch/arm/boot/dts/overlays/Makefile
arch/arm/boot/dts/overlays/max96724-imx490-overlay.dts
GMSL_MAX96724_IMX390.md
Documentation/gmsl/CHANGELOG.md
```

### AR0820 模组支持

新增 SG8-AR0820C 相机支持：

- `gmsl-ar0820.c/.h`：AR0820 sensor ops 和 3840x2160 UYVY desc；
- `gmsl-max96717.c/.h`：MAX96717/96717F serializer ops（ID `0xbf`/`0xc8`，
  `0x0314/0x0318`、`0x0311/0x0308/0x0002`、4-lane 和 GPIO frame sync）；
- `gmsl-ser-auto.c/.h`：根据 0x000d DEV_ID 自动选择 MAX9295A 或 96717F；
- `max96724_ar0820_board` 和新 overlay `max96724-ar0820`；
- `gmsl-capture.sh` 增加 `-s WxH`，AR0820 可用 `-s 3840x2160` 抓 4K。

确认要点：SG8S-AR0820C-5300-G2A 数据手册和厂商驱动使用 **MAX9295A**
（DEV_ID `0x91`），不是 96717F；AR0820 在 GW5300 ISP 后面，host 不直接
配置 sensor。自动识别仍保留 96717/96717F 路径，便于同系列模组。

上板结果（2026-11-21）：

- 实际模组：serializer `0x41` DEV_ID 实测 `0x91`（MAX9295A），EEPROM reg0
  由 `0x30` 变为 `0x17`；不是 96717F。
- Link A 锁定，overlay `max96724-ar0820` 使用 800 MHz（D-PHY 1600 Mbps），
  CFE 日志为 `Using a link rate of 1600 Mbps`。
- 冷启动后单次 4K 抓图成功：3840x2160 UYVY，10 帧 / 30 帧均能生成 PNG；
  912 Mbps 下观察到约 25 fps。
- 已知限制：停止后再次启动，第二次只收到约 4 个 buffer（约 4 帧）就停住。
  912/1600 Mbps、keep pipe、keep CSI RX、清 `job_queued` 都未解决；判断为
  CFE/DMA 的 4K stop/start 重启问题，尚需继续修。单次冷启动抓图可用。

变更文件：

```text
drivers/media/i2c/gmsl-ar0820.c
drivers/media/i2c/gmsl-ar0820.h
drivers/media/i2c/gmsl-max96717.c
drivers/media/i2c/gmsl-max96717.h
drivers/media/i2c/gmsl-ser-auto.c
drivers/media/i2c/gmsl-ser-auto.h
drivers/media/i2c/gmsl-desc.c
drivers/media/i2c/gmsl-desc.h
drivers/media/i2c/gmsl-v4l2.c
drivers/media/i2c/Makefile
arch/arm/boot/dts/overlays/Makefile
arch/arm/boot/dts/overlays/max96724-ar0820-overlay.dts
tools/gmsl/gmsl-capture.sh
GMSL_MAX96724_IMX390.md
Documentation/gmsl/CHANGELOG.md
```

### P0：多路 link streaming 控制

- core 的 `gmsl_core_s_stream()` 改为接受 `link_mask`，只启停 mask 内且已锁定
  的 link；新增 `streaming_mask`，重复 enable/disable 不再重复操作已在线 link；
- 单 link probe/sensor init 失败只清除该 link 的 lock bit，不再让整个 probe
  失败；
- V4L2 增加 pad `enable_streams/disable_streams`（非 `FL_STREAMS`，每个 pad
  视为 stream 0），legacy `s_stream` 保留并按 active/locked link 推导 mask；
- `rp1_cfe` csi2 直通路径改为按 `node->id` 调 pad streams；首个通道打开 CSI
  RX，最后一个通道关闭，单路停止不关闭其他通道；
- MAX96724 pipe enable 改为 mask 语义，停止某一路时写入剩余 link mask；
- FE 路径仍使用 legacy `s_stream`，不在本次改动范围内。

单 link 实测（Link A，当前只有一颗相机）：

- 120 帧 + 二次重启 60 帧，1920x1080 UYVY，约 28–29 fps；
- dmesg 为 `enable streams pad=0` / `disable streams pad=0`；
- 912 Mbps，`error: slave` 计数 0，抓图正立清晰；
- 未对 B/C/D 发 sensor start；4 路 mixed-link 独立启停和缺失 link 隔离
  仍需四相机板验证。

变更文件：

```text
drivers/media/i2c/gmsl-core.c
drivers/media/i2c/gmsl-core.h
drivers/media/i2c/gmsl-max96724.c
drivers/media/i2c/gmsl-v4l2.c
drivers/media/platform/raspberrypi/rp1_cfe/cfe.c
Documentation/gmsl/ARCHITECTURE.md
Documentation/gmsl/CHANGELOG.md
GMSL_MAX96724_IMX390.md
```

### Phase 2：board/link/sensor 描述符拆分

将 Phase 1 的“一个 sensor desc 服务所有 link”改为板级/链路级/传感器级描述符：

- `gmsl_board_desc`：板级 compatible、deser ops、已配置 link 数；
- `gmsl_link_desc`：`index`（0..3 = A..D = pad = VC）、sensor desc、ser ops、
  sensor 地址、lane 数和 link freq；
- `gmsl_sensor_desc`：只保留分辨率、格式、CSI DT、sensor ops 和寄存器序列；
- `gmsl_core_set_board()`：按 `link_desc.index` 稀疏填充 `gmsl->links[]`，
  允许只配置 Link A/C 等组合，并拒绝越界、重复 index 和不完整描述符；
- 未使用的 link 继承首个已配置 link 的默认格式，4 个 source pad 始终对 CFE
  报告合法格式；
- `set_fmt` 改为 per-link，不再修改所有 link；
- IMX390 sensor ops 改用 `link->sensor_addr` / `link->link_freq`；
- probe 的 match data 从 sensor desc 改为 board desc，模块名/compatible/overlay
  保持不变。

### 用户态寄存器工具 gmsl-reg

新增 `tools/gmsl/gmsl-reg.c` 和 `tools/gmsl/README.md`，类似 `cci_debug`，
不依赖 `i2c-tools`，通过 `/dev/i2c-N` 提供：

- `detect`：探测解串器、加串器、ISP、EEPROM、sensor；
- `scan [start] [end]`：扫描 I2C 地址；
- `read/write/dump`：16 位寄存器地址、8 位数据，支持连续读取和 dump；
- `links`：显示 GMSL link lock 和 `0x0006` 路由掩码；
- 交互 shell。

板级确认的 SG2-IMX390C 拓扑：

```text
IMX390(0x21) -> GW5200 ISP(0x6d) -> MAX9295A(0x41) -> MAX96724(0x27)
EEPROM: 0x51
```

`detect` 实测可访问 `0x27(id=0x4e)`、`0x41(id=0x91)`、`0x6d`、`0x51`；
IMX390 的 `0x21` 被 GW5200 挡在本地 I2C 后面，默认不能直读。工具保留
`sensor` 别名供直连 sensor 的模组使用，并在 README 中记录该限制。

### 上板验证

- 首次启动：Link A 锁定，`gmsl_links = 1 0 0 0`，serializer detected。
- 首次抓图失败：`VIDIOC_STREAMON` 返回 `Broken pipe`，dmesg 为
  `Failed to start media pipeline: -32`。根因是 Phase 2 下未使用的 sensor pad
  1..3 报告 `unknown/0x0`，CFE 启动 pipeline 时校验失败；让未使用 link 继承
  默认格式后恢复。同时 `enum_mbus_code` 改为按 pad 返回，避免返回 0 格式。
- 最终测试（120 帧）：
  - `gmsl_links = 1 0 0 0`
  - CFE `Using a link rate of 912 Mbps`
  - 1920x1080 UYVY，约 28–29 fps
  - `error: slave` 计数 0
  - 抓图 PNG 正立、画面清晰

### 变更文件

```text
drivers/media/i2c/gmsl-core.c
drivers/media/i2c/gmsl-core.h
drivers/media/i2c/gmsl-desc.c
drivers/media/i2c/gmsl-desc.h
drivers/media/i2c/gmsl-imx390.c
drivers/media/i2c/gmsl-v4l2.c
Documentation/gmsl/ARCHITECTURE.md
Documentation/gmsl/CHANGELOG.md
GMSL_MAX96724_IMX390.md
tools/gmsl/Makefile
tools/gmsl/gmsl-reg.c
tools/gmsl/README.md
```

## 2026-11-17

### Phase 1：GMSL 分层框架

将单体 `max96724_imx390.c` 拆分为“V4L2 适配层 + core + 每芯片 ops + desc”：

- `gmsl-core.c/.h`：`gmsl_dev`/`gmsl_link`、I2C 地址切换/重试、
  `gmsl_load_regs`、deser/ser/sensor ops 调度、descriptor 初始化。
- `gmsl-v4l2.c`：4-pad V4L2 subdev，fmt/frame_desc/mbus_config/s_stream。
- `gmsl-max96724.c/.h`：MAX96724 ops（reset/link lock/monopolize/pipe/VC/
  enable pipes）。
- `gmsl-max9295.c/.h`：MAX9295A ops（地址、link、GPIO frame sync）。
- `gmsl-imx390.c/.h`：IMX390 sensor ops（init/start/stop/fmt/mbus）。
- `gmsl-desc.c/.h`：sensor descriptor 表。

保持不变：

- 模块名 `max96724_imx390.ko`（复合模块，源文件已拆分）
- compatible `zuojisi,max96724-imx390`
- overlay、config.txt、`gmsl_links` sysfs
- 所有寄存器序列、延时、link lock/VC 映射和 30fps 行为

验证：

- `W=1` 编译通过，拆分文件无新增 warning。
- 上板 golden test 已通过：
  - Link A 锁定，`gmsl_links = 1 0 0 0`
  - CFE 使用 912 Mbps
  - 60 帧 1920x1080 UYVY，约 29.95 fps
  - 无 `error: slave`
  - 图像正常（`gmsl-capture.sh -r` 生成的正立 PNG）
- 测试方式：备份 master 模块后，安装分层模块并重启；boot probe 成功。
- 新模块产物：
  `rpi-6.18/framework-build/drivers/media/i2c/max96724_imx390.ko`。

## 2026-11-15

### 1. 背景与基线

厂商提供的原始资料是 Raspberry Pi `rpi-6.6.y`（6.6.78）内核树，里面包含
一整套 `z_xcdes*` 私有驱动和 overlay。本次只保留 SG2-IMX390C-5200-GMSL2
这一路相机，移植到：

- `raspberrypi/linux` 分支 `rpi-6.18.y`
- 基线 commit `ea17ae02ca28483e508b05c7c44190c01a1ef8eb`
- Makefile 版本 6.18.62
- 目标板 Raspberry Pi 5 Model B Rev 1.1

厂商驱动是“解串器 + 串行器 + 相机”一体的单体 V4L2 subdev 驱动，不是通用的
MAX96724/MAX9295 驱动。

### 2. 移植到 6.18 的 V4L2 API 适配

原始 6.6 驱动在新内核上不能直接编译，做了以下适配：

| 6.6 写法 | 6.18 写法 |
|---|---|
| `v4l2_subdev_get_try_format(sd, state, pad)` | `v4l2_subdev_state_get_format(state, pad)` |
| `v4l2_subdev_get_try_crop(sd, state, pad)` | `v4l2_subdev_state_get_crop(state, pad)` |
| `pad_ops.init_cfg` | `internal_ops.init_state`，并在 probe 设置 `sd->internal_ops` |
| `video_ops.g_frame_interval` / `s_frame_interval` | `pad_ops.get_frame_interval` / `set_frame_interval`，回调增加 state 参数 |
| 无 | 增加 `v4l2_subdev_init_finalize()` / `v4l2_subdev_cleanup()` |

同时保留/增加了：

- `v4l2_async_register_subdev_sensor()`
- 4 个 source pad，pad 号 = GMSL link 号 = CSI2 VC 号
- `get_frame_desc()` 返回 `DT=0x1e, VC=pad`
- CFE downstream 驱动的 `CSI2_CH1` 从 metadata 改成 video capture 节点，
  这样 Link A/B/C/D 都能对应到 `csi2_ch0..3`

### 3. 驱动重命名

原名字 `z_xcdes40_sg2_imx390c_5200_gmsl2` 过于厂商化，改为：

| 项目 | 新名字 |
|---|---|
| 源文件 | `drivers/media/i2c/max96724_imx390.c` |
| 模块 | `max96724_imx390.ko` |
| overlay | `arch/arm/boot/dts/overlays/max96724-imx390-overlay.dts` |
| dtbo | `max96724-imx390.dtbo` |
| config.txt | `dtoverlay=max96724-imx390` |
| compatible | `zuojisi,max96724-imx390` |
| sysfs | `/sys/bus/i2c/devices/*/gmsl_links` |
| 日志前缀 | `max96724-imx390:` |
| 模块描述 | `MAX96724 + MAX9295A + IMX390 GMSL2 camera driver` |

内部 `imx390_5200_*` 前缀统一改为 `imx390_*`。

### 4. 问题修复

#### 4.1 `set_fmt()` 重复 `mutex_unlock()`

厂商模板在非 TRY 分支先 unlock 一次，`out:` 标签又 unlock 一次。已删除
多余的那一次。

#### 4.2 serializer 地址写 NACK

原逻辑每路都先往 serializer 默认地址 0x40 写新地址。如果 serializer 在上次
启动时已经被改到 0x41，重启后再写 0x40 会 NACK：

```text
imx390_write_reg: error: slave=40, reg=0, val=82
```

改为先用静默读探测 0x40：

- 只有 ID 仍是 0x91 时才写新地址；
- 如果 0x40 不在，直接去目标地址 0x41 读 ID；
- 探测失败不打印错误。

重复启动不再出现该 NACK。

#### 4.3 CFE 一直回退 999 Mbps

原驱动用 `v4l2_fwnode_endpoint_parse()`，这个函数不会解析
`link-frequencies`。改为：

- `v4l2_fwnode_endpoint_alloc_parse()`；
- 把 bus type、data lanes、flags、link frequency 存到
  `sensor->mbus_config`；
- 实现 `get_mbus_config()` pad op；
- 解析完成后 `v4l2_fwnode_endpoint_free()`。

现在 CFE 日志为：

```text
rp1-cfe 1f00128000.csi: Using a link rate of 912 Mbps
```

不再出现：

```text
Unable to determine sensor link rate, using 999 Mbps
```

#### 4.4 日志降噪

以下日志从 `dev_info` 改为 `dev_dbg`：

- 每寄存器 I2C 读写
- `get_fmt` / `set_fmt`
- `enum_frame_size` / `enum_frame_interval`
- `get_frame_interval` / `set_frame_interval`
- sysfs `gmsl_links` 读取

probe、link lock、`s_stream` 等关键日志保留 `dev_info`。

#### 4.5 `check_version()` 失败路径恢复 I2C 地址

`check_version()` 失败时直接返回，`client->addr` 会停留在 0x27。已补上
`client->addr = addrbackup`。

#### 4.6 I2C 瞬态 NACK 重试

`write_reg` / `read_reg` 增加 3 次重试 + 1~2ms 延时。3 次都失败才打印
`error: slave=...`。这样偶发总线 NACK 不会污染日志。

#### 4.7 overlay 去掉无效的 180° 属性

overlay 里的 `rotation = <180>` / `orientation = <2>` 驱动从未实现，属于
无效属性，已删除。相机模组物理安装是倒置的，需要正立画面时用抓图脚本的
`-r`（PNG 做 hflip+vflip），或在应用里软件旋转；raw 数据保持 sensor 原始
方向。

### 5. 抓图脚本

新增 `tools/gmsl/gmsl-capture.sh`，一条命令完成：

- 自动从 `gmsl_links` 识别 Link A/B/C/D；
- 自动遍历 `/dev/media*` 找 `driver rp1-cfe`；
- 自动从 media topology 找 `rp1-cfe-csi2_chN` 对应的 `/dev/videoN`；
- 使能 `"csi2":4+N -> "rp1-cfe-csi2_chN":0[1]`；
- 设置 4 个 csi2 sink pad 为 `UYVY8_1X16/1920x1080 field:none`；
- 设置视频节点 UYVY 1920x1080；
- 抓帧、统计最后一帧 Y/U/V；
- 可选生成 180° 旋转后的 PNG。

用法：

```bash
./tools/gmsl/gmsl-capture.sh -r
./tools/gmsl/gmsl-capture.sh -l 0 -n 120 -o cap.raw -p cap.png -r
```

### 6. 验证结果

硬件：RPi 5 Model B Rev 1.1 + MAX96724 + MAX9295A + SG2-IMX390C-5200-GMSL2，
相机接 Port 0 / Link A。

```text
gmsl_links = 1 0 0 0
link lock status = 0x01
serializer detected
probe success!

link:  0
media: /dev/media2
video: /dev/video0
captured: 60 frames (248832000 bytes)
Y min=0 max=255 mean=86.50
U min=50 max=185 mean=131.03
V min=74 max=179 mean=124.51
png: gmsl-test.png (rotated 180)
```

- 1920x1080 UYVY，约 28 fps
- CFE link rate 912 Mbps
- 无 `error: slave`
- `get_fmt` 日志数量 0
- `-r` 输出已确认正立

### 7. 变更文件清单

最终 patch 包含 6 个文件、1360 行新增：

```text
arch/arm/boot/dts/overlays/Makefile
arch/arm/boot/dts/overlays/max96724-imx390-overlay.dts
drivers/media/i2c/Makefile
drivers/media/i2c/max96724_imx390.c
drivers/media/platform/raspberrypi/rp1_cfe/cfe.c
tools/gmsl/gmsl-capture.sh
```

### 8. 已知限制

- 使用 downstream CFE `rp1_cfe`，DT compatible 为 `raspberrypi,rp1-cfe`；
  没有改 upstream `rp1-cfe`。
- media 设备编号每次启动可能变化，脚本已自动查找；手工命令不要写死
  `media2`。
- 驱动是 SG2-IMX390C-5200-GMSL2 专用单体驱动，分辨率/格式/30fps 和寄存器
  序列硬编码；换模组需要改驱动。
- 没有实现曝光/增益/翻转 V4L2 控制；模组 AE 自动工作。
- serializer 的 0x007b~0x00ab 寄存器组在 Link A 上回读仍是默认值
  （0x10/0x00），说明这组写在该模组上可能是只读/保留的；实际视频路由由
  MAX96724 的 pipe/VC 寄存器和 serializer 的 0x0318/0x0311/0x0331 完成。
- 手动安装单独编译的模块时可能出现
  `no symbol version for module_layout`；完整 `make modules` 构建不会出现。
