# RPi 5 + MAX96724 + IMX390 GMSL2 camera support

这是基于 `raspberrypi/linux` `rpi-6.18.y` 的完整内核 fork，用于
Raspberry Pi 5 + MAX96724 四路 GMSL2 解串器 + MAX9295A 串行器 +
SG2-IMX390C-5200-GMSL2 相机。

## 分支说明

- `master`（默认）：只保留 SG2-IMX390C-5200-GMSL2 一路相机，包含驱动、
  overlay、抓图脚本和本文档。后续新增相机模组直接在此分支追加提交。
- `max96724-framework`：Phase 1 分层重构分支（当前分支）。模块名、
  compatible、overlay、寄存器序列与 master 保持一致，等待上板 golden test。
- `vendor-full-port`：厂商 14 个 `z_xcdes*` 驱动的完整移植版本，保留作参考；
  本次 IMX390 最小版本不依赖它。

## 基线

- 仓库：`raspberrypi/linux`
- 分支：`rpi-6.18.y`
- 基线 commit：`ea17ae02ca28483e508b05c7c44190c01a1ef8eb`
- 内核版本：6.18.62
- 目标板：Raspberry Pi 5 Model B Rev 1.1
- 测试内核：`6.18.62-v8-16k+`

## 关键文件

```text
drivers/media/i2c/gmsl-core.c/.h
drivers/media/i2c/gmsl-v4l2.c
drivers/media/i2c/gmsl-max96724.c/.h
drivers/media/i2c/gmsl-max9295.c/.h
drivers/media/i2c/gmsl-imx390.c/.h
drivers/media/i2c/gmsl-desc.c/.h
arch/arm/boot/dts/overlays/max96724-imx390-overlay.dts
tools/gmsl/gmsl-capture.sh
Documentation/gmsl/CHANGELOG.md
Documentation/gmsl/patches/rpi-6.18.y-max96724-imx390.patch
Documentation/gmsl/patches/rpi-6.18.y-max96724-imx390.diff
Documentation/gmsl/config.txt.example
```

## 分层结构（max96724-framework）

- `gmsl-core.c/.h`：`gmsl_dev`/`gmsl_link` 状态、I2C 地址切换与重试、
  `gmsl_load_regs`、deser/ser/sensor ops 调度、descriptor 初始化。
- `gmsl-v4l2.c`：4-pad V4L2 subdev，fmt/frame_desc/mbus_config/s_stream；
  对 CFE 暴露的仍然是同一个 `max96724_imx390` sensor entity。
- `gmsl-max96724.c/.h`：MAX96724 解串器 ops（reset/link lock/pipe/VC）。
- `gmsl-max9295.c/.h`：MAX9295A 串行器 ops（地址、link、GPIO frame sync）。
- `gmsl-imx390.c/.h`：IMX390 sensor ops（init/start/stop/fmt/mbus）。
- `gmsl-desc.c/.h`：sensor descriptor 表；新增模组先加 desc + ops + DT。

Phase 1 只做结构拆分，模块名 `max96724_imx390.ko`、compatible
`zuojisi,max96724-imx390`、overlay、config.txt 和寄存器序列保持不变。
`W=1` 编译已通过；上板 golden test 尚未执行，板子仍运行 master 的单体驱动，
部署/重启需要单独确认。

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
dtoverlay=max96724-imx390
```

如果相机接在 CAM/DISP0：

```ini
dtoverlay=max96724-imx390,cam0
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

# 指定 Link / 帧数 / 输出
./tools/gmsl/gmsl-capture.sh -l 0 -n 120 \
  -o /home/zaiwei/cap.raw -p /home/zaiwei/cap.png -r
```

脚本会自动：

- 从 `/sys/bus/i2c/devices/*/gmsl_links` 识别 Link A/B/C/D；
- 遍历 `/dev/media*` 找 `driver rp1-cfe`，避免硬编码 media 编号；
- 从 media topology 找 `rp1-cfe-csi2_chN` 对应的 `/dev/videoN`；
- 使能 `"csi2":4+N -> "rp1-cfe-csi2_chN":0[1]`；
- 设置 4 个 csi2 sink pad 为 `UYVY8_1X16/1920x1080 field:none`；
- 设置视频节点 UYVY 1920x1080；
- 抓帧、统计最后一帧 Y/U/V，可选生成 180° 旋转后的 PNG。

依赖：`v4l-utils`，PNG/统计部分需要 `ffmpeg` 和 `python3`。

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

- 1920x1080 UYVY，约 28 fps
- CFE link rate：`Using a link rate of 912 Mbps`
- 无 `error: slave` 日志
- `get_fmt` 日志数量 0
- `-r` 输出已确认正立

## 已知限制

- 使用 downstream CFE `rp1_cfe`（DT compatible `raspberrypi,rp1-cfe`），
  不是 upstream `rp1-cfe`。
- media 设备编号每次启动可能变化，脚本已自动查找；手工命令不要写死
  `media2`。
- 相机模组物理安装是 180° 倒置的；overlay 不再保留无效的
  `rotation`/`orientation` 属性。需要正立画面时用 `gmsl-capture.sh -r`，
  或在应用里做 `hflip+vflip`。
- 驱动是 SG2-IMX390C-5200-GMSL2 专用单体驱动，分辨率/格式/30fps 和寄存器
  序列硬编码；不是通用 V4L2 sensor/serializer 驱动。
- 没有实现曝光/增益/翻转等 V4L2 控制，模组 AE 自动工作。
- serializer 的 0x007b~0x00ab 寄存器组在 Link A 上回读仍是默认值
  （0x10/0x00），说明这组写在该模组上可能是只读/保留的；实际视频路由由
  MAX96724 的 pipe/VC 寄存器和 serializer 的 0x0318/0x0311/0x0331 完成。
- 手动安装单独编译的模块时可能出现
  `no symbol version for module_layout`；完整 `make modules` 构建不会出现。

## 修改记录

详细的移植、API 适配、重命名和问题修复记录见
[Documentation/gmsl/CHANGELOG.md](Documentation/gmsl/CHANGELOG.md)。

`Documentation/gmsl/patches/` 下同时保留了可独立应用到干净
`rpi-6.18.y` 的 patch 和 diff。
