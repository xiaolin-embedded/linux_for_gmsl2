# RPi 5 + MAX96724 + IMX390 GMSL2 camera support

这是基于 `raspberrypi/linux` `rpi-6.18.y` 的完整内核 fork，用于
Raspberry Pi 5 + MAX96724 四路 GMSL2 解串器，支持 SG2-IMX390C 和
SG8-AR0820C 两种相机模组。

## 分支说明

- `master`（默认，唯一开发分支）：包含 IMX390/AR0820 驱动、Phase 1/2
  分层框架、board/link 描述符、per-link streaming 和 `gmsl-reg` 工具。
  后续修改直接提交到 master。
- `vendor-full-port`：厂商 14 个 `z_xcdes*` 驱动的完整移植版本，保留作历史
  参考；本次 IMX390 最小版本不依赖它。

## 基线

- 仓库：`raspberrypi/linux`
- 分支：`rpi-6.18.y`
- 基线 commit：`ea17ae02ca28483e508b05c7c44190c01a1ef8eb`
- 内核版本：6.18.62
- 目标板：Raspberry Pi 5 Model B Rev 1.1
- 测试内核：`6.18.62-v8-16k+`

## 支持模组

| 模组 | Sensor | ISP | 加串器 | 分辨率 | overlay |
|---|---|---|---|---|---|
| SG2-IMX390C-5200-GMSL2 | IMX390 | GW5200 | MAX9295A | 1920x1080 UYVY | `max96724-imx390` |
| SG8-AR0820C-5300/5200-G2A | AR0820 | GW5300/5200 | MAX9295A（实测 0x91；96717F 自动识别） | 3840x2160 UYVY | `max96724-ar0820` |
| SG5-IMX490C-5300/5400-G2A | IMX490 | GW5300/5400 | MAX9295A，默认可能在 0x40/0x41/0x62 | 2880x1860 UYVY | `max96724-imx490` |

加串器支持运行时自动识别：MAX9295A (`0x91`)、MAX96717 (`0xbf`)、
MAX96717F (`0xc8`)。因此同一 overlay 也可用于 96717/96717F 版本的模组。
两颗 Sensor 都在 ISP 后面，host 侧只控制 ISP + SerDes，不直接配置 Sensor。

## 关键文件

```text
drivers/media/i2c/gmsl-core.c/.h
drivers/media/i2c/gmsl-v4l2.c
drivers/media/i2c/gmsl-max96724.c/.h
drivers/media/i2c/gmsl-max9295.c/.h
drivers/media/i2c/gmsl-max96717.c/.h
drivers/media/i2c/gmsl-ser-auto.c/.h
drivers/media/i2c/gmsl-imx390.c/.h
drivers/media/i2c/gmsl-ar0820.c/.h
drivers/media/i2c/gmsl-imx490.c/.h
drivers/media/i2c/gmsl-desc.c/.h
drivers/media/platform/raspberrypi/rp1_cfe/cfe.c
arch/arm/boot/dts/overlays/max96724-imx390-overlay.dts
arch/arm/boot/dts/overlays/max96724-ar0820-overlay.dts
arch/arm/boot/dts/overlays/max96724-imx490-overlay.dts
tools/gmsl/gmsl-capture.sh
tools/gmsl/gmsl-reg.c
tools/gmsl/README.md
Documentation/gmsl/CHANGELOG.md
Documentation/gmsl/patches/rpi-6.18.y-max96724-imx390.patch
Documentation/gmsl/patches/rpi-6.18.y-max96724-imx390.diff
Documentation/gmsl/config.txt.example
```

## 分层结构

- `gmsl-core.c/.h`：`gmsl_dev`/`gmsl_link` 状态、I2C 地址切换与重试、
  `gmsl_load_regs`、deser/ser/sensor ops 调度、`streaming_mask` 和逐 link
  启停；单 link 失败隔离。
- `gmsl-v4l2.c`：4-pad V4L2 subdev，fmt/frame_desc/mbus_config、
  legacy `s_stream` 和 per-pad `enable_streams/disable_streams`；对 CFE
  暴露的仍然是同一个 `max96724_imx390` sensor entity。
- `rp1_cfe/cfe.c`：csi2 直通路径按通道调用 sensor pad streams，首个通道
  打开 CSI RX、最后一个通道关闭。
- `gmsl-max96724.c/.h`：MAX96724 解串器 ops（reset/link lock/pipe/VC）。
- `gmsl-max9295.c/.h`：MAX9295A 串行器 ops（地址、link、GPIO frame sync）。
- `gmsl-max96717.c/.h`：MAX96717/96717F 串行器 ops（地址、0x0314/0x0318+
  pipe 配置、GPIO frame sync）。
- `gmsl-ser-auto.c/.h`：按 0x000d DEV_ID 自动选择 MAX9295A 或 MAX96717F。
- `gmsl-imx390.c/.h`：IMX390 sensor ops（init/start/stop/fmt/mbus）。
- `gmsl-ar0820.c/.h`：AR0820 sensor ops（3840x2160 UYVY）。
- `gmsl-desc.c/.h`：`board_desc + link_desc + sensor_desc` 表；新增模组先加
  desc + ops + DT。`max96724_imx390_board` 和 `max96724_ar0820_board`
  共用 MAX96724 deser ops。

Phase 1 完成 core/ops/desc 分层；Phase 2 完成 board/link/sensor 描述符拆分、
稀疏 link 映射和 per-link 格式；P0 完成 per-link streaming 控制。模块名
`max96724_imx390.ko`、compatible `zuojisi,max96724-imx390`、overlay、
config.txt 和寄存器序列保持不变。`W=1` 编译已通过；单 link 上板 golden
test 已通过：

- Link A 锁定，`gmsl_links = 1 0 0 0`
- CFE 使用 912 Mbps
- 120 帧 + 二次重启 60 帧，1920x1080 UYVY，约 28–29 fps
- dmesg：`enable streams pad=0` / `disable streams pad=0`
- 无 `error: slave`
- `gmsl-capture.sh -r` 生成的正立 PNG 正常

完整的 target 设计、SIPL/AIS 对标、验证矩阵和当前 gap 分析见
[Documentation/gmsl/ARCHITECTURE.md](Documentation/gmsl/ARCHITECTURE.md)。

## 构建

```bash
export KERNEL=kernel_2712
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- bcm2712_defconfig
make -j$(nproc) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules dtbs
```

## 部署

```bash
Image                         -> /boot/firmware/kernel_2712.img
arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dtb
                              -> /boot/firmware/
arch/arm64/boot/dts/overlays/max96724-imx390.dtbo
                              -> /boot/firmware/overlays/

sudo make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
    INSTALL_MOD_PATH=/目标rootfs modules_install
sudo depmod -a
```

`/boot/firmware/config.txt`：

```ini
camera_auto_detect=0

# SG2-IMX390C
dtoverlay=max96724-imx390

# 或 SG8-AR0820C
# dtoverlay=max96724-ar0820

# 或 SG5-IMX490C
# dtoverlay=max96724-imx490
```

如果相机接在 CAM/DISP0，加上 `,cam0`：

```ini
dtoverlay=max96724-imx390,cam0
dtoverlay=max96724-ar0820,cam0
```

重启后检查：

```bash
lsmod | grep max96724
dmesg | grep -i max96724
cat /sys/bus/i2c/devices/*/gmsl_links
```

正常输出：

```text
max96724-imx390: deserializer detected
max96724-imx390: link lock status=0x01
max96724-imx390: serializer detected
max96724-imx390: probe success!
```

`gmsl_links = 1 0 0 0` 表示 Link A 已锁定。

## 抓图

```bash
# 自动识别 Link / media / video，抓 60 帧并生成正立 PNG
./tools/gmsl/gmsl-capture.sh -r

# SG8-AR0820C：指定 4K 分辨率
./tools/gmsl/gmsl-capture.sh -s 3840x2160 -n 30 \
  -o /home/zaiwei/ar0820.raw -p /home/zaiwei/ar0820.png -r

# SG5-IMX490C：指定 2880x1860
./tools/gmsl/gmsl-capture.sh -s 2880x1860 -n 30 \
  -o /home/zaiwei/imx490.raw -p /home/zaiwei/imx490.png -r

# 指定 Link / 帧数 / 输出
./tools/gmsl/gmsl-capture.sh -l 0 -n 120 \
  -o /home/zaiwei/cap.raw -p /home/zaiwei/cap.png -r
```

脚本会自动：

- 从 `/sys/bus/i2c/devices/*/gmsl_links` 识别 Link A/B/C/D；
- 遍历 `/dev/media*` 找 `driver rp1-cfe`，避免硬编码 media 编号；
- 从 media topology 找 `rp1-cfe-csi2_chN` 对应的 `/dev/videoN`；
- 使能 `"csi2":4+N -> "rp1-cfe-csi2_chN":0[1]`；
- 按 `-s` 设置 4 个 csi2 sink pad 为 `UYVY8_1X16/WxH field:none`；
- 按 `-s` 设置视频节点 UYVY WxH（IMX390 默认 1920x1080，
  AR0820 使用 3840x2160）；
- 抓帧、统计最后一帧 Y/U/V，可选生成 180° 旋转后的 PNG。

依赖：`v4l-utils`，PNG/统计部分需要 `ffmpeg` 和 `python3`。

## 寄存器调试

`tools/gmsl/gmsl-reg.c` 是类似 `cci_debug` 的用户态 I2C 工具，不依赖
`i2c-tools`，可直接读写：

| 目标 | 地址 | 说明 |
|---|---|---|
| 解串器 | `0x27` | MAX96724 |
| 加串器 | `0x41 + link` | MAX9295A；驱动 probe 后 Link A 为 `0x41` |
| ISP | `0x6d` | GW5200（8-bit `0xDA`） |
| EEPROM | `0x51` | 模组 EEPROM（8-bit `0xA2`） |
| Sensor | `0x21` | IMX390（8-bit `0x42`）；被 GW5200 挡住，不能直读 |

SG2-IMX390C 的实际链路是
`IMX390(0x21) -> GW5200 ISP(0x6d) -> MAX9295A(0x41) -> MAX96724(0x27)`，
所以 IMX390 寄存器默认不在 GMSL I2C 总线上；工具保留 `sensor` 别名，供直连
sensor 的模组使用。

```bash
cd tools/gmsl && make
./gmsl-reg detect
./gmsl-reg scan
./gmsl-reg read des 0x0000
./gmsl-reg read ser 0x000d
./gmsl-reg dump ser 0x0300 0x033f
./gmsl-reg read isp 0x0000 8
./gmsl-reg dump eeprom 0x0000 0x003f
./gmsl-reg -i
```

写寄存器会绕过驱动互斥，出流时只建议读；详细说明见
[tools/gmsl/README.md](tools/gmsl/README.md)。

## 已验证结果

在 RPi 5 + MAX96724 + MAX9295A + SG2-IMX390C-5200-GMSL2 上实测：

```text
gmsl_links = 1 0 0 0
link lock status = 0x01

link:  0
media: /dev/media2
video: /dev/video0
captured: 60 frames (248832000 bytes)
Y min=0 max=255 mean=86.50
U min=50 max=185 mean=131.03
V min=74 max=179 mean=124.51
png: gmsl-test.png (rotated 180)
```

- 1920x1080 UYVY，约 28–29 fps
- CFE link rate：`Using a link rate of 912 Mbps`
- 无 `error: slave` 日志
- `get_fmt` 日志数量 0
- `-r` 输出已确认正立

P0 per-link streaming 实测（单相机）：

```text
enable streams pad=0
120 frames, 912 Mbps, ~29 fps
enable streams pad=0   # 二次启动
disable streams pad=0
```

- 只启动实际有效的 Link A，未对 B/C/D 发 sensor start；
- 停止后再次启动正常；
- 4 路 mixed-link 独立启停逻辑已实现，但当前只有一颗相机，尚未做四路硬件矩阵测试。

双 IMX490 实测（2026-11-21，Link A/B）：

```text
gmsl_links = 1 1 0 0
serializer: 0x41, 0x42
ISP @0x6d ACK
顺序 A→B 抓图成功
同时 csi2_ch0/ch1 各 10 帧成功
2880x1860 UYVY
```

两颗模组一颗 serializer 默认 0x62、一颗在 0x41；驱动自动迁移/分配地址。
跨 link 顺序抓图要求所有 locked link 的 pipe 保持使能，否则先启动的一路会
把另一颗相机的 ISP 停掉。

IMX490 实测（2026-11-21，Link A）：

```text
serializer @0x41 id=0x91
ISP @0x6d ACK
EEPROM @0x51 reg0=0x32, string HXxx-G03050520
CFE link rate: 912 Mbps
2880x1860 UYVY, 30 frames, ~26.4 fps
```

---

AR0820 4K 实测（2026-11-21，Link A）：

```text
serializer @0x41 id=0x91 (MAX9295A)
EEPROM @0x51 reg0=0x17
CFE link rate: 1600 Mbps
3840x2160 UYVY, single cold-boot capture OK
```

- overlay `max96724-ar0820` 的 `link-frequencies` 已改为 800 MHz；
- 单次冷启动抓图可以生成 4K PNG，912 Mbps 下约 25 fps；
- **已知限制**：4K 停止后再次启动，第二次只收到约 4 个 buffer 就停住。
  keep pipe、keep CSI RX、清 CFE `job_queued` 均未解决；问题定位在 CFE/DMA
  的 4K stop/start 重启路径，单次抓图/连续流可用，连续 start/stop 待修。

## 已知限制

- 使用 downstream CFE `rp1_cfe`（DT compatible `raspberrypi,rp1-cfe`），
  不是 upstream `rp1-cfe`。
- media 设备编号每次启动可能变化，脚本已自动查找；手工命令不要写死
  `media2`。
- 相机模组物理安装是 180° 倒置的；overlay 不再保留无效的
  `rotation`/`orientation` 属性。需要正立画面时用 `gmsl-capture.sh -r`，
  或在应用里做 `hflip+vflip`。
- 当前每个 board desc 只配置一路相机；Link 索引在 link desc 中固定为 A。
- AR0820 的 4K UYVY 链路率沿用厂商配置 456 MHz（912 Mbps）；如果实际帧率
  或带宽不足，需要再调整 overlay 的 `link-frequencies` 和 SerDes PHY 配置。
- 加串器自动识别支持 MAX9295A/MAX96717/MAX96717F；如果模组使用其他型号，
  需要新增 ser ops。
- 没有实现曝光/增益/翻转等 V4L2 控制，模组 AE 自动工作。
- SG2-IMX390C 的 IMX390 位于 GW5200 ISP 之后，默认不能通过 GMSL I2C 直接
  读写 sensor 寄存器；可访问 ISP `0x6d`，sensor 直读需要 ISP 厂商提供
  pass-through/寄存器资料。
- serializer 的 0x007b~0x00ab 寄存器组在 Link A 上回读仍是默认值
  （0x10/0x00），说明这组写在该模组上可能是只读/保留的；实际视频路由由
  MAX96724 的 pipe/VC 寄存器和 serializer 的 0x0318/0x0311/0x0331 完成。
- 双 IMX490 的 Link A/B 同时抓图已通过；单路停止/重启和 4 路 mixed-link
  （IMX490 + AR0820）仍需继续验证。
- 相机模组可能为 90° 安装，`-r` 只做 180°；需要应用侧再旋转。
- AR0820 4K 单次冷启动抓图通过，但停止后第二次启动只收到约 4 个 buffer；
  CFE/DMA 的 4K stop/start 重启路径仍需修复，期间可用单次抓图或保持连续流。
- IMX490 已上板验证：Link A locked、serializer `0x91`、ISP `0x6d`，
  2880x1860 UYVY 抓图成功。
- 部分 IMX490 模组的 MAX9295A 默认地址是 `0x62`，驱动已支持自动迁移到
  0x41；这类模组 EEPROM `0x51` 可能无响应，按输出 2880x1860 识别。
- 0x62 模组实测约 14 fps；EEPROM 有响应的另一颗约 26.4 fps。
- FE（frontend）路径目前仍走 legacy `s_stream`，暂不支持按 pad 独立停流。
- 手动安装单独编译的模块时可能出现
  `no symbol version for module_layout`；完整 `make modules` 构建不会出现。

## 修改记录

详细的移植、API 适配、重命名和问题修复记录见
[Documentation/gmsl/CHANGELOG.md](Documentation/gmsl/CHANGELOG.md)。

`Documentation/gmsl/patches/` 下同时保留了可独立应用到干净
`rpi-6.18.y` 的 patch 和 diff。
