# Raspberry Pi 5 + MAX96724 GMSL2 相机适配完全指南

> 版本：2026-11-22
> 适用内核：`raspberrypi/linux` `rpi-6.18.y`，6.18.62，运行版本 `6.18.62-v8-16k+`
> 目标硬件：Raspberry Pi 5 Model B Rev 1.1 + MAX96724 四路 GMSL2 解串板
> 已支持模组：SG2-IMX390C、SG8-AR0820C、SG5-IMX490C、SG-3M-190A-AA-031（ISX031）
> 代码位置：`rpi-6.18/linux-rpi-6.18.y`
> 相关文档：`GMSL_MAX96724_IMX390.md`、`Documentation/gmsl/ARCHITECTURE.md`、`Documentation/gmsl/CHANGELOG.md`、`tools/gmsl/README.md`

---

## 0. 阅读指南

这份文档把“树莓派 Camera 基础 → GMSL 基础 → 本项目框架 → 构建部署 → 设备树 → 四路混插 → 新增模组 → 调试 → 出图/录制 → 验收 → 适配历史”放在一起，适合当作项目的长期手册。

推荐阅读路线：

| 你的目标 | 建议顺序 |
|---|---|
| 第一次接触这个项目 | 1 → 2 → 3 → 4 → 5 → 6 → 7 → 10 → 11 |
| 只想把相机点亮 | 5 → 6 → 7 → 10.7 → 11.1 |
| 新增一款相机模组 | 4 → 9 → 7 → 8 → 12 |
| 调试四路混插 | 4.7 → 8 → 10 → 12 |
| 排查“没有图/没有 link” | 10.7 → 3.7 → 8.8 |

文档中的命令默认在开发主机上执行，标注“在 Pi 上”的命令才在目标板执行。

---

## 1. 系统总览

### 1.1 一句话理解

树莓派 5 的 Camera 接口在 RP1 芯片上，不直接接在 BCM2712 SoC 上。GMSL 解串器（MAX96724）把 4 路同轴相机汇聚成 1 路 4-lane MIPI CSI-2，送进 RP1 的 CFE；CFE 再交给 PiSP 或直接 DMA 到内存，最终以标准 V4L2 `/dev/videoN` 暴露给用户态。

```text
相机模组（Camera Module）                        树莓派 5
┌──────────────────────────┐                ┌──────────────────────────────────┐
│ Sensor / ISP             │                │  MAX96724（4 路 GMSL2 解串器）    │
│   │ MIPI CSI-2           │   GMSL2 同轴    │    link A/B/C/D → VC0..3          │
│   ▼                      │   ───────────▶ │         │ 4-lane MIPI CSI-2       │
│ Serializer（加串器）     │                │         ▼                        │
└──────────────────────────┘                │  RP1 CFE（csi2_ch0..3）           │
                                            │         │                        │
                                            │         ▼                        │
                                            │  PiSP BE（pisp-be，可选 ISP）     │
                                            │         │                        │
                                            │         ▼                        │
                                            │  /dev/videoN（V4L2）             │
                                            │         │                        │
                                            │         ▼                        │
                                            │  libcamera / rpicam / 应用        │
                                            └──────────────────────────────────┘
```

关键点：

- **内核只负责传输层**：GMSL 链路、CSI-2 接收、DMA、V4L2/media 拓扑。
- **ISP 可能不在 Pi 里**：IMX390/AR0820/IMX490 模组自带 GW5200/GW5300 ISP，host 只控制 ISP 和 SerDes；ISX031 是 sensor 直连（经 MAX96717F pass-through）。
- **PiSP 是可选后端**：当前抓图主要走 CFE 的 CSI-2 直通路径；`pisp-be` 用于需要 PiSP 处理的路径。
- **应用不直接碰寄存器**：应用通过 V4L2/media controller 选择 link、格式、开流；寄存器调试用 `gmsl-reg`。

### 1.2 当前仓库里的两套内核

工作区里有两个内核树，不要混用：

| 路径 | 版本 | 用途 |
|---|---|---|
| `rpi-6.18/linux-rpi-6.18.y` | 6.18.62 | **当前 GMSL 框架开发树**，本文档的主角 |
| `linux` | 6.6.78 | 厂商/旧版树，`mybuild/` 脚本和 `z_xcdes*` 私有驱动使用；不要拿它的模块装到 6.18 上 |

`rpi-6.18/framework-build/` 是 6.18 的 out-of-tree 构建目录（`O=../framework-build`），`CONFIG_LOCALVERSION="-v8-16k"`、16K 页。

### 1.3 关键文件

| 文件 | 作用 |
|---|---|
| `drivers/media/i2c/gmsl-v4l2.c` | V4L2 subdev 适配层、4 个 source pad、格式/帧描述/stream ops、PM |
| `drivers/media/i2c/gmsl-core.c/.h` | GMSL 状态机、I2C helper、link mask、ops 调度 |
| `drivers/media/i2c/gmsl-max96724.c/.h` | MAX96724 解串器 ops |
| `drivers/media/i2c/gmsl-max9295.c/.h` | MAX9295A 加串器 ops |
| `drivers/media/i2c/gmsl-max96717.c/.h` | MAX96717/96717F 加串器 ops |
| `drivers/media/i2c/gmsl-ser-auto.c/.h` | 按 `0x000d` DEV_ID 自动识别加串器 |
| `drivers/media/i2c/gmsl-imx390.c/.h` | IMX390 sensor/ISP ops |
| `drivers/media/i2c/gmsl-ar0820.c/.h` | AR0820 sensor/ISP ops |
| `drivers/media/i2c/gmsl-imx490.c/.h` | IMX490 sensor/ISP ops |
| `drivers/media/i2c/gmsl-isx031.c/.h` | ISX031 sensor ops（真实直连 I2C） |
| `drivers/media/i2c/gmsl-desc.c/.h` | board/link/sensor 描述符表 |
| `drivers/media/platform/raspberrypi/rp1_cfe/` | RP1 CFE 驱动（含 per-CSI2-channel stream 修改） |
| `arch/arm/boot/dts/overlays/max96724-*-overlay.dts` | 四个相机 overlay |
| `tools/gmsl/gmsl-capture.sh` | 一键抓图/生成 PNG |
| `tools/gmsl/gmsl-reg.c` | 用户态 GMSL 寄存器读写工具 |

---

## 2. 树莓派 5 Camera 基础

### 2.1 硬件拓扑

树莓派 5 有两类 MIPI 接口：

- **CAM/DISP0**
- **CAM/DISP1**

每个 FPC 连接器都可以作为相机（CSI-2 输入）或显示（DSI 输出），由设备树 overlay 决定。GMSL 解串板通常接在其中一个口上，本文默认接 **CAM/DISP1**；接 CAM/DISP0 时在 overlay 后加 `,cam0`。

Pi 5 的 Camera 外设位于 **RP1** 南桥：

- RP1 内有两个 CFE 实例：`rp1_csi0`、`rp1_csi1`，分别对应两个 FPC 口。
- 每个 CFE 包含：MIPI D-PHY、CSI-2 接收、DMA、PiSP Front End。
- MAX96724 的 4 路 GMSL link 在内部被复用到 1 路 CSI-2 的 4 个虚拟通道（VC0..3），CFE 再按 VC 解复用成 `csi2_ch0..3`。

### 2.2 软件栈

```text
应用：libcamera / rpicam-apps / 自己的 V4L2 程序
        │
        ▼
V4L2 + media controller
        │
        ├── max96724_imx390（GMSL 解串器 + 相机 subdev）
        │       └── 4 个 source pad ↔ 4 个 GMSL link
        │
        └── rp1-cfe（CFE）
                ├── csi2 子设备（4 个 sink + 4 个 source pad）
                ├── csi2_ch0..3 视频节点
                └── PiSP FE
                        │
                        ▼
                pisp-be（PiSP 后端，可选）
```

几个容易混淆的名字：

| 名字 | 是什么 | 不是什么 |
|---|---|---|
| `rp1-cfe` / `rp1_cfe` | RP1 Camera Front End 驱动 | 不是 ISP，不做去马赛克/AE/AWB |
| `pisp-be` | PiSP Back End，真正的 ISP | 不是 CSI-2 接收端 |
| `max96724_imx390` | 本项目的复合 GMSL 相机驱动模块 | 名字虽然带 imx390，但已支持多种模组 |
| `csi2_ch0..3` | CFE 按 VC 分出来的 4 个采集通道 | 不是 4 个物理 CSI 口 |
| `link A/B/C/D` | MAX96724 的 4 个 GMSL 输入 | 不是 4 个 `/dev/videoN`，但一一对应 |

### 2.3 启动链路

```text
config.txt
  ├── camera_auto_detect=0
  └── dtoverlay=max96724-imx390
        │
        ▼
固件加载 base DTB + overlay
        │
        ▼
设备树中出现 cam_node@36（compatible = "zuojisi,max96724-imx390"）
        │
        ▼
I2C 核心创建 i2c client，匹配 max96724_imx390 驱动
        │
        ▼
probe：读 MAX96724 ID、link lock、SerDes、sensor/ISP
        │
        ▼
注册 4-pad V4L2 subdev → CFE async 绑定 → media graph
        │
        ▼
/dev/mediaN + /dev/videoN 可用
```

### 2.4 设备节点和 media graph

启动后重点看这些节点：

| 节点 | 含义 |
|---|---|
| `/dev/mediaN` | media controller 设备，编号每次启动可能变化 |
| `/dev/videoN` | CFE 的采集节点，`csi2_ch0..3` 各一个 |
| `/dev/v4l-subdevN` | subdev 节点（max96724、csi2、pisp-be 等） |
| `/sys/bus/i2c/devices/*/gmsl_links` | 4 个 link 的 lock 状态，例如 `1 0 0 0 ` |

`media-ctl -p -d /dev/mediaN` 里重点找：

- `max96724-imx390` / `max96724` 实体：4 个 source pad。
- `csi2` 实体：4 个 sink pad + 4 个 source pad。
- `rp1-cfe-csi2_ch0..3`：视频节点实体。
- `pisp-be`：如果走 PiSP 路径会出现。

### 2.5 为什么必须 `camera_auto_detect=0`

树莓派的固件会自动探测标准 CSI 相机并加载对应 overlay。GMSL 解串板对固件来说不是“标准相机”，自动探测可能：

- 加载错误的 IMX219/OV5647 overlay；
- 占用同一个 I2C/CSI 资源；
- 让我们的 `max96724` overlay 无法正确绑定。

所以在 `/boot/firmware/config.txt` 中明确关闭：

```ini
camera_auto_detect=0
```

然后只加载 GMSL overlay。

### 2.6 常用基础命令

```bash
# 看内核版本和模块
uname -a
lsmod | grep -E 'max96724|rp1'

# 看设备
v4l2-ctl --list-devices
media-ctl -p -d /dev/media0

# 看 link 状态
cat /sys/bus/i2c/devices/*/gmsl_links

# 看 probe 日志
dmesg | grep -i -E 'max96724|rp1-cfe|gmsl'
```

---

## 3. GMSL2 基础

### 3.1 为什么用 GMSL

车载/机器人相机需要：

- **长距离**：同轴线可到 10~15 m，STP 也可用；
- **抗干扰**：差分/同轴，适合电机、电源噪声环境；
- **少线束**：一根同轴同时传视频、I2C、GPIO、帧同步；
- **PoC**：Power over Coax，相机端不用单独供电；
- **集中控制**：host 通过 I2C pass-through 访问远端 serializer/ISP/sensor。

GMSL2 前向链路常见 3 Gbps 或 6 Gbps，反向控制链路 187.5 Mbps（GMSL2）；GMSL1 兼容模式速率不同。

### 3.2 链路和术语

```text
Sensor → ISP → Serializer → GMSL2 coax → Deserializer → MIPI CSI-2 → CFE
                                      │
                                      └── 双向 I2C/GPIO/帧同步
```

| 术语 | 含义 |
|---|---|
| Serializer（加串器） | 相机端，把 MIPI/并行视频转成 GMSL2；本项目是 MAX9295A / MAX96717 / MAX96717F |
| Deserializer（解串器） | 板端，把 GMSL2 转回 MIPI CSI-2；本项目是 MAX96724 |
| Link | MAX96724 的一个 GMSL 输入，编号 0..3 = A..D |
| Pipe | MAX96724 内部视频管道，每个 link 一条 |
| VC | MIPI CSI-2 Virtual Channel，本项目 link N → VC N |
| DT | MIPI CSI-2 Data Type，本项目使用 `0x1e`（YUV422 8-bit） |
| Frame sync | 多相机同步曝光/帧起始，通过 serializer GPIO 或内部同步 |
| I2C pass-through | host 通过解串器/加串器访问远端 ISP、sensor、EEPROM |
| Address translation | 远端设备地址在 GMSL 链路上被映射/迁移，例如 serializer 0x40 → 0x41+link |

### 3.3 本项目的芯片组合

| 芯片 | 角色 | 关键点 |
|---|---|---|
| MAX96724 | 4-link GMSL2 解串器 | 4 路输入 → 1 路 4-lane CSI-2；支持 2x4/4x2、VC、pipe |
| MAX9295A | 加串器 | DEV_ID `0x91`；默认地址可能是 `0x40` 或 `0x62`，驱动迁移到 `0x41+link` |
| MAX96717 | 加串器 | DEV_ID `0xbf`；配置寄存器与 MAX9295A 略有不同 |
| MAX96717F | 加串器 | DEV_ID `0xc8`；本项目用于 ISX031，最高 3 Gbps |
| MAX96712 | 4-link 解串器（同系列） | ADI 现网没有公开产品页/数据手册；可参考 MAX96724 文档，精确差异需向 ADI 确认 |
| MAX9296A | 双路解串器（同系列） | 官方不公开完整数据手册；公开近似参考是 MAX96716A |
| MAX9295A | 加串器（同系列） | 官方不公开完整数据手册；公开近似参考是 MAX9295D |

> 说明：MAX96712/MAX9296A/MAX9295A 的完整寄存器手册通常需要 ADI FAE/NDA。公开可下载的是 MAX96724、MAX9295D、MAX96716A、MAX96717 系列，见 `datasheets/` 目录。

### 3.4 I2C 地址与 pass-through

本项目的典型地址（7-bit）：

| 目标 | 地址 | 说明 |
|---|---|---|
| MAX96724 解串器 | `0x27` | 驱动内部固定用它访问，见 `gmsl-max96724.c` 的 `MAX96724_ADDR` |
| 加串器（迁移后） | `0x41 + link` | Link A=0x41，B=0x42，C=0x43，D=0x44 |
| 加串器（出厂） | `0x40` 或 `0x62` | 驱动 probe 时自动探测并迁移 |
| GW5200/GW5300 ISP | `0x6d` | IMX390/AR0820/IMX490 模组 |
| 模组 EEPROM | `0x51` | 部分模组有响应，部分没有 |
| IMX390 sensor | `0x21` | 在 GW5200 后面，host 默认不能直读 |
| ISX031 sensor | `0x1a` | 在 MAX96717F pass-through 总线上，可直接读写 |

注意：

- overlay 里的 `reg = <0x36>` **不是** MAX96724 的真实地址，而是复合相机节点的 I2C 绑定占位地址；驱动内部用 `0x27` 访问解串器。
- `gmsl-reg` 的 `ser` 别名默认是 `0x41`，可以用 `-l` 选择 link。
- 多 link 同时访问时，解串器的 I2C 转发路由（`0x0006`）需要指向目标 link；当前 `gmsl-reg -l` 只改别名，不自动切路由，调试前先停止出流。

### 3.5 Link / Pipe / VC / Pad / Channel 映射

本项目把编号统一成一条直线：

```text
GMSL link N（A/B/C/D）
  = MAX96724 pipe N
  = MIPI CSI-2 VC N
  = max96724 subdev source pad N
  = CFE csi2 sink pad N
  = CFE csi2_chN
  = /dev/videoX（由 media graph 决定）
```

| Link | 字母 | pipe | VC | max96724 pad | CFE 通道 | 视频节点 |
|---|---|---|---|---|---|---|
| 0 | A | 0 | 0 | 0 | `csi2_ch0` | `/dev/video?` |
| 1 | B | 1 | 1 | 1 | `csi2_ch1` | `/dev/video?` |
| 2 | C | 2 | 2 | 2 | `csi2_ch2` | `/dev/video?` |
| 3 | D | 3 | 3 | 3 | `csi2_ch3` | `/dev/video?` |

视频节点编号每次启动可能变化，必须用 `media-ctl -p` 或 `gmsl-capture.sh` 自动查找，不要写死 `/dev/video0`。

### 3.6 上电、PoC 和帧同步

- **上电顺序**：先给解串板/相机供电，再给 Pi 上电；掉电时反过来。相机链路异常时，先把 12V PoC 断电重启，再重启 Pi。
- **PoC**：同轴线同时供电，相机端不要额外接电源；确认解串板的 PoC 电感和保险丝、FAKRA 连接器方向。
- **帧同步**：MAX9295A/MAX96717 支持 GPIO frame sync；当前驱动在 `gmsl_core_s_stream()` 里对新启动的 link 调用 `gpio_frame_sync()`。ISX031 模组使用内部同步，board desc 设置 `skip_frame_sync=true`。
- **3 Gbps vs 6 Gbps**：MAX96724 默认 6 Gbps；MAX96717F 最高 3 Gbps。3 Gbps 模组需要 board desc 的 `three_gbps=true`、`phy_clk=0x25`，并在 `max96724_init()` 里先打开 link enable 再读 lock。混插 3 Gbps/6 Gbps 是当前未完成的项，详见 8.6。

### 3.7 常见 GMSL 故障模式

| 现象 | 常见原因 | 先查什么 |
|---|---|---|
| `gmsl_links = 0 0 0 0` | 相机没供电、同轴没接、link 没 enable、3Gbps 配置错 | PoC 12V、FAKRA、`dmesg` 的 link lock、serializer DEV_ID |
| 有 link 但 probe 失败 | serializer 地址不是 0x40/0x62、I2C 总线错、overlay 没加载 | `i2cdetect`、`gmsl-reg scan`、`dmesg` |
| 有 link 但无图 | sensor/ISP 没出流、pipe/VC 错、CFE 格式不匹配 | `media-ctl -p`、`v4l2-ctl --get-fmt-video`、CFE 日志 |
| 图像花屏/颜色错 | UYVY/YUYV 搞反、stride 不对、DT/VC 错 | PNG 统计、`v4l2-ctl` 格式、`get_frame_desc` |
| 第二次启动 4K 失败 | CFE/DMA 4K stop/start 重启问题 | 已知限制，见 12.5 |
| `error: slave=...` | I2C NACK、地址迁移失败、远端设备不在线 | 地址表、`gmsl-reg read`、重新上电 |

---

## 4. 本项目框架设计

### 4.1 设计目标

- 一块 MAX96724 板支持最多 4 路 GMSL link，可混插不同相机模组。
- 新增模组只增加：`sensor_desc`、`sensor_ops`、必要时 `serializer_ops`、一个 DT endpoint/overlay。
- 不复制 core、不复制 V4L2 层、不复制解串器初始化。
- 对用户态保持标准 V4L2/media 接口，不引入自定义 ioctl。
- 单 link 失败隔离，不影响其他 link。

### 4.2 分层结构

```text
gmsl-v4l2.c        V4L2 subdev：4 pad、fmt、frame_desc、mbus_config、stream、PM
      │
gmsl-core.c        状态机：link lock、I2C helper、ops 调度、streaming_mask
      │
      ├── gmsl-max96724.c     解串器 ops
      ├── gmsl-max9295.c      MAX9295A 加串器 ops
      ├── gmsl-max96717.c     MAX96717/96717F 加串器 ops
      ├── gmsl-ser-auto.c     加串器自动识别
      └── gmsl-<sensor>.c     sensor/ISP ops
      │
gmsl-desc.c        board_desc + link_desc + sensor_desc 表
```

各层职责：

| 层 | 负责 | 不负责 |
|---|---|---|
| `gmsl-v4l2.c` | V4L2/media、pad、格式、stream 入口、PM、regulator/clock | 寄存器细节、link lock 细节 |
| `gmsl-core.c` | link 状态、I2C 重试、ops 调用、mask、rollback | 具体芯片寄存器 |
| `gmsl-max96724.c` | MAX96724 reset/lock/pipe/VC/monopolize | sensor 控制 |
| `gmsl-max9295.c` / `gmsl-max96717.c` | 加串器地址、pipe、DT、lane、GPIO sync | V4L2 |
| `gmsl-<sensor>.c` | sensor/ISP init/start/stop、格式、mbus | 解串器寄存器 |
| `gmsl-desc.c` | 板/链路/模组描述 | 代码逻辑 |

约束：

- 所有 ops 只能通过 `gmsl_read_addr()` / `gmsl_write_addr()` 访问 I2C，不直接调用 `i2c_transfer()`。
- core 里不出现 `if (sensor == imx390)` 这种芯片分支。
- 新增模组的 commit 只包含 desc/ops/DT/测试，不改 core。

### 4.3 对象模型

```c
struct gmsl_board_desc {
    const char *compatible;
    const struct gmsl_des_ops *des_ops;
    unsigned int num_links;
    const struct gmsl_link_desc *links;
    u8 phy_clk;
    bool three_gbps;
    bool skip_frame_sync;
};

struct gmsl_link_desc {
    unsigned int index;              /* 0..3 = A..D = pad = VC，可稀疏 */
    const struct gmsl_sensor_desc *sensor;
    const struct gmsl_ser_ops *ser_ops;
    u8 sensor_addr;
    u8 num_lanes;
    u64 link_freq;
};

struct gmsl_sensor_desc {
    const char *compatible;
    const char *name;
    u32 width, height;
    u32 mbus_code;
    u8 csi_dt;
    const struct gmsl_sensor_ops *sensor_ops;
    const struct gmsl_reg_value *init;
    unsigned int init_len;
    const struct gmsl_reg_value *start;
    unsigned int start_len;
    const struct gmsl_reg_value *stop;
    unsigned int stop_len;
};
```

`gmsl_core_set_board()` 按 `link_desc.index` 稀疏填充 `gmsl->links[4]`，允许只配置 Link A/C 等组合；未使用的 link 继承首个已配置 link 的默认格式，保证 4 个 source pad 始终报告合法格式（CFE pipeline 校验要求）。

### 4.4 Probe 流程

```text
i2c client 匹配 compatible
  → i2c_get_match_data() 得到 board_desc
  → 解析 endpoint（link-frequencies、data-lanes）
  → 获取 xclk、可选 reset/powerdown GPIO、regulator
  → 创建 4 个 source pad 的 V4L2 subdev（MEDIA_ENT_F_CAM_SENSOR）
  → gmsl_core_hw_init():
       deser init（ID/version/reset/link lock）
       for each locked link:
         deser configure_link（pipe/VC/DT）
         deser monopolize_link（0x0006 路由到该 link）
         ser probe（0x40/0x41+link/0x62，迁移地址）
         ser configure（pipe/DT/lane/GPIO sync）
       deser restore_link（0x0006 = 0xf0 | lock mask）
  → gmsl_camera_sensor_resume() → gmsl_core_sensor_init()
       for each locked link:
         sensor/ISP init（如果该 sensor ops 有 init）
  → v4l2_async_register_subdev_sensor()
  → 创建 gmsl_links sysfs
```

失败处理：

- 单个 link 的 ser/sensor init 失败，只清除该 link 的 lock bit，不影响其他 link。
- 只有解串器全局初始化失败（ID 不对、没有 lock）才整体 probe 失败。
- I2C 读写有 3 次重试，避免瞬态 NACK 污染日志。

### 4.5 Streaming 流程

`gmsl_core_s_stream(gmsl, link_mask, on)`：

```text
enable:
  target = link_mask & link_lock_status
  changed = target & ~streaming_mask
  for changed links: sensor_ops->start_stream(link, true)
  streaming_mask |= changed
  des_ops->enable_pipes(gmsl, link_lock_status, true)
  for changed links: ser_ops->gpio_frame_sync(link)（除非 skip_frame_sync）
disable:
  changed = target & streaming_mask
  for changed links: sensor_ops->start_stream(link, false)
  streaming_mask &= ~changed
  des_ops->enable_pipes(gmsl, 0, false)  // 当前实现不真正关 pipe
```

关键行为：

- **pipe 保持策略**：只要还有 locked link，启动任一路时都把**所有 locked link** 的 pipe 使能；停止某一路时不关闭 pipe。原因是关闭某路 pipe 会停掉远端 ISP 输出，重新使能 pipe 也恢复不了（双 IMX490 实测根因）。
- **CFE 配合**：`rp1_cfe/cfe.c` 的 csi2 直通路径按 `node->id` 调用 `v4l2_subdev_enable_streams(sensor, pad, BIT(0))`；第一个通道打开 CSI RX，最后一个通道关闭。这样单路停止不会关闭其他通道。
- **FE 路径**：仍走 legacy `s_stream`，暂不支持按 pad 独立停流。

### 4.6 当前支持矩阵

| board_desc | compatible | link | sensor | 分辨率/格式 | 加串器 | link-freq (CFE) | overlay | 状态 |
|---|---|---|---|---|---|---|---|---|
| `max96724_imx390_board` | `zuojisi,max96724-imx390` | A（index 0） | IMX390 | 1920x1080 UYVY | MAX9295A | 456 MHz / 912 Mbps | `max96724-imx390` | 已上板 |
| `max96724_ar0820_board` | `zuojisi,max96724-ar0820` | A | AR0820 | 3840x2160 UYVY | 自动（实测 MAX9295A） | 800 MHz / 1600 Mbps | `max96724-ar0820` | 单次冷启动 4K 通过；重复 start/stop 待修 |
| `max96724_imx490_board` | `zuojisi,max96724-imx490` | A-D（index 0..3） | IMX490 | 2880x1860 UYVY | 自动 | 456 MHz / 912 Mbps | `max96724-imx490` | Link A/B 双路通过；4 路未实测 |
| `max96724_isx031_board` | `zuojisi,max96724-isx031` | A | ISX031 | 1920x1536 UYVY | MAX96717F | 288 MHz / 576 Mbps | `max96724-isx031` | Link A 通过（3 Gbps） |

### 4.7 已知限制

| 限制 | 影响 | 备注 |
|---|---|---|
| AR0820 4K 停止后第二次启动只收约 4 个 buffer | 4K 不能连续 start/stop | 定位在 CFE/DMA 4K 重启路径 |
| 4 路 mixed-link 未做硬件矩阵 | 独立启停/错误隔离缺少实测 | 双 IMX490 已通过；4 路待补 |
| 3 Gbps/6 Gbps 混插 | `three_gbps`、`phy_clk`、`skip_frame_sync` 是 board 全局 | 需要 per-link 化 |
| 不同分辨率同时出流 | CFE 共享 D-PHY，抓图脚本把 4 个 csi2 pad 设成同一格式 | 需要按通道设置/CFE 支持 |
| 无 link loss/hot-plug 恢复 | 拔线/复位后不能自动恢复 | 需 delayed work/错误计数 |
| 无 per-link 错误计数/debugfs | 现场定位困难 | 目前靠 dmesg + gmsl-reg |
| IMX390/AR0820/IMX490 的 sensor 在 ISP 后面 | 不能直接读写 sensor | 控制 ISP `0x6d`，sensor 直读需厂商 pass-through |
| 无曝光/增益/翻转 V4L2 controls | 只能靠模组 AE/应用软件旋转 | 模组 AE 自动工作 |
| 模块名/日志仍叫 `max96724_imx390` | 使用 ISX031 时日志前缀不变 | 历史兼容，后续可改 `gmsl-max96724` |

---

## 5. 硬件与接线

### 5.1 连接器

- Pi 5 的 CAM/DISP0、CAM/DISP1 都是 22-pin FPC；确认排线方向（金手指朝向）。
- 解串板输出接 Pi 的 CAM 口；相机接解串板的 FAKRA 口。
- 默认 overlay 使用 **CAM/DISP1**；接 **CAM/DISP0** 时 `dtoverlay=...,cam0`。
- 不要热插拔 FAKRA/同轴；断电后再插拔。

### 5.2 模组拓扑

| 模组 | 链路 |
|---|---|
| SG2-IMX390C | IMX390(0x21) → GW5200 ISP(0x6d) → MAX9295A(0x41) → MAX96724(0x27) |
| SG8-AR0820C | AR0820 → GW5300/5200 ISP → MAX9295A（实测 0x91）→ MAX96724 |
| SG5-IMX490C | IMX490 → 模组 ISP → MAX9295A（0x41 或 0x62）→ MAX96724 |
| SG-3M-190A-AA-031 | ISX031(0x1a) → MAX96717F(0x41) → MAX96724 |

要点：

- IMX390/AR0820/IMX490 的 sensor 在 ISP 后面，host 默认看不到 sensor 地址；`gmsl-reg read sensor` 对这些模组 NACK 是正常的。
- ISX031 的 sensor 在 MAX96717F pass-through 上，`0x1a` 可以直接读写；驱动在 probe 时使能 pass-through 并释放 sensor reset。
- 部分 IMX490 模组的 MAX9295A 出厂地址是 `0x62`，驱动会自动探测并迁移到 `0x41`；如果模组不接受迁移，会保留 `0x62`。

### 5.3 上电顺序

推荐：

1. 连接 FAKRA/同轴和 FPC。
2. 给解串板/相机 12V PoC 上电。
3. 给 Pi 5 上电。
4. 启动后检查 link lock。

掉电/重启：

1. Pi 关机。
2. 断 12V。
3. 等 2~3 秒。
4. 重新上 12V，再上 Pi。

相机不锁、I2C NACK、4K 第二次启动异常时，先做一次完整 12V 断电重启，再判断软件问题。

### 5.4 上电前检查清单

- [ ] FPC 方向正确，CAM/DISP 口与 overlay 的 `cam0` 参数一致。
- [ ] FAKRA 插紧，同轴没有短路/断路。
- [ ] 解串板 12V/PoC 供电正常，电压和电流在模组要求范围内。
- [ ] `config.txt` 已设 `camera_auto_detect=0`，只加载一个 GMSL overlay。
- [ ] 内核/模块/overlay 是同一棵树、同一版本构建的。
- [ ] 没有同时加载旧的 `z_xcdes*` 驱动或旧 overlay。

---

## 6. 构建与部署

### 6.1 环境准备

在 x86_64 开发主机上安装交叉编译工具和内核构建依赖（以 Debian/Ubuntu 为例）：

```bash
sudo apt install -y \
  build-essential bc bison flex libssl-dev libncurses-dev \
  gcc-aarch64-linux-gnu \
  device-tree-compiler \
  kmod rsync
```

确认：

```bash
aarch64-linux-gnu-gcc --version
dtc --version
```

### 6.2 配置内核

```bash
cd rpi-6.18/linux-rpi-6.18.y
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

# 首次或需要恢复默认配置时
make O=../framework-build bcm2712_defconfig

# 如果 framework-build/.config 已存在，想沿用现有配置
make O=../framework-build olddefconfig
```

确认关键配置：

```bash
grep -E 'CONFIG_LOCALVERSION|CONFIG_ARM64_16K_PAGES|CONFIG_VIDEO_RP1_CFE' \
  ../framework-build/.config
```

应看到：

```text
CONFIG_LOCALVERSION="-v8-16k"
CONFIG_ARM64_16K_PAGES=y
CONFIG_VIDEO_RP1_CFE=m
CONFIG_VIDEO_RP1_CFE_DOWNSTREAM=m
```

说明：

- `max96724_imx390` **没有** `CONFIG_VIDEO_MAX96724` 这样的配置项；它在 `drivers/media/i2c/Makefile` 里是 `obj-m += max96724_imx390.o`，会作为模块无条件构建。
- 这里有两个 CFE 驱动：
  - `CONFIG_VIDEO_RP1_CFE=m` → 上游 `rp1-cfe.ko`（源码 `drivers/media/platform/raspberrypi/rp1-cfe/`）；
  - `CONFIG_VIDEO_RP1_CFE_DOWNSTREAM=m` → **本项目实际使用的** downstream `rp1-cfe-downstream.ko`（源码 `drivers/media/platform/raspberrypi/rp1_cfe/`）。
- 本项目的 per-CSI2-channel streaming 修改在 `rp1_cfe/cfe.c` 里，所以**必须一起重新构建/部署 `rp1-cfe-downstream.ko`**，不能只换 `max96724_imx390.ko`。
- 两个驱动的 media 设备名都可能是 `rp1-cfe`；`gmsl-capture.sh` 用 `driver rp1-cfe` 找 media 设备。确认实际绑定的是 downstream，可看 `lsmod`（`rp1_cfe_downstream`）和 `dmesg`。

### 6.3 全量构建

```bash
make O=../framework-build -j$(nproc) Image modules dtbs
```

产物：

| 产物 | 路径 |
|---|---|
| 内核 Image | `../framework-build/arch/arm64/boot/Image` |
| 基础 DTB | `../framework-build/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dtb` |
| overlay | `../framework-build/arch/arm64/boot/dts/overlays/max96724-*.dtbo` |
| GMSL 模块 | `../framework-build/drivers/media/i2c/max96724_imx390.ko` |
| CFE 模块（downstream） | `../framework-build/drivers/media/platform/raspberrypi/rp1_cfe/rp1-cfe-downstream.ko` |

如果 `framework-build` 里找不到某个产物，先确认 `dtbs`/`modules` 是否真的执行成功，再用 `find` 定位：

```bash
find ../framework-build -name 'max96724_imx390.ko' -o -name 'rp1-cfe-downstream.ko' -o -name 'max96724-*.dtbo'
```

### 6.4 只重建驱动（开发迭代）

只改 GMSL 驱动时，可以只构建模块：

```bash
make O=../framework-build -j$(nproc) \
  M=drivers/media/i2c modules
```

只改 CFE 时：

```bash
make O=../framework-build -j$(nproc) \
  M=drivers/media/platform/raspberrypi/rp1_cfe modules
```

注意：

- 必须使用与目标内核**同一份源码、同一份 `.config`、同一个 `O=` 构建目录**，否则可能出现 `no symbol version for module_layout`。
- 不建议把在别处单独编译的 `.ko` 直接复制到 Pi；symbol version/vermagic 不匹配会加载失败。
- 最稳妥的方式仍然是完整 `make modules` + `modules_install`。

### 6.5 部署

假设目标 Pi 已通过网络/SSH 可访问，`RASPI_IP`、`RASPI_PASSWD` 已设置。

```bash
KERNEL=kernel_2712
BUILD=../framework-build

# 1) 内核
scp $BUILD/arch/arm64/boot/Image root@$RASPI_IP:/boot/firmware/$KERNEL.img

# 2) 基础 DTB（可选，只有改了 base DTS 才需要）
scp $BUILD/arch/arm64/boot/dts/broadcom/*.dtb root@$RASPI_IP:/boot/firmware/

# 3) overlay
scp $BUILD/arch/arm64/boot/dts/overlays/max96724-*.dtbo \
  root@$RASPI_IP:/boot/firmware/overlays/

# 4) 模块（推荐用 modules_install 到临时根目录，再整体同步）
make O=$BUILD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  INSTALL_MOD_PATH=/tmp/rpi-rootfs modules_install
rsync -a /tmp/rpi-rootfs/lib/modules/ root@$RASPI_IP:/lib/modules/

# 5) 在 Pi 上
ssh root@$RASPI_IP 'depmod -a && reboot'
```

如果不想用 `modules_install`，至少复制：

```text
/lib/modules/$(uname -r)/kernel/drivers/media/i2c/max96724_imx390.ko
/lib/modules/$(uname -r)/kernel/drivers/media/platform/raspberrypi/rp1_cfe/rp1-cfe-downstream.ko
```

然后 `depmod -a`。

### 6.6 `/boot/firmware/config.txt`

单相机示例：

```ini
camera_auto_detect=0

# CAM/DISP1（默认）
dtoverlay=max96724-imx390

# 如果接在 CAM/DISP0：
#dtoverlay=max96724-imx390,cam0
```

其他模组：

```ini
#dtoverlay=max96724-ar0820
#dtoverlay=max96724-imx490
#dtoverlay=max96724-isx031
```

规则：

- 一次只启用一个 GMSL overlay（除非你明确知道两个 CSI 口各接一个解串板）。
- `camera_auto_detect=0` 必须保留。
- 改完 `config.txt` 必须重启；overlay 不会热加载。

### 6.7 启动后验证

```bash
# 内核版本
uname -r
# 期望：6.18.62-v8-16k+

# 模块
lsmod | grep -E 'max96724|rp1'
modinfo max96724_imx390 | head

# link 状态
cat /sys/bus/i2c/devices/*/gmsl_links
# 单相机 Link A：1 0 0 0
# 双相机 A/B：1 1 0 0

# probe 日志
dmesg | grep -i -E 'max96724|rp1-cfe|gmsl' | tail -50
```

正常日志片段：

```text
max96724-imx390: probing...
max96724-imx390: deserializer detected
max96724-imx390: link lock status=0x01
max96724-imx390: serializer detected at 0x41
max96724-imx390: probe success!
rp1-cfe ...: Using a link rate of 912 Mbps
```

`Using a link rate of 912 Mbps` 对应 DT 的 `link-frequencies = <456000000>`；1600 Mbps 对应 AR0820 的 800 MHz；576 Mbps 对应 ISX031 的 288 MHz。

### 6.8 常见构建/部署错误

| 现象 | 原因 | 处理 |
|---|---|---|
| `no symbol version for module_layout` | 模块和内核不是同一棵树/同一配置构建的 | 用同一 `O=` 完整构建模块，或完整 `modules_install` |
| `modprobe: FATAL: Module max96724_imx390 not found` | 模块没装到 `$(uname -r)` 对应目录，或没 `depmod` | 检查 `/lib/modules/$(uname -r)/...`，执行 `depmod -a` |
| overlay 不生效 | dtbo 没复制、`config.txt` 没改、`camera_auto_detect=1` | 检查 `/boot/firmware/overlays/`、`dtoverlay -l`、`dmesg` |
| 找不到 `CONFIG_VIDEO_MAX96724` | 项目没有这个 Kconfig | 模块是 `obj-m` 无条件构建，直接 `make modules` |
| 只换了 GMSL 模块，单路停止会关掉另一路 | CFE 的 per-channel 修改没部署 | 同时部署 `rp1-cfe-downstream.ko` |
| `supply DOVDD not found, using dummy regulator` | overlay 用 VANA/VDIG/VDDL，驱动 bulk get 用 DOVDD/AVDD/DVDD | 功能上可用；要真正控制电源就统一命名 |
| `rp1-cfe-downstream` 加载失败 | 依赖模块/固件缺失 | `modprobe rp1-cfe-downstream` 看错误，检查 `dmesg` |
| 内核起来但没有任何 media 设备 | overlay 没启用 `csi1`/`i2c_csi_dsi` | 检查 overlay fragment 0/1/2/100/101 |

---

## 7. 设备树 Overlay 详解

### 7.1 Overlay 是什么

树莓派的启动流程是：

```text
base DTB（bcm2712-rpi-5-b.dtb）
  + overlay（max96724-imx390.dtbo）
  = 最终设备树
```

overlay 不是独立设备树，它通过 `fragment` 往 base DTB 的指定节点里“打补丁”。编译后是 `.dtbo`，放在 `/boot/firmware/overlays/`，由 `config.txt` 的 `dtoverlay=` 加载。

几个语法：

| 语法 | 含义 |
|---|---|
| `/dts-v1/; /plugin/;` | 声明这是 overlay |
| `fragment@N { target = <&label>; __overlay__ { ... }; }` | 往 base DTB 的 `label` 节点里追加/覆盖属性 |
| `__dormant__` | 默认不生效，只有 `__overrides__` 激活时才应用 |
| `__overrides__` | overlay 的运行时参数，例如 `,cam0`、`,4lane` |
| `target:0=` | 覆盖 fragment 的 target（用于 `cam0` 切换） |
| `+201+202` | 激活编号 201/202 的 dormant fragment |

### 7.2 完整示例：`max96724-imx390-overlay.dts`

下面是当前 IMX390 overlay 的完整结构（省略了部分空行，保留了实际属性）：

```dts
// MAX96724 + MAX9295A + IMX390 GMSL2 camera
/dts-v1/;
/plugin/;

#include <dt-bindings/gpio/gpio.h>
/{
    compatible = "brcm,bcm2835";

    fragment@0 {
        target = <&i2c0if>;
        __overlay__ {
            status = "okay";
        };
    };

    clk_frag: fragment@1 {
        target = <&cam1_clk>;
        __overlay__ {
            status = "okay";
            clock-frequency = <24000000>;
        };
    };

    fragment@2 {
        target = <&i2c0mux>;
        __overlay__ {
            status = "okay";
        };
    };

    i2c_frag: fragment@100 {
        target = <&i2c_csi_dsi>;
        __overlay__ {
            #address-cells = <1>;
            #size-cells = <0>;
            status = "okay";

            cam_node: imx390@36 {
                compatible = "zuojisi,max96724-imx390";
                reg = <0x36>;
                status = "ok";

                clocks = <&cam1_clk>;
                clock-names = "xclk";

                VANA-supply = <&cam1_reg>;      /* 2.8v */
                VDIG-supply = <&cam_dummy_reg>; /* 1.8v */
                VDDL-supply = <&cam_dummy_reg>; /* 1.2v */

                port {
                    cam_endpoint: endpoint {
                        clock-lanes = <0>;
                        data-lanes = <1 2 3 4>;
                        clock-noncontinuous;
                        link-frequencies =
                            /bits/ 64 <456000000>;
                    };
                };
            };

            vcm: ad5398@c {
                compatible = "adi,ad5398";
                reg = <0x0c>;
                status = "disabled";
                VANA-supply = <&cam1_reg>;
            };
        };
    };

    csi_frag: fragment@101 {
        target = <&csi1>;
        csi: __overlay__ {
            status = "okay";
            brcm,media-controller;

            port {
                csi_ep: endpoint {
                    remote-endpoint = <&cam_endpoint>;
                    clock-lanes = <0>;
                    data-lanes = <1 2 3 4>;
                    clock-noncontinuous;
                };
            };
        };
    };

    fragment@201 {
        target = <&csi_ep>;
        __dormant__ {
            data-lanes = <1 2 3 4>;
        };
    };

    fragment@202 {
        target = <&cam_endpoint>;
        __dormant__ {
            data-lanes = <1 2 3 4>;
            link-frequencies =
                /bits/ 64 <363000000>;
        };
    };

    __overrides__ {
        media-controller = <&csi>,"brcm,media-controller?";
        cam0 = <&i2c_frag>, "target:0=",<&i2c_vc>,
               <&csi_frag>, "target:0=",<&csi0>,
               <&clk_frag>, "target:0=",<&cam0_clk>,
               <&cam_node>, "clocks:0=",<&cam0_clk>,
               <&cam_node>, "VANA-supply:0=",<&cam0_reg>,
               <&vcm>, "VANA-supply:0=", <&cam0_reg>;
        vcm = <&vcm>, "status=okay",
              <&cam_node>,"lens-focus:0=", <&vcm>;
        4lane = <0>, "+201+202";
    };
};

&cam_node {
    status = "okay";
};

&cam_endpoint {
    remote-endpoint = <&csi_ep>;
};
```

### 7.3 逐个 fragment 解释

| fragment | target | 作用 |
|---|---|---|
| `fragment@0` | `&i2c0if` | 打开 I2C0 控制器。Pi 5 的相机 I2C 来自 I2C0。 |
| `fragment@1` | `&cam1_clk` | 打开 CAM/DISP1 的 24 MHz 时钟，`clock-frequency = <24000000>`。 |
| `fragment@2` | `&i2c0mux` | 打开 I2C0 mux。相机 I2C 是 I2C0 的一个 mux 分支。 |
| `fragment@100` | `&i2c_csi_dsi` | 在相机 I2C 总线上创建 `cam_node@36`，并挂一个可选 VCM。 |
| `fragment@101` | `&csi1` | 打开 CAM/DISP1 的 CSI 接收器，建立 `cam_node` ↔ CSI 的 endpoint 连接。 |
| `fragment@201` | `&csi_ep` | dormant：`4lane` 参数时把 CSI endpoint 设为 4 lane。 |
| `fragment@202` | `&cam_endpoint` | dormant：`4lane` 参数时把相机 endpoint 设为 4 lane，并把 link freq 改成 363 MHz。 |
| `__overrides__` | 无 | 定义 `media-controller`、`cam0`、`vcm`、`4lane` 四个运行时参数。 |
| 末尾 `&cam_node` / `&cam_endpoint` | 标签 | 确保节点 `status = "okay"`，并把 `cam_endpoint` 的 `remote-endpoint` 指向 `csi_ep`。 |

### 7.4 每个属性的含义

| 属性 | 出现在 | 含义 | 本项目取值/注意 |
|---|---|---|---|
| `compatible = "brcm,bcm2835"` | overlay 根 | overlay 自身的兼容字符串，RPi overlay 的标准写法 | 不要改 |
| `compatible = "zuojisi,max96724-imx390"` | `cam_node` | 匹配 `gmsl-v4l2.c` 的 `of_device_id`，决定使用哪个 `board_desc` | 换模组/混插时改这里 |
| `reg = <0x36>` | `cam_node` | I2C client 的绑定地址 | **占位地址**，不是 MAX96724 的真实 0x27；驱动内部固定用 0x27 |
| `status = "okay"` / `"ok"` | 各节点 | 启用节点 | `ok` 和 `okay` 等价 |
| `clocks = <&cam1_clk>` | `cam_node` | 相机 xclk 时钟源 | 驱动 `devm_clk_get(dev, "xclk")` |
| `clock-names = "xclk"` | `cam_node` | 时钟名 | 必须是 `xclk` |
| `clock-frequency = <24000000>` | `cam1_clk` | 24 MHz | 模组要求 24 MHz INCK 时使用 |
| `VANA-supply` / `VDIG-supply` / `VDDL-supply` | `cam_node` | 相机模拟/IO/数字电源 | 见下方“电源命名”注意 |
| `port` / `endpoint` | `cam_node` / `csi` | 描述 CSI-2 连接 | 不是 GMSL 同轴侧 |
| `remote-endpoint = <&csi_ep>` | `cam_endpoint` | 指向 CSI 接收端 | 双向引用，csi 侧也指向 cam |
| `clock-lanes = <0>` | endpoint | MIPI CSI-2 时钟 lane | 固定 0 |
| `data-lanes = <1 2 3 4>` | endpoint | 4 条数据 lane | 对应 4-lane CSI-2 |
| `clock-noncontinuous` | endpoint | 非连续时钟 | 与 CFE 配置匹配 |
| `link-frequencies = /bits/ 64 <456000000>` | `cam_endpoint` | CSI-2 输出链路频率（每 lane，DDR） | 456 MHz → CFE 912 Mbps；AR0820 用 800 MHz → 1600 Mbps；ISX031 用 288 MHz → 576 Mbps |
| `brcm,media-controller` | `csi` | 让 CFE 使用 media controller | 当前 overlay 默认开启 |
| `__dormant__` | fragment | 默认不应用 | 只有 `4lane` 参数激活 |
| `target:0=` | `cam0` override | 替换 fragment 的 target | `i2c_csi_dsi` → `i2c_vc`，`csi1` → `csi0` |

补充：`cam_node: imx390@36` 里的 `imx390@36` 只是节点名/习惯写法，驱动匹配靠 `compatible`，不是靠节点名；混插时可以改成 `gmsl@36`。
| `clocks:0=` | `cam0` override | 替换第一个 clocks 句柄 | `cam1_clk` → `cam0_clk` |
| `VANA-supply:0=` | `cam0` override | 替换 VANA 电源句柄 | `cam1_reg` → `cam0_reg` |
| `status=okay` | `vcm` override | 启用 VCM | 一般不用 |
| `lens-focus:0=` | `vcm` override | 把 VCM 挂到相机 | 一般不用 |
| `+201+202` | `4lane` override | 激活 dormant fragment 201/202 | 只有需要 363 MHz 4-lane 变体时使用 |

#### 电源命名注意

overlay 里的电源属性是 `VANA` / `VDIG` / `VDDL`，但 `gmsl-v4l2.c` 的 `gmsl_supply_name[]` 是：

```c
"DOVDD", "AVDD", "DVDD"
```

因此 probe 时会出现：

```text
supply DOVDD not found, using dummy regulator
```

这是当前代码的实际行为：驱动拿到的是 dummy regulator，电源实际由 `cam1_reg`/`cam_dummy_reg` 的默认状态决定。功能上不影响出图，但**驱动并不能真正控制这三路电源**。如果以后要做低功耗/断电复位，把 overlay 的属性名和驱动数组统一即可（例如都改成 `DOVDD/AVDD/DVDD`，或都改成 `VANA/VDIG/VDDL`）。

### 7.5 CAM/DISP1 与 CAM/DISP0

默认 overlay 使用 CAM/DISP1：

| 资源 | 默认（无参数） | `,cam0` |
|---|---|---|
| I2C 总线 | `i2c_csi_dsi` | `i2c_vc` |
| 时钟 | `cam1_clk` | `cam0_clk` |
| 2.8V 电源 | `cam1_reg` | `cam0_reg` |
| CSI 接收器 | `csi1` | `csi0` |
| 物理接口 | CAM/DISP1 | CAM/DISP0 |

使用方式：

```ini
# CAM/DISP1
dtoverlay=max96724-imx390

# CAM/DISP0
dtoverlay=max96724-imx390,cam0
```

不要同时加载两个相同的 GMSL overlay 去占两个口；如果确实两个口各接一块解串板，需要两个不同的 compatible/board desc，并确认 I2C 地址不冲突。

### 7.6 `4lane`、`media-controller`、`vcm`

- **`4lane`**：当前 base overlay 本身已经是 4 lane（`data-lanes = <1 2 3 4>`）；`4lane` 参数会把 link freq 改成 363 MHz。不要因为名字叫 4lane 就默认加它；只有模组确实需要 363 MHz 时才用。
- **`media-controller`**：base fragment 已经写了 `brcm,media-controller`；这个 override 基本是冗余的，保留是为了兼容旧 config。
- **`vcm`**：启用 `ad5398@0c` 并建立 `lens-focus`。GMSL 相机模组通常没有这个 VCM，保持 disabled。

### 7.7 编译和检查 overlay

```bash
cd rpi-6.18/linux-rpi-6.18.y
make O=../framework-build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dtbs

ls -l ../framework-build/arch/arm64/boot/dts/overlays/max96724-*.dtbo
```

检查 overlay 能否套到 base DTB：

```bash
fdtoverlay \
  -i ../framework-build/arch/arm64/boot/dts/broadcom/bcm2712-rpi-5-b.dtb \
  -o /tmp/merged.dtb \
  ../framework-build/arch/arm64/boot/dts/overlays/max96724-imx390.dtbo
```

反编译看合并结果：

```bash
dtc -I dtb -O dts /tmp/merged.dtb | less
```

在 Pi 上确认已加载：

```bash
dtoverlay -l
ls /proc/device-tree/ | grep -i gmsl
dmesg | grep -i -E 'max96724|rp1-cfe'
```

### 7.8 Overlay 修改清单

改 overlay 时按这个顺序检查：

- [ ] `compatible` 是否和驱动 `of_device_id` 一致。
- [ ] `reg` 是否只作为占位，没有和总线上真实设备冲突。
- [ ] `clocks` / `VANA` 等句柄在 `cam0` 下是否同步替换。
- [ ] `data-lanes` 与模组/解串器实际 lane 数一致。
- [ ] `link-frequencies` 与 CFE 日志的 `Using a link rate of ...` 一致。
- [ ] `remote-endpoint` 双向都写了。
- [ ] `csi1`/`csi0` 与物理接口一致。
- [ ] `config.txt` 里 `camera_auto_detect=0`，只启用一个 GMSL overlay。
- [ ] 重新编译了 `dtbs` 并复制 `.dtbo` 到 `/boot/firmware/overlays/`。
- [ ] 重启后 `dtoverlay -l` 能看到对应 overlay。

---

## 8. 4 个 Link 混插不同相机

### 8.1 先纠正一个常见误解

**overlay 不描述 4 个 GMSL link。** overlay 只描述：

- 解串器作为一个 I2C 设备如何绑定；
- 解串器的 CSI-2 输出如何接到 Pi 的 CSI 接收器。

4 个 GMSL link 在 MAX96724 内部，是同轴侧的输入；它们由驱动里的 `board_desc`/`link_desc` 描述，不是由 overlay 的 4 个 endpoint 描述。

当前代码：

```text
overlay：1 个 cam_node + 1 个 CSI endpoint
driver：board_desc 里 4 个 link_desc（或 1 个）
```

所以“4 个 link 分别接不同相机”的核心工作不在 overlay，而在 `gmsl-desc.c`。

### 8.2 映射关系

```text
Link A/B/C/D
  → MAX96724 pipe 0..3
  → VC 0..3
  → max96724 subdev pad 0..3
  → CFE csi2 sink pad 0..3
  → csi2_ch0..3
  → /dev/videoN
```

| Link | 字母 | `link_desc.index` | 加串器迁移地址 | sensor_addr 示例 |
|---|---|---|---|---|
| 0 | A | 0 | 0x41 | IMX390/AR0820/IMX490 占位 0x36；ISX031 0x1a |
| 1 | B | 1 | 0x42 | 同上 |
| 2 | C | 2 | 0x43 | 同上 |
| 3 | D | 3 | 0x44 | 同上 |

### 8.3 当前代码的真实限制

- `max96724_imx390_board`：1 个 link（A），IMX390。
- `max96724_ar0820_board`：1 个 link（A），AR0820。
- `max96724_imx490_board`：4 个 link（A-D），**全部是 IMX490**。
- `max96724_isx031_board`：1 个 link（A），ISX031。

也就是说：

- 4 路同型号（IMX490）的 board desc 已经有了；Link A/B 双路已实测，4 路未实测。
- 4 路不同型号的 board desc **还没有**。
- `gmsl-v4l2.c` 的 probe 只解析第一个 endpoint（`fwnode_graph_get_next_endpoint()`），不会解析 4 个 per-link endpoint。
- `gmsl_board_desc` 的 `phy_clk`、`three_gbps`、`skip_frame_sync` 是**板级全局**，不是 per-link。

因此有两种实现路线：

| 路线 | 改动 | 灵活性 | 当前状态 |
|---|---|---|---|
| A. 新增 mixed board_desc + 新 compatible + 新 overlay | 只改 desc/驱动表/overlay | 每种混插组合一个 board desc | 可以在当前代码上做 |
| B. per-link DT endpoint | 改 driver 解析 `port@N`，把 sensor/ser 信息放进 DT | 一个 overlay 描述任意混插 | 目标设计，未实现 |

### 8.4 方案 A：mixed board_desc + 新 overlay（推荐先做）

以 Link A=IMX390、B=AR0820、C=IMX490、D=ISX031 为例。

#### 1）在 `gmsl-desc.c` 增加 link 表和 board desc

```c
static const struct gmsl_link_desc max96724_mix_links[] = {
    {
        .index = 0,
        .sensor = &imx390_desc,
        .ser_ops = &gmsl_ser_auto_ops,
        .sensor_addr = IMX390_SENSOR_ADDR,   /* 0x36 占位 */
        .num_lanes = 4,
        .link_freq = 456000000,
    },
    {
        .index = 1,
        .sensor = &ar0820_desc,
        .ser_ops = &gmsl_ser_auto_ops,
        .sensor_addr = AR0820_SENSOR_ADDR,   /* 0x36 占位 */
        .num_lanes = 4,
        .link_freq = 456000000,
    },
    {
        .index = 2,
        .sensor = &imx490_desc,
        .ser_ops = &gmsl_ser_auto_ops,
        .sensor_addr = IMX490_SENSOR_ADDR,   /* 0x36 占位 */
        .num_lanes = 4,
        .link_freq = 456000000,
    },
    {
        .index = 3,
        .sensor = &isx031_desc,
        .ser_ops = &gmsl_ser_auto_ops,
        .sensor_addr = ISX031_SENSOR_ADDR,   /* 0x1a 真实地址 */
        .num_lanes = 4,
        .link_freq = 288000000,
    },
};

const struct gmsl_board_desc max96724_mix_board = {
    .compatible = "zuojisi,max96724-mix",
    .des_ops = &max96724_des_ops,
    .num_links = ARRAY_SIZE(max96724_mix_links),
    .links = max96724_mix_links,
    /* 注意：phy_clk / three_gbps / skip_frame_sync 是全局的，见 8.6 */
};
```

在 `gmsl-desc.h` 增加：

```c
extern const struct gmsl_board_desc max96724_mix_board;
```

#### 2）在 `gmsl-v4l2.c` 注册

```c
static const struct i2c_device_id gmsl_camera_id[] = {
    /* 现有项... */
    { "max96724_mix", (kernel_ulong_t)&max96724_mix_board },
    {},
};

static const struct of_device_id gmsl_camera_dt_ids[] = {
    /* 现有项... */
    { .compatible = "zuojisi,max96724-mix", .data = &max96724_mix_board },
    { /* sentinel */ }
};
```

#### 3）新建 overlay

复制 `max96724-imx390-overlay.dts`，只改三处：

```dts
cam_node: gmsl@36 {
    compatible = "zuojisi,max96724-mix";
    reg = <0x36>;
    status = "ok";

    clocks = <&cam1_clk>;
    clock-names = "xclk";

    VANA-supply = <&cam1_reg>;
    VDIG-supply = <&cam_dummy_reg>;
    VDDL-supply = <&cam_dummy_reg>;

    port {
        cam_endpoint: endpoint {
            clock-lanes = <0>;
            data-lanes = <1 2 3 4>;
            clock-noncontinuous;
            link-frequencies = /bits/ 64 <800000000>;
        };
    };
};
```

说明：

- `link-frequencies` 是**整条 CSI-2 输出**的频率，不是每个 link 的频率。混插时取所有相机里最高的需求；例如包含 AR0820 4K 时用 800 MHz（1600 Mbps）。
- 4 个 link 的类型在 `max96724_mix_board` 里，不在 overlay 里。
- `config.txt` 用 `dtoverlay=max96724-mix`（文件名对应 `max96724-mix.dtbo`）。
- 在 `arch/arm/boot/dts/overlays/Makefile` 加 `max96724-mix.dtbo`。

#### 4）编译和验证

```bash
make O=../framework-build -j$(nproc) modules dtbs
# 部署 max96724_imx390.ko + max96724-mix.dtbo + config.txt
```

启动后：

```bash
cat /sys/bus/i2c/devices/*/gmsl_links
# 期望：1 1 1 1（或者缺失的 link 为 0）
dmesg | grep -i max96724
```

#### 5）方案 A 的风险

- 4 个 link 的 `sensor_addr`、`ser_ops`、`link_freq` 都在 C 表里，改组合要重新编译内核模块。
- `phy_clk`/`three_gbps`/`skip_frame_sync` 是全局的，混插 3 Gbps 和 6 Gbps 会冲突。
- CFE 仍然只有一条 CSI-2 输出，4 个 VC 共享 D-PHY；不同分辨率的支持需要额外验证。

### 8.5 方案 B：per-link DT endpoint（目标设计）

目标形状（来自 `Documentation/gmsl/ARCHITECTURE.md`）：

```dts
gmsl: gmsl@27 {
    compatible = "zuojisi,max96724-gmsl";
    reg = <0x27>;

    ports {
        #address-cells = <1>;
        #size-cells = <0>;

        port@0 {
            reg = <0>;
            endpoint {
                remote-endpoint = <&imx390_ep>;
                data-lanes = <1 2 3 4>;
                link-frequencies = /bits/ 64 <456000000>;
            };
        };

        port@1 {
            reg = <1>;
            endpoint {
                remote-endpoint = <&ar0820_ep>;
                data-lanes = <1 2 3 4>;
                link-frequencies = /bits/ 64 <800000000>;
            };
        };

        port@2 {
            reg = <2>;
            endpoint {
                remote-endpoint = <&imx490_ep>;
                data-lanes = <1 2 3 4>;
                link-frequencies = /bits/ 64 <456000000>;
            };
        };

        port@3 {
            reg = <3>;
            endpoint {
                remote-endpoint = <&isx031_ep>;
                data-lanes = <1 2 3 4>;
                link-frequencies = /bits/ 64 <288000000>;
            };
        };
    };
};
```

要做这个方案，驱动需要：

1. 遍历 `ports/port@N`，用 `reg` 作为 link index；
2. 每个 port 解析自己的 `link-frequencies`、`data-lanes`、`remote-endpoint`；
3. 为每个 link 找到/创建对应的 sensor subdev 或 sensor desc；
4. 把 `phy_clk`/`three_gbps`/`skip_frame_sync` 也做成 per-link；
5. 重新设计 CFE 的 D-PHY 共享策略（多 link 不同 rate 时）。

**当前没有实现**，不要在现有代码上直接写 4 个 endpoint 然后期待工作。

### 8.6 3 Gbps / 6 Gbps 混插

当前 `gmsl_board_desc` 的三个字段是全局的：

```c
u8 phy_clk;
bool three_gbps;
bool skip_frame_sync;
```

`max96724_init()` 会：

- `three_gbps=true`：写 `0x0010=0x11`、`0x0011=0x11`、`0x0006=0xff`、`0x0018=0x0f`；
- `phy_clk`：写 `0x415/0x418/0x41b/0x41e`（4 个 PHY 共用）；
- `skip_frame_sync`：跳过所有 link 的 GPIO frame sync。

所以：

- 4 路全是 6 Gbps（IMX390/AR0820/IMX490 的常见配置）：可以用方案 A。
- 4 路全是 3 Gbps（ISX031）：可以用方案 A，设置 `three_gbps=true`、`phy_clk=0x25`、`skip_frame_sync=true`。
- ISX031（3 Gbps）+ 6 Gbps 相机混插：**当前不行**。需要把这三个字段搬到 `gmsl_link_desc`，并在 `configure_link()`/`enable_pipes()` 里按 link 应用。这是 8.7/roadmap 的一部分。

### 8.7 不同分辨率/格式的同时出流

CFE 的 4 个 csi2 通道各有 sink/source pad，`csi2_pad_set_fmt()` 是 per-pad 的；但：

- D-PHY 只有一套，第一个开始流的通道决定 `dphy_rate`；
- `gmsl-capture.sh` 为了通过 pipeline 校验，会把 4 个 csi2 sink pad 都设成同一个 `WxH`；
- 不同分辨率/不同 link rate 同时出流，需要手工设置每个 pad，并确认 CFE 能接受共享 D-PHY 的 rate。

实践建议：

1. 先让 4 路输出**相同格式/相同分辨率**，验证 4 路链路和独立启停。
2. 再做不同分辨率，但一次只开一路，确认每路单独可用。
3. 最后尝试两路/多路同时开，观察 `dmesg` 的 CFE 错误、帧计数、丢帧。
4. 如果 CFE 不接受不同 rate，需要改 CFE/D-PHY 或让所有相机切到同一输出模式。

### 8.8 4 路 bring-up 顺序

```text
第 1 步：只接 Link A，确认 gmsl_links=1 0 0 0，单路抓图
第 2 步：只接 Link B，确认 gmsl_links=0 1 0 0，单路抓图
第 3 步：只接 Link C/D，重复
第 4 步：A+B 同时接，确认 1 1 0 0，顺序抓图 + 同时抓图
第 5 步：A+B+C+D，确认 1 1 1 1，先顺序再同时
第 6 步：混插不同型号，先单路，再两路
```

每一步都检查：

- `cat /sys/bus/i2c/devices/*/gmsl_links`
- `dmesg | grep -i -E 'max96724|rp1-cfe'`
- `media-ctl -p -d /dev/mediaN`
- `gmsl-capture.sh -l N -n 10`
- `gmsl-reg links`

### 8.9 测试矩阵

| 组合 | 目标 | 当前状态 |
|---|---|---|
| IMX390 ×1 | 单 link golden | 通过 |
| AR0820 ×1 | 4K 单次冷启动 | 通过；重复 start/stop 待修 |
| IMX490 ×1 | 2880x1860 | 通过 |
| IMX490 ×2（A/B） | 双路顺序 + 同时 | 通过 |
| ISX031 ×1（3 Gbps） | 1920x1536 | 通过 |
| IMX490 ×4 | 4 路同型号 | 未实测 |
| IMX390 + AR0820 + IMX490 + ISX031 | 4 路混插 | 未实现（全局 rate 冲突） |
| 不同分辨率同时出流 | CFE 多格式 | 需额外验证 |

---

## 9. 新增相机模组

这一节以“新增一款 `FOO` 模组”为例，说明从确认拓扑到上板抓图的完整流程。真实代码可参考：

- `gmsl-isx031.c/.h`：sensor 直连、pass-through I2C、3 Gbps；
- `gmsl-imx490.c/.h` 和 `gmsl-desc.c`：sensor 在 ISP 后面、init/start/stop 为空、只配置 SerDes；
- `gmsl-max96717.c/.h`：新增加串器 ops 的模板。

### 9.1 第一步：确认模组拓扑和地址

在现有板子上（或先用厂商驱动点亮）执行：

```bash
# 在 Pi 上
gmsl-reg detect
gmsl-reg scan 0x20 0x70
gmsl-reg read 0x40 0x000d
gmsl-reg read 0x41 0x000d
gmsl-reg read 0x62 0x000d
gmsl-reg read des 0x001a
gmsl-reg read isp 0x0000 8
gmsl-reg dump eeprom 0x0000 0x003f
```

确认：

| 问题 | 怎么看 |
|---|---|
| 加串器型号 | `0x000d`：`0x91`=MAX9295A，`0xbf`=MAX96717，`0xc8`=MAX96717F |
| 加串器出厂地址 | 0x40、0x41、0x62 都试 |
| sensor 是否直连 | sensor 地址能读就是直连；NACK 可能在 ISP 后面 |
| ISP 地址 | 常见 0x6d；读 ID/版本 |
| EEPROM | 0x51；有的模组无响应 |
| link 速率 | 6 Gbps 还是 3 Gbps；看模组/加串器手册 |
| 分辨率/格式 | UYVY? NV12? RAW? 输出给 SerDes 的是什么 |

### 9.2 第二步：新增 `gmsl-<sensor>.c/.h`

#### sensor ops 契约

```c
struct gmsl_sensor_ops {
    const char *name;
    int (*init)(struct gmsl_link *link);
    int (*start_stream)(struct gmsl_link *link, bool on);
    int (*get_fmt)(struct gmsl_link *link, struct v4l2_mbus_framefmt *fmt);
    int (*get_mbus_config)(struct gmsl_link *link,
                           struct v4l2_mbus_config *cfg);
};
```

- **sensor 直连**（如 ISX031）：在 `init` 里写 sensor 寄存器、释放 reset、配置 lane/模式；`start_stream` 写 stream on/off。
- **sensor 在 ISP 后面**（如 IMX390/AR0820/IMX490）：`init`/`start_stream` 可以为空；host 只配置 SerDes，ISP 自己负责 sensor。`get_fmt` 返回 `link->fmt`，`get_mbus_config` 返回 `link->dev->mbus_config`。

#### 最小模板

```c
// gmsl-foo.c
#include "gmsl-core.h"
#include "gmsl-foo.h"

static int foo_sensor_init(struct gmsl_link *link)
{
    struct gmsl_dev *gmsl = link->dev;

    if (!link->desc->init || !link->desc->init_len)
        return 0;

    return gmsl_load_regs(gmsl, link->sensor_addr,
                          link->desc->init, link->desc->init_len);
}

static int foo_start_stream(struct gmsl_link *link, bool on)
{
    struct gmsl_dev *gmsl = link->dev;
    const struct gmsl_reg_value *regs;
    unsigned int len;

    regs = on ? link->desc->start : link->desc->stop;
    len = on ? link->desc->start_len : link->desc->stop_len;
    if (!regs || !len)
        return 0;

    return gmsl_load_regs(gmsl, link->sensor_addr, regs, len);
}

static int foo_get_fmt(struct gmsl_link *link,
                       struct v4l2_mbus_framefmt *fmt)
{
    *fmt = link->fmt;
    return 0;
}

static int foo_get_mbus_config(struct gmsl_link *link,
                               struct v4l2_mbus_config *cfg)
{
    *cfg = link->dev->mbus_config;
    if (!cfg->link_freq)
        cfg->link_freq = link->link_freq;
    return 0;
}

const struct gmsl_sensor_ops foo_sensor_ops = {
    .name = "foo",
    .init = foo_sensor_init,
    .start_stream = foo_start_stream,
    .get_fmt = foo_get_fmt,
    .get_mbus_config = foo_get_mbus_config,
};
```

头文件：

```c
// gmsl-foo.h
#ifndef __GMSL_FOO_H__
#define __GMSL_FOO_H__
#include "gmsl-core.h"
extern const struct gmsl_sensor_ops foo_sensor_ops;
#endif
```

#### 寄存器序列

如果 sensor 直连，把 init/start/stop 序列写成 `gmsl_reg_value[]`：

```c
static const struct gmsl_reg_value foo_init_regs[] = {
    { 0x0000, 0x01 },
    { 0x0001, 0x02, 0x00, 10 }, /* mask=0 表示整字节写；delay_ms=10 */
};

static const struct gmsl_reg_value foo_start_regs[] = {
    { 0x0100, 0x01 },
};

static const struct gmsl_reg_value foo_stop_regs[] = {
    { 0x0100, 0x00 },
};
```

如果 sensor 在 ISP 后面，这些数组可以为 `NULL`，由模组 ISP 负责；host 只配置 `MAX96724`/加串器的 pipe、VC、DT、lane。

### 9.3 第三步：在 `gmsl-desc.c` 增加 sensor_desc

```c
static const struct gmsl_sensor_desc foo_desc = {
    .compatible = "vendor,foo",
    .name = "foo",
    .width = 1920,
    .height = 1080,
    .mbus_code = MEDIA_BUS_FMT_UYVY8_1X16,
    .csi_dt = 0x1e,              /* YUV422 8-bit */
    .sensor_ops = &foo_sensor_ops,
    .init = foo_init_regs,
    .init_len = ARRAY_SIZE(foo_init_regs),
    .start = foo_start_regs,
    .start_len = ARRAY_SIZE(foo_start_regs),
    .stop = foo_stop_regs,
    .stop_len = ARRAY_SIZE(foo_stop_regs),
};
```

要点：

- `width/height` 必须和模组实际输出一致；抓图脚本的 `-s` 也要一致。
- `mbus_code` 当前项目统一是 `MEDIA_BUS_FMT_UYVY8_1X16`；如果模组输出 RAW/NV12，CFE/抓图脚本需要相应修改。
- `csi_dt = 0x1e` 是 YUV422 8-bit；RAW 模组要改成对应 DT，并且 `get_frame_desc()` 会返回这个 DT。
- `sensor_addr` 对 ISP 后面的模组是占位；对直连 sensor 是真实地址（ISX031 是 0x1a）。

### 9.4 第四步：选择/新增加串器 ops

优先复用：

- `max9295_ser_ops`：DEV_ID 0x91；
- `max96717_ser_ops`：DEV_ID 0xbf / 0xc8；
- `gmsl_ser_auto_ops`：运行时按 `0x000d` 自动选。

新增加串器时，实现：

```c
struct gmsl_ser_ops {
    const char *name;
    u8 id;
    int (*probe)(struct gmsl_link *link);
    int (*configure)(struct gmsl_link *link);
    int (*gpio_frame_sync)(struct gmsl_link *link);
};
```

`probe` 要处理：

- 默认地址 0x40/0x62；
- 读 `0x000d` 校验 ID；
- 写 `0x0000` 把地址迁移到 `0x41 + link`；
- 迁移失败时保留原地址；
- 设置 `link->ser_addr`。

`configure` 要处理：

- pipe/VC/DT（`0x0318`/`0x0314`、`0x0311`、`0x0331` 等，按芯片手册）；
- 4-lane；
- 可选的 GPIO frame sync。

### 9.5 第五步：增加 link_desc / board_desc

单相机：

```c
static const struct gmsl_link_desc max96724_foo_links[] = {
    {
        .index = 0,
        .sensor = &foo_desc,
        .ser_ops = &gmsl_ser_auto_ops,
        .sensor_addr = FOO_SENSOR_ADDR,
        .num_lanes = 4,
        .link_freq = 456000000,
    },
};

const struct gmsl_board_desc max96724_foo_board = {
    .compatible = "zuojisi,max96724-foo",
    .des_ops = &max96724_des_ops,
    .num_links = ARRAY_SIZE(max96724_foo_links),
    .links = max96724_foo_links,
};
```

4 路同型号：在 `links[]` 里写 index 0..3，sensor 都指向 `&foo_desc`，参考 `max96724_imx490_links`。

4 路混插：参考 8.4 的 `max96724_mix_links`。

`gmsl-desc.h` 加 `extern`。

### 9.6 第六步：注册驱动、overlay、Makefile

1. `gmsl-v4l2.c` 的 `i2c_device_id` 和 `of_device_id` 增加新 board；
2. 新建 `max96724-foo-overlay.dts`，`compatible = "zuojisi,max96724-foo"`，`link-frequencies` 按模组设置；
3. `arch/arm/boot/dts/overlays/Makefile` 增加 `max96724-foo.dtbo`；
4. `drivers/media/i2c/Makefile` 的 `max96724_imx390-y` 增加 `gmsl-foo.o`；
5. 重新编译 `modules dtbs`，部署 `max96724_imx390.ko` + `max96724-foo.dtbo` + `config.txt`；
6. 重启，检查 `gmsl_links` 和 `dmesg`。

### 9.7 第七步：上板测试

```bash
# 1. 确认 link
cat /sys/bus/i2c/devices/*/gmsl_links
dmesg | grep -i max96724

# 2. 确认拓扑
media-ctl -p -d /dev/mediaN

# 3. 抓图
./tools/gmsl/gmsl-capture.sh -l 0 -n 30 -s 1920x1080 -r

# 4. 寄存器确认
gmsl-reg links
gmsl-reg read des 0x001a
gmsl-reg read ser 0x000d
```

### 9.8 新增模组常见坑

| 问题 | 根因 | 处理 |
|---|---|---|
| link 不锁 | 模组是 3 Gbps，MAX96724 还在 6 Gbps | `three_gbps=true`，`phy_clk=0x25`，先开 link enable 再读 lock |
| serializer 找不到 | 出厂地址是 0x62 | 用 `gmsl_ser_auto_ops`，或手动探测 0x62 |
| 地址迁移失败 | 模组不接受写 0x0000 | 保留原地址，`link->ser_addr = src` |
| sensor NACK | sensor 在 ISP 后面 | 这是正常的；改读 ISP 0x6d |
| pass-through 读不到 | 没使能 serializer 的 pass-through/reset | 参考 `isx031_ser_init()`：0x0001、0x0003、0x0006、0x03f0/0x03f1、0x02be |
| 有 link 无图 | DT/VC/pipe 不匹配 | 检查 `configure_link()` 的 `0x40*i` 偏移和 `csi_dt` |
| 颜色不对 | 输出不是 UYVY，或 stride 错 | 确认 mbus_code、DT、抓图脚本的 `-s` |
| 帧率低 | link rate/PHY clock 不对 | 看 CFE `Using a link rate of ...`，3 Gbps 用 0x25 |
| 第二次启动失败 | 4K CFE/DMA 重启问题 | 已知限制，先用单次抓图/连续流 |

---

## 10. 调试工具与命令

### 10.1 基础检查

```bash
# 内核版本、启动参数
uname -a
cat /proc/cmdline

# 模块
lsmod | grep -E 'max96724|rp1'
modinfo max96724_imx390
modinfo rp1-cfe-downstream

# 设备节点
ls -l /dev/media* /dev/video*
v4l2-ctl --list-devices

# link 状态
cat /sys/bus/i2c/devices/*/gmsl_links

# 日志
dmesg | grep -i -E 'max96724|rp1-cfe|gmsl' | tail -80
```

`gmsl_links` 的格式是 4 个字符 + 空格：

```text
1 0 0 0    # 仅 Link A locked
1 1 0 0    # Link A/B locked
1 1 1 1    # 四路 locked
```

### 10.2 media-ctl：看拓扑、拉 link、设格式

查看拓扑：

```bash
media-ctl -p -d /dev/media0
```

重点实体：

```text
- entity ...: max96724-imx390 (4 pads, MEDIA_ENT_F_CAM_SENSOR)
- entity ...: csi2 (8 pads, MEDIA_ENT_F_VID_IF_BRIDGE)
- entity ...: rp1-cfe-csi2_ch0
- entity ...: rp1-cfe-csi2_ch1
- entity ...: rp1-cfe-csi2_ch2
- entity ...: rp1-cfe-csi2_ch3
```

`max96724` 的 4 个 source pad 会由 CFE 驱动自动连到 `csi2` 的 4 个 sink pad（`cfe_link_node_pads()`），一般不需要手工创建。手工需要做的是：

1. 把 `csi2` 的 source pad（4..7）连到目标 `rp1-cfe-csi2_chN` 的 sink pad 0；
2. 设置 `csi2` 的 4 个 sink pad 格式；
3. 设置视频节点格式。

link 语法（link 0 为例）：

```bash
media-ctl -d /dev/media0 \
  -l '"csi2":4->"rp1-cfe-csi2_ch0":0[1]'
```

- `"csi2":4`：`csi2` 子设备的 source pad 4，对应 channel 0；
- `"rp1-cfe-csi2_ch0":0`：视频实体 sink pad 0；
- `[1]`：启用。

link 1/2/3 分别是 `"csi2":5/6/7` → `"rp1-cfe-csi2_ch1/2/3":0`。

设置格式：

```bash
for p in 0 1 2 3; do
  media-ctl -d /dev/media0 \
    -V "\"csi2\":$p[fmt:UYVY8_1X16/1920x1080 field:none]"
done
```

注意：抓图脚本会把 4 个 sink pad 都设成同一格式，这是为了通过 CFE pipeline 校验；未使用的通道也必须有合法格式。

### 10.3 v4l2-ctl：看格式、抓帧

```bash
# 看视频节点能力/格式
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --get-fmt-video

# 设格式
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=1920,height=1080,pixelformat=UYVY

# 抓 60 帧到 raw 文件
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=60 \
  --stream-to=/tmp/cap.raw

# 抓 10 帧到 stdout（给 ffmpeg/ffplay）
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=10 \
  --stream-to=-
```

常见问题：

- `VIDIOC_STREAMON` 返回 `Broken pipe` 或 `-32`：media pipeline 没建好、格式不匹配、link 没启用。
- 抓到的文件比 `W*H*2*frames` 小：丢帧或提前停止；看 `dmesg` 和 `--stream-count`。
- 视频节点编号变化：用 `media-ctl -p` 或 `gmsl-capture.sh` 自动找，不要写死。

### 10.4 `gmsl-reg`：用户态寄存器读写

构建/安装：

```bash
cd tools/gmsl
make
sudo make install
# 交叉编译：make CC=aarch64-linux-gnu-gcc
```

运行用户需要在 `i2c` 组，或用 `sudo`。

常用命令：

```bash
# 自动从 sysfs 找 max96724 驱动所在总线，探测设备
gmsl-reg detect

# 扫描 0x08..0x77
gmsl-reg scan
gmsl-reg scan 0x20 0x70

# 读寄存器（16 位地址，8 位数据）
gmsl-reg read des 0x0000
gmsl-reg read ser 0x000d
gmsl-reg read isp 0x0000 8
gmsl-reg read sensor 0x0000

# 写寄存器（会影响运行中的链路，先停流）
gmsl-reg write des 0x0006 0xf1
gmsl-reg write ser 0x0318 0x5e

# dump
gmsl-reg dump des 0x0000 0x003f
gmsl-reg dump ser 0x0300 0x033f
gmsl-reg dump eeprom 0x0000 0x003f

# 查看 link lock 和 0x0006 路由掩码
gmsl-reg links

# 交互模式
gmsl-reg -i
```

选项：

```text
-b, --bus N        指定 I2C 总线
-l, --link N       选择 link 0..3，仅影响 ser 别名地址
-g, --reg-bits N   8 或 16 位寄存器地址（默认 16）
-t, --retries N    NACK 重试次数（默认 2）
-i, --interactive  交互 shell
```

地址别名：

| 别名 | 地址 | 说明 |
|---|---|---|
| `des` | 0x27 | MAX96724 |
| `ser` | 0x41 + link | 加串器；`-l` 改 link |
| `isp` | 0x6d | GW5200/GW5300 |
| `eeprom` | 0x51 | 模组 EEPROM |
| `sensor` | 0x21 | IMX390；在 ISP 后面，通常 NACK |
| `sensor`（ISX031） | 0x1a | 直连 pass-through，可读写 |

注意：

- 工具绕过驱动的互斥/runtime PM；**写寄存器前先停止出流**。
- 多 link 同时访问时，`0x0006` 路由需要指向目标 link；`-l` 只改别名，不改路由。
- IMX390/AR0820/IMX490 的 sensor 在 ISP 后面，`sensor` NACK 是正常的；用 `isp` 访问 GW5200/GW5300。
- 如果 `0x40`/`0x62` 能读但 `0x41` 不能读，说明地址还没迁移；先让驱动 probe，或手动读原始地址。

### 10.5 i2c-tools（可选）

`gmsl-reg` 已经覆盖大部分需求；如果习惯 `i2c-tools`：

```bash
# 找总线
ls /sys/bus/i2c/devices/i2c-*/name
i2cdetect -y -r <bus>

# 读 16 位寄存器地址（MAX96724 0x0000）
i2ctransfer -y <bus> w2@0x27 0x00 0x00 r1
```

注意：很多 GMSL 设备是 16 位寄存器地址、8 位数据；`i2cget` 的 `w` 是 16 位数据，语义不同，容易读错。优先用 `gmsl-reg`。

### 10.6 动态调试和 CFE verbose

GMSL 驱动没有模块参数，使用内核 dynamic debug：

```bash
# 确认 debugfs 已挂载
mount | grep debugfs

# 打开 GMSL 模块所有 dev_dbg
echo 'module max96724_imx390 +p' | sudo tee \
  /sys/kernel/debug/dynamic_debug/control

# 或按文件
echo 'file gmsl-core.c +p' | sudo tee -a \
  /sys/kernel/debug/dynamic_debug/control
echo 'file gmsl-v4l2.c +p' | sudo tee -a \
  /sys/kernel/debug/dynamic_debug/control

# 看当前规则
grep -E 'gmsl|max96724' /sys/kernel/debug/dynamic_debug/control | head
```

CFE 驱动有模块参数：

```bash
# 重新加载 downstream CFE，打开 verbose 和 CSI2 错误统计
sudo modprobe -r rp1-cfe-downstream
sudo modprobe rp1-cfe-downstream verbose_debug=1 track_csi2_errors=1

# 或运行时打开 verbose（权限 0644）
echo 1 | sudo tee /sys/module/rp1_cfe_downstream/parameters/verbose_debug
```

日志会包含：

```text
rp1-cfe ...: Using a link rate of 912 Mbps
rp1-cfe ...: Configuring CSI-2 block - 4 data lanes
max96724-imx390: enable streams pad=0
max96724-imx390: disable streams pad=0
```

### 10.7 故障排查树

按顺序往下查，不要跳步：

```text
1. overlay 加载了吗？
   dtoverlay -l
   ls /proc/device-tree/ | grep -i gmsl
   dmesg | grep -i 'max96724.*probing'

2. I2C client 创建了吗？
   ls /sys/bus/i2c/devices/
   dmesg | grep -i 'i2c'
   gmsl-reg detect

3. link 锁了吗？
   cat /sys/bus/i2c/devices/*/gmsl_links
   gmsl-reg links
   dmesg | grep -i 'link lock'

4. 加串器认到了吗？
   gmsl-reg scan 0x20 0x70
   gmsl-reg read 0x40 0x000d
   gmsl-reg read 0x62 0x000d
   dmesg | grep -i 'serializer'

5. media/video 节点有吗？
   media-ctl -p -d /dev/mediaN
   ls /dev/video*
   dmesg | grep -i 'rp1-cfe'

6. 能开流吗？
   media-ctl -l ...
   v4l2-ctl --set-fmt-video=...
   v4l2-ctl --stream-mmap --stream-count=10 --stream-to=/tmp/x.raw
   dmesg | tail -50

7. 图正常吗？
   文件大小 = W*H*2*frames？
   PNG 颜色/方向？
   Y/U/V 统计是否合理？
```

常见现象对照：

| 现象 | 优先检查 |
|---|---|
| `gmsl_links = 0 0 0 0` | 12V PoC、FAKRA、同轴、link enable、3 Gbps 配置 |
| `deserializer NOT detected` | I2C 总线、0x27、overlay 的 `i2c_csi_dsi`/`i2c_vc` |
| `serializer NOT detected` | 0x40/0x62/0x41、地址迁移、模组供电 |
| `no camera detected` | link 没锁、模组没出流 |
| `Failed to start media pipeline: -32` | csi2 pad 格式、未使用 pad 格式、media link |
| `VIDIOC_STREAMON: Broken pipe` | 同上，或 sensor/ISP 没开流 |
| 抓到 0 字节 | 视频节点选错、link 没使能、CFE 没收到数据 |
| 图像全黑/全绿 | sensor 没出图、pipe/VC 错、DT 错 |
| 颜色错/花屏 | UYVY/YUYV、stride、PNG 转换参数 |
| `error: slave=...` | I2C NACK、地址迁移失败、远端设备不在线 |
| 第二次 4K 启动失败 | 已知 CFE/DMA 4K 重启问题 |
| `supply DOVDD not found` | overlay/驱动电源命名不一致，dummy regulator；一般不影响出图 |

---

## 11. 出图与录制

### 11.1 `gmsl-capture.sh` 一键抓图

用法：

```bash
./tools/gmsl/gmsl-capture.sh [options]

-l, --link N       GMSL link: 0=A, 1=B, 2=C, 3=D（默认 auto）
-n, --frames N     抓帧数（默认 60）
-o, --output FILE  raw UYVY 输出（默认 $HOME/gmsl-capture.raw）
-p, --png FILE     最后一帧 PNG（默认 $HOME/gmsl-capture.png）
-s, --size WxH     尺寸（默认 1920x1080）
-r, --rotate       PNG 旋转 180°
-h, --help         帮助
```

示例：

```bash
# IMX390 1920x1080
./tools/gmsl/gmsl-capture.sh -r

# AR0820 4K
./tools/gmsl/gmsl-capture.sh -s 3840x2160 -n 30 \
  -o /tmp/ar0820.raw -p /tmp/ar0820.png -r

# IMX490 2880x1860
./tools/gmsl/gmsl-capture.sh -s 2880x1860 -n 30 \
  -o /tmp/imx490.raw -p /tmp/imx490.png -r

# ISX031 1920x1536
./tools/gmsl/gmsl-capture.sh -s 1920x1536 -n 30 \
  -o /tmp/isx031.raw -p /tmp/isx031.png -r

# 指定 link（多路时必须显式指定）
./tools/gmsl/gmsl-capture.sh -l 1 -n 60 -o /tmp/b.raw -p /tmp/b.png
```

脚本内部做的事：

1. 从 `/sys/bus/i2c/devices/*/gmsl_links` 读 4 个 link 状态；
2. 如果 `-l auto`，要求恰好只有一个 `1`，否则报错；
3. 遍历 `/dev/media*`，找 `driver rp1-cfe`；
4. 从 media topology 找 `rp1-cfe-csi2_chN` 对应的 `/dev/videoN`；
5. 先 disable 所有 `csi2`→`ch` 的 link，避免 CFE 保留“enabled-but-not-streaming”的旧通道；
6. enable 目标 `"csi2":4+N -> "rp1-cfe-csi2_chN":0[1]`；
7. 把 4 个 `csi2` sink pad 都设成 `UYVY8_1X16/WxH field:none`；
8. 设置视频节点 UYVY WxH；
9. `v4l2-ctl --stream-mmap --stream-count=N --stream-to=raw`；
10. 统计最后一帧 Y/U/V，可选用 ffmpeg 生成 PNG（`-r` 做 hflip+vflip）。

依赖：`v4l-utils`、`ffmpeg`、`python3`。

### 11.2 手工抓图（理解脚本在做什么）

以 link 0、1920x1080 为例：

```bash
MEDIA=/dev/media2          # 用 media-ctl -p 找 rp1-cfe 对应的 media
VIDEO=/dev/video0          # 用 media-ctl -p 找 csi2_ch0 对应的 video
W=1920
H=1080

# 1) 只保留目标通道，先关闭所有 csi2→ch link
for p in 0 1 2 3; do
  media-ctl -d $MEDIA -l "\"csi2\":$((4+p))->\"rp1-cfe-csi2_ch$p\":0[0]" || true
done

# 2) 打开 link 0
media-ctl -d $MEDIA -l '"csi2":4->"rp1-cfe-csi2_ch0":0[1]'

# 3) 设置 4 个 csi2 sink pad 格式
for p in 0 1 2 3; do
  media-ctl -d $MEDIA -V "\"csi2\":$p[fmt:UYVY8_1X16/${W}x${H} field:none]"
done

# 4) 设置视频节点格式
v4l2-ctl -d $VIDEO --set-fmt-video=width=$W,height=$H,pixelformat=UYVY

# 5) 抓 60 帧
v4l2-ctl -d $VIDEO --stream-mmap --stream-count=60 --stream-to=/tmp/cap.raw

# 6) 检查大小
ls -l /tmp/cap.raw
# 期望：60 * W * H * 2 字节
```

### 11.3 生成 PNG

从 raw 的最后一帧生成 PNG：

```bash
W=1920; H=1080; N=60
FRAME=$((W*H*2))
dd if=/tmp/cap.raw of=/tmp/last.raw bs=$FRAME skip=$((N-1)) count=1 status=none
ffmpeg -hide_banner -loglevel error -y \
  -f rawvideo -pix_fmt uyvy422 -s ${W}x${H} -i /tmp/last.raw \
  -frames:v 1 /tmp/cap.png
```

正立（相机 180° 倒装）：

```bash
ffmpeg -hide_banner -loglevel error -y \
  -f rawvideo -pix_fmt uyvy422 -s ${W}x${H} -i /tmp/last.raw \
  -vf hflip,vflip -frames:v 1 /tmp/cap-upright.png
```

### 11.4 录制 MP4/H.264

把 `v4l2-ctl` 的 raw 流直接管道给 ffmpeg：

```bash
VIDEO=/dev/video0
W=1920; H=1080; FPS=30; SEC=10

v4l2-ctl -d $VIDEO \
  --set-fmt-video=width=$W,height=$H,pixelformat=UYVY \
  --stream-mmap --stream-count=$((FPS*SEC)) --stream-to=- | \
ffmpeg -hide_banner -loglevel warning -y \
  -f rawvideo -pix_fmt uyvy422 -s ${W}x${H} -framerate $FPS -i - \
  -c:v libx264 -preset ultrafast -qp 23 -pix_fmt yuv420p \
  /tmp/record.mp4
```

说明：

- `--stream-to=-` 把 raw 写到 stdout；`v4l2-ctl` 的日志走 stderr，不会污染视频数据。
- `-framerate` 用实测帧率（IMX390 约 28–29 fps，IMX490 约 26 fps，AR0820 约 25 fps）；不匹配会导致播放速度不对。
- 先短录 10 秒验证，再延长。
- Pi 5 没有硬件 H.264 编码器；x264 会占 CPU。4K 建议先录 raw 再离线转码，或降低分辨率/帧率。
- 如果 ffmpeg 提前退出，`v4l2-ctl` 可能收到 SIGPIPE；用 `-t`/`-frames:v` 限制，或先录 raw。

GStreamer 方式（可选，先确保 media link/格式已设置）：

```bash
gst-launch-1.0 -e \
  v4l2src device=/dev/video0 num-buffers=300 \
  ! video/x-raw,format=UYVY,width=1920,height=1080,framerate=30/1 \
  ! videoconvert \
  ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=8000 \
  ! h264parse ! mp4mux ! filesink location=/tmp/record-gst.mp4
```

### 11.5 实时预览

```bash
VIDEO=/dev/video0
W=1920; H=1080

v4l2-ctl -d $VIDEO \
  --set-fmt-video=width=$W,height=$H,pixelformat=UYVY \
  --stream-mmap --stream-to=- | \
ffplay -hide_banner -f rawvideo -pix_fmt uyvy422 \
  -s ${W}x${H} -framerate 30 -
```

预览会占 CPU，调试时可以用 `-framerate 15` 或缩小分辨率。

### 11.6 多路同时出图

`gmsl-capture.sh` 的 auto 模式只适合“只有一路 locked”；多路时要显式 `-l`，而且脚本会先 disable 所有通道，不适合同时抓。双路同时抓参考（以 A/B 为例）：

```bash
MEDIA=/dev/media2
VID0=/dev/video0      # csi2_ch0
VID1=/dev/video1      # csi2_ch1
W=2880; H=1860

# 先设置所有 csi2 sink pad（两路同格式）
for p in 0 1 2 3; do
  media-ctl -d $MEDIA -V "\"csi2\":$p[fmt:UYVY8_1X16/${W}x${H} field:none]"
done

# 打开两路
media-ctl -d $MEDIA -l '"csi2":4->"rp1-cfe-csi2_ch0":0[1]'
media-ctl -d $MEDIA -l '"csi2":5->"rp1-cfe-csi2_ch1":0[1]'

# 设置两个视频节点
v4l2-ctl -d $VID0 --set-fmt-video=width=$W,height=$H,pixelformat=UYVY
v4l2-ctl -d $VID1 --set-fmt-video=width=$W,height=$H,pixelformat=UYVY

# 同时抓 10 帧
v4l2-ctl -d $VID0 --stream-mmap --stream-count=10 --stream-to=/tmp/a.raw &
v4l2-ctl -d $VID1 --stream-mmap --stream-count=10 --stream-to=/tmp/b.raw &
wait

ls -l /tmp/a.raw /tmp/b.raw
```

双 IMX490 已实测：Link A/B、各 10 帧、2880x1860 成功。4 路同时抓尚未做完整矩阵测试。

注意：

- 不同分辨率时，4 个 csi2 sink pad 应该按各自分辨率设置；第一个开始流的通道决定共享 D-PHY rate。
- 如果两路 rate/format 不兼容，CFE 可能报错或只出一路；先让两路同格式。
- 抓完一路后，脚本/手工要把对应 `csi2`→`ch` link disable，避免 CFE 保留 enabled-but-not-streaming 状态。

### 11.7 带宽和存储

UYVY 是 2 字节/像素：

| 分辨率 | 每帧字节 | 30 fps 带宽 |
|---|---:|---:|
| 1920x1080 | 4,147,200（约 3.96 MiB） | 约 124 MB/s |
| 2880x1860 | 10,713,600（约 10.2 MiB） | 约 321 MB/s |
| 3840x2160 | 16,588,800（约 15.8 MiB） | 约 498 MB/s |

四路 1080p30 约 498 MB/s，建议写 NVMe/SSD，不要写 SD 卡。录制 MP4 可以大幅降低存储，但 CPU 编码会成为瓶颈；4K 建议先 raw 再离线转码。

---

## 12. 测试与验收

### 12.1 单 link golden test

以 IMX390 Link A 为例：

```bash
# 1) 状态
cat /sys/bus/i2c/devices/*/gmsl_links
# 期望：1 0 0 0

# 2) 日志
dmesg | grep -i max96724 | tail -20
# 期望：deserializer detected / link lock status=0x01 / serializer detected / probe success

# 3) CFE link rate
dmesg | grep -i 'Using a link rate'
# 期望：912 Mbps（IMX390）

# 4) 抓图
./tools/gmsl/gmsl-capture.sh -l 0 -n 120 -r
# 期望：120 帧、约 28–29 fps、PNG 正立、无 error: slave

# 5) 二次启动
./tools/gmsl/gmsl-capture.sh -l 0 -n 60 -r
# 期望：仍能出图
```

验收标准：

- `gmsl_links` 对应 bit 为 1；
- `dmesg` 无 `error: slave`（偶发 1~2 次可接受，持续 NACK 不行）；
- CFE link rate 与 DT `link-frequencies` 一致；
- 抓图帧数 = `--stream-count`；
- PNG 颜色/方向正常；
- 第二次启动仍能出图（4K 除外，见已知限制）。

### 12.2 每个 link 单独测

```bash
# 只接 Link A
gmsl-capture.sh -l 0 -n 30

# 只接 Link B
gmsl-capture.sh -l 1 -n 30

# 只接 Link C
gmsl-capture.sh -l 2 -n 30

# 只接 Link D
gmsl-capture.sh -l 3 -n 30
```

每次检查：

```bash
cat /sys/bus/i2c/devices/*/gmsl_links
dmesg | grep -i -E 'link [A-D]|serializer|link lock'
```

### 12.3 双路/四路

双路（以 A/B 为例）：

```bash
# 同时接 A/B
cat /sys/bus/i2c/devices/*/gmsl_links
# 期望：1 1 0 0

# 顺序抓
gmsl-capture.sh -l 0 -n 10
gmsl-capture.sh -l 1 -n 10

# 同时抓（手工，见 11.6）
```

四路：

```bash
# 期望：1 1 1 1
# 顺序抓 A/B/C/D，再两两同时，最后四路同时
```

### 12.4 性能指标

| 指标 | 怎么看 | 期望 |
|---|---|---|
| link lock | `gmsl_links` | 对应 bit=1 |
| CFE link rate | `dmesg` | 912/1600/576 Mbps，与 DT 一致 |
| 帧率 | 抓图时间/帧数 | IMX390 ~28–29 fps；IMX490 ~26 fps；AR0820 ~25 fps |
| 丢帧 | 文件大小 vs `W*H*2*N` | 一致 |
| I2C 错误 | `dmesg` 中 `error: slave` 的数量 | 0 或极少 |
| 图像质量 | PNG/YUV 统计 | 不是全黑/全绿/花屏 |
| 4K 重复启动 | 第二次抓图 | 目前已知会失败，见 12.5 |

### 12.5 已知问题与验收边界

| 问题 | 状态 | 验收建议 |
|---|---|---|
| AR0820 4K stop/start 第二次只收约 4 buffer | 已知，未修 | 单次冷启动抓图/连续流验收；不要以“反复 start/stop 4K”作为通过标准 |
| 4 路 mixed-link 未实测 | 待补 | 先做同型号 4 路，再做混插 |
| 3 Gbps/6 Gbps 混插 | 未支持 | 先全部同 rate |
| 不同分辨率同时出流 | 待验证 | 先同格式，再多路 |
| hot-plug/link loss | 未实现 | 拔线后重新上电验证，不要求自动恢复 |
| FE 路径 per-pad stop | 未实现 | 只用 CSI2 直通路径验收 |
| sensor 在 ISP 后面 | 设计如此 | 用 ISP 0x6d 验收，不要求直读 sensor |
| 无 V4L2 controls | 设计如此 | 用模组 AE/应用软件旋转 |
| 电源 dummy regulator | 当前行为 | 不影响出图；要电源控制再统一命名 |

---

## 13. 适配过程记录（时间线）

这一节是项目适配过程的摘要；完整细节见 `Documentation/gmsl/CHANGELOG.md`。

### 13.1 2026-11-15：从厂商 6.6 移植到 6.18

背景：

- 厂商原始资料是 `rpi-6.6.y`（6.6.78），包含一整套 `z_xcdes*` 私有驱动和 overlay。
- 目标：只保留 SG2-IMX390C 这一路，移植到 `raspberrypi/linux` `rpi-6.18.y`，基线 commit `ea17ae02ca28483e508b05c7c44190c01a1ef8eb`，版本 6.18.62。

主要工作：

- 6.18 V4L2 API 适配：
  - `v4l2_subdev_get_try_format()` → `v4l2_subdev_state_get_format()`；
  - `v4l2_subdev_get_try_crop()` → `v4l2_subdev_state_get_crop()`；
  - `pad_ops.init_cfg` → `internal_ops.init_state`；
  - `video_ops.g_frame_interval/s_frame_interval` → `pad_ops.get_frame_interval/set_frame_interval`；
  - 增加 `v4l2_subdev_init_finalize()` / `v4l2_subdev_cleanup()`。
- 保留 `v4l2_async_register_subdev_sensor()`、4 个 source pad（pad=link=VC）、`get_frame_desc()` 返回 `DT=0x1e, VC=pad`。
- 驱动重命名：`z_xcdes40_sg2_imx390c_5200_gmsl2` → `max96724_imx390`；模块、overlay、compatible、sysfs、日志前缀统一。
- 修复：
  - `set_fmt()` 重复 `mutex_unlock()`；
  - serializer 地址写 NACK（先静默读 0x40，再决定是否迁移）；
  - CFE 一直回退 999 Mbps（改用 `v4l2_fwnode_endpoint_alloc_parse()` 解析 `link-frequencies`，实现 `get_mbus_config()`）；
  - 日志降噪（寄存器读写、get_fmt 等改 `dev_dbg`）；
  - `check_version()` 失败恢复 I2C 地址；
  - I2C 瞬态 NACK 3 次重试；
  - overlay 去掉无效的 `rotation`/`orientation`。
- 新增 `tools/gmsl/gmsl-capture.sh`，一条命令完成 link/media/video 自动查找、格式设置、抓帧、PNG。
- 首次上板 golden test 通过：Link A、912 Mbps、1920x1080 UYVY、约 28–29 fps、PNG 正常。

### 13.2 2026-11-17：Phase 1 分层框架

把单体 `max96724_imx390.c` 拆成：

- `gmsl-core.c/.h`：`gmsl_dev`/`gmsl_link`、I2C helper/重试、`gmsl_load_regs`、ops 调度；
- `gmsl-v4l2.c`：4-pad V4L2 subdev、fmt/frame_desc/mbus_config/s_stream；
- `gmsl-max96724.c/.h`：解串器 ops；
- `gmsl-max9295.c/.h`：加串器 ops；
- `gmsl-imx390.c/.h`：sensor ops；
- `gmsl-desc.c/.h`：描述符表。

保持模块名、compatible、overlay、sysfs、寄存器序列不变；`W=1` 编译通过，上板 golden test 通过。

### 13.3 Phase 2 + P0：board/link/sensor 拆分和 per-link streaming

Phase 2：

- 引入 `gmsl_board_desc`、`gmsl_link_desc`、`gmsl_sensor_desc`；
- `link_desc.index` 允许 0..3 稀疏映射；
- 未使用 link 继承首个已配置 link 的默认格式，保证 4 个 source pad 对 CFE 合法；
- `set_fmt` 改为 per-link；
- probe 的 match data 从 sensor desc 改为 board desc；
- 模块名/compatible/overlay 保持不变。

P0 per-link streaming：

- `gmsl_core_s_stream()` 接受 `link_mask`，维护 `streaming_mask`；
- 单 link probe/sensor init 失败只清该 link 的 lock bit；
- V4L2 增加 pad `enable_streams/disable_streams`（非 `FL_STREAMS`，每个 pad 视为 stream 0）；
- CFE csi2 直通路径按 `node->id` 调 pad streams；第一个通道打开 CSI RX，最后一个关闭；
- MAX96724 pipe enable 改为 mask 语义；
- 单 link 实测：120 帧 + 二次启动 60 帧、912 Mbps、约 28–29 fps、无 `error: slave`。

### 13.4 2026-11-21：多模组支持

AR0820：

- `gmsl-ar0820.c/.h`：3840x2160 UYVY；
- `gmsl-max96717.c/.h`：MAX96717/96717F；
- `gmsl-ser-auto.c/.h`：按 `0x000d` 自动识别；
- overlay `max96724-ar0820`，link-frequencies 800 MHz（1600 Mbps）；
- 实测：MAX9295A（0x91）、EEPROM reg0=0x17、单次冷启动 4K 抓图成功；已知 4K stop/start 第二次失败。

IMX490：

- `gmsl-imx490.c/.h`：2880x1860 UYVY；
- `max96724_imx490_board`：PHY clock 0x32，Link A-D；
- 实测：serializer 0x41/0x62 自动迁移、ISP 0x6d、2880x1860、约 26.4 fps（0x62 模组约 14 fps）；
- 双 IMX490 Link A/B 同时抓图通过（各 10 帧）。

0x62 地址：

- 部分模组 MAX9295A 出厂地址是 0x62；
- `max9295`/`max96717`/`gmsl_ser_auto` 增加 0x62 探测和迁移；
- 迁移失败保留 0x62。

Pipe keep：

- 跨 link 启动时，原实现只写当前 link 的 pipe mask，会把另一路 pipe 关掉，远端 ISP 停流后无法恢复；
- 修复：启动任一路时，pipe mask 使用所有 locked link；per-link stop 不关闭 pipe；
- 抓图脚本在 enable 目标 csi2 channel 前先 disable 所有 csi2→ch link，避免 CFE 保留旧通道。

ISX031 3 Gbps：

- `gmsl-isx031.c/.h`：1920x1536 UYVY、CSI DT 0x1e；
- MAX96717F pass-through I2C，sensor 0x1a；
- MFP4 输出 24 MHz sensor INCK，MFP0 释放 reset；
- ISX031 内部同步，4-lane 30 fps，`0x8A00=0x17`，`0x8A01=0x80`；
- `max96724_isx031_board`：3 Gbps、`phy_clk=0x25`、`skip_frame_sync=true`；
- 根因：MAX96724 默认 6 Gbps，MAX96717F 最高 3 Gbps；3 Gbps 且 link enable 后 `0x001a` bit3 才置 1；CFE 576 Mbps 下 PHY clock 改 0x25 后正常出图。

### 13.5 当前状态（2026-11-22）

已实现：

- 分层框架、board/link/sensor 描述符、per-link 格式/streaming；
- IMX390/AR0820/IMX490/ISX031 四种模组；
- MAX9295A/MAX96717/MAX96717F 自动识别和 0x62 地址迁移；
- 单 link、双 IMX490、AR0820 单次 4K、ISX031 3 Gbps 上板验证；
- `gmsl-capture.sh`、`gmsl-reg`、`media-ctl`/`v4l2-ctl` 调试链路。

待完成：

- AR0820 4K stop/start 第二次启动；
- 4 路 mixed-link 硬件矩阵；
- 3 Gbps/6 Gbps 混插（per-link `three_gbps`/`phy_clk`/`skip_frame_sync`）；
- 不同分辨率同时出流；
- link loss/hot-plug 恢复、per-link 错误计数/debugfs；
- FE 路径 per-pad stop；
- V4L2 controls（flip/exposure/gain）；
- DT YAML binding / per-link endpoint。

### 13.6 2026-11-22 补充：Link A AR0820 实测日志

测试背景：

- 用户将第一路换上 SG8-AR0820C（实际链路为 Link A，`gmsl_links=1 0 0 0`）；
- 切换到 `dtoverlay=max96724-ar0820`；
- 冷启动后执行一次 4K 抓图，不重复 start/stop。

probe 日志：

```text
max96724_imx390 11-0036: max96724-imx390: deserializer detected
max96724_imx390 11-0036: max96724-imx390: link lock status=0x01
max96724_imx390 11-0036: max96724-imx390: max96724_monopolize_link: monopolize link_A
max96724_imx390 11-0036: max96724-imx390: serializer detected at 0x41
max96724_imx390 11-0036: max96724-imx390: probe success!
```

链路状态：

```text
$ cat /sys/bus/i2c/devices/*/gmsl_links
1 0 0 0
```

I2C 扫描：

```text
scan /dev/i2c-11 0x08..0x77
  0x27: ACK (reg0=0x4e)
  0x41: ACK (reg0=0x82)
  0x51: ACK (reg0=0x17)
  0x6d: ACK (reg0=0x00)
```

寄存器状态：

```text
0x41 0x000d = 0x91   # MAX9295A
0x41 0x0112 = 0xda   # PCLK detect
0x41 0x0013 = 0xde   # serializer link lock
des 0x001a = 0xdb    # link A lock
des 0x01dc = 0x81    # video lock
```

抓图命令：

```bash
./gmsl-capture.sh \
  -l 0 -s 3840x2160 -n 10 \
  -o /tmp/ar0820-link1.raw \
  -p /home/pi/ar0820-link1.png
```

抓图输出：

```text
link:  0
media: /dev/media1
video: /dev/video0
<<<<<<<<<<
captured: 10 frames -> /tmp/ar0820-link1.raw (165888000 bytes)
Y min=0 max=255 mean=78.69
U min=67 max=255 mean=130.22
V min=67 max=191 mean=126.60
png: /home/pi/ar0820-link1.png
```

dmesg：

```text
rp1-cfe 1f00128000.csi: Using a link rate of 1600 Mbps
rp1-cfe 1f00128000.csi: DPHY: Datarate 1600 Mbps out of range
max96724_imx390 11-0036: max96724-imx390: enable streams pad=0
max96724_imx390 11-0036: max96724-imx390: disable streams pad=0
```

结论：

- AR0820 在 Link A 锁定，MAX9295A 在 `0x41`，ISP 在 `0x6d`；
- 10 帧 3840x2160 抓图成功，raw 大小为 `3840*2160*2*10 = 165888000`；
- PNG 为 3840x2160，图像正常；本模组安装方向为 180°，需要 `-r` 或应用侧旋转；
- CFE 打印 `DPHY: Datarate 1600 Mbps out of range`，本次仍成功抓到 10 帧，但这是后续 4K 稳定性/链路余量需要继续确认的点；
- 本次只验证“冷启动单次 4K 抓图”，没有把“第二次 start/stop”纳入通过标准；该问题仍是已知限制。

> 注意：用户口语中的“link1”在第一路连接时对应 Link A。若物理相机接在 B/C/D，`gmsl_links` 应分别显示 `0 1 0 0` / `0 0 1 0` / `0 0 0 1`；不能只看接口丝印，必须看实际 lock mask。

### 13.7 本次总结复核

上一版总结对 ISX031 的主要结论是正确的，包括：

- ISX031 + MAX96717F、3Gbps、内部同步；
- 3Gbps 需要先配速率、打开 link enable，再 oneshot；
- pass-through I2C 的 sensor 在 `0x1a`；
- CFE 576Mbps 需要 PHY clock `0x25`；
- 单路 1920x1536 已成功抓图。

本版补充了：

- Link A AR0820 的真实 probe、I2C、寄存器和 4K 抓图日志；
- “link1”与 `gmsl_links` 的对应关系；
- CFE 1600Mbps warning 的实际观察；
- 最终文档的统一入口和已知限制。

仍未完成、不能声称已经支持的内容：

- 4 路不同相机同时出图；
- 3Gbps/6Gbps 混插；
- 不同 CSI-2 rate/port 的四路异构；
- 外部 trigger/多相机同步；
- AR0820 4K 重复 start/stop；
- 录制 MP4 命令的最终上板复测（本次只验证了 4K 抓图/PNG）。

---

## 14. 附录

### A. I2C 地址速查

| 设备 | 7-bit 地址 | 8-bit 写地址 | 说明 |
|---|---:|---:|---|
| MAX96724 | 0x27 | 0x4E | 驱动内部固定 |
| MAX9295A / MAX96717(F) | 0x41 + link | 0x82 + 2*link | Link A=0x41，B=0x42，C=0x43，D=0x44 |
| 加串器出厂 | 0x40 / 0x62 | 0x80 / 0xC4 | 驱动自动探测/迁移 |
| GW5200/GW5300 ISP | 0x6d | 0xDA | IMX390/AR0820/IMX490 |
| 模组 EEPROM | 0x51 | 0xA2 | 部分模组无响应 |
| IMX390 | 0x21 | 0x42 | 在 ISP 后面，通常 NACK |
| ISX031 | 0x1a | 0x34 | MAX96717F pass-through |
| 复合相机节点（DT） | 0x36 | 0x6C | overlay 占位地址，不是真实设备 |

### B. Overlay 模板

单相机模板见 7.2；mixed board 模板见 8.4。最小骨架：

```dts
/dts-v1/;
/plugin/;
/{
    compatible = "brcm,bcm2835";

    fragment@0  { target = <&i2c0if>;  __overlay__ { status = "okay"; }; };
    clk_frag: fragment@1 { target = <&cam1_clk>; __overlay__ { status = "okay"; clock-frequency = <24000000>; }; };
    fragment@2  { target = <&i2c0mux>; __overlay__ { status = "okay"; }; };

    i2c_frag: fragment@100 {
        target = <&i2c_csi_dsi>;
        __overlay__ {
            #address-cells = <1>;
            #size-cells = <0>;
            status = "okay";

            cam_node: gmsl@36 {
                compatible = "zuojisi,max96724-foo";
                reg = <0x36>;
                status = "ok";
                clocks = <&cam1_clk>;
                clock-names = "xclk";
                VANA-supply = <&cam1_reg>;
                VDIG-supply = <&cam_dummy_reg>;
                VDDL-supply = <&cam_dummy_reg>;
                port {
                    cam_endpoint: endpoint {
                        clock-lanes = <0>;
                        data-lanes = <1 2 3 4>;
                        clock-noncontinuous;
                        link-frequencies = /bits/ 64 <456000000>;
                    };
                };
            };
        };
    };

    csi_frag: fragment@101 {
        target = <&csi1>;
        csi: __overlay__ {
            status = "okay";
            brcm,media-controller;
            port {
                csi_ep: endpoint {
                    remote-endpoint = <&cam_endpoint>;
                    clock-lanes = <0>;
                    data-lanes = <1 2 3 4>;
                    clock-noncontinuous;
                };
            };
        };
    };

    __overrides__ {
        cam0 = <&i2c_frag>, "target:0=",<&i2c_vc>,
               <&csi_frag>, "target:0=",<&csi0>,
               <&clk_frag>, "target:0=",<&cam0_clk>,
               <&cam_node>, "clocks:0=",<&cam0_clk>,
               <&cam_node>, "VANA-supply:0=",<&cam0_reg>;
    };
};

&cam_node { status = "okay"; };
&cam_endpoint { remote-endpoint = <&csi_ep>; };
```

### C. 构建/部署脚本模板

```bash
#!/bin/bash
set -e

SRC=$(cd "$(dirname "$0")/rpi-6.18/linux-rpi-6.18.y" && pwd)
BUILD=$(cd "$(dirname "$0")/rpi-6.18/framework-build" && pwd)
RASPI_IP=${RASPI_IP:?set RASPI_IP}
RASPI_USER=${RASPI_USER:-root}

export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

cd "$SRC"

# 首次配置
[ -f "$BUILD/.config" ] || make O="$BUILD" bcm2712_defconfig

# 全量构建
make O="$BUILD" -j"$(nproc)" Image modules dtbs

# 部署内核 + dtb + overlay
scp "$BUILD/arch/arm64/boot/Image" \
  "$RASPI_USER@$RASPI_IP:/boot/firmware/kernel_2712.img"
scp "$BUILD"/arch/arm64/boot/dts/broadcom/*.dtb \
  "$RASPI_USER@$RASPI_IP:/boot/firmware/"
scp "$BUILD"/arch/arm64/boot/dts/overlays/max96724-*.dtbo \
  "$RASPI_USER@$RASPI_IP:/boot/firmware/overlays/"

# 模块
rm -rf /tmp/rpi-rootfs
make O="$BUILD" INSTALL_MOD_PATH=/tmp/rpi-rootfs modules_install
rsync -a /tmp/rpi-rootfs/lib/modules/ \
  "$RASPI_USER@$RASPI_IP:/lib/modules/"

ssh "$RASPI_USER@$RASPI_IP" 'depmod -a && reboot'
```

### D. 术语表

| 术语 | 解释 |
|---|---|
| CFE | Camera Front End，RP1 里的 CSI-2 接收/前端 |
| PiSP | Raspberry Pi Image Signal Processor，含 FE/BE |
| V4L2 | Video for Linux 2，用户态视频 API |
| media controller | 描述 subdev/视频节点连接关系的内核框架 |
| subdev | V4L2 子设备，sensor/CSI/ISP 等 |
| GMSL | Gigabit Multimedia Serial Link，Maxim/ADI 的 SerDes 技术 |
| SerDes | Serializer/Deserializer，加串/解串 |
| PoC | Power over Coax，同轴供电 |
| VC | MIPI CSI-2 Virtual Channel |
| DT | MIPI CSI-2 Data Type |
| pipe | MAX96724 内部视频管道 |
| link | MAX96724 的一个 GMSL 输入 |
| D-PHY | MIPI CSI-2 物理层 |
| UYVY | 16-bit YUV422 打包格式，本项目默认 |
| golden test | 单相机最小可复现验收测试 |

### E. 参考资料

本地 datasheets（工作区根目录 `datasheets/`，即 `.../kernel/datasheets/`）：

- `MAX96724_Datasheet.pdf`
- `MAX96724F_Datasheet.pdf`
- `MAX96724FR_User_Guide.pdf`
- `MAX96724_series_Errata.pdf`
- `MAX96717_Datasheet.pdf` / `MAX96717F_Datasheet.pdf` / `MAX96717R_Datasheet.pdf`
- `MAX96717FR_User_Guide.pdf`
- `MAX9295D_Datasheet.pdf` / `MAX9295D_User_Guide.pdf`
- `MAX96716A_Datasheet.pdf` / `MAX96716A_F_User_Guide.pdf`
- `GMSL2_General_User_Guide.pdf`
- `GMSL2_Channel_Specification_User_Guide.pdf`
- `GMSL1_Channel_Specification_User_Guide.pdf`
- `Public_GMSL2_Hardware_Design_and_Validation_Guide.pdf`
- `GMSL2_EMC_Test_Report.pdf`

ADI 官方页面：

- MAX96724：<https://www.analog.com/en/products/max96724.html>
- MAX96717：<https://www.analog.com/en/products/max96717.html>
- MAX9295D：<https://www.analog.com/en/products/max9295d.html>
- MAX96716A：<https://www.analog.com/en/products/max96716a.html>
- GMSL 产品分类：<https://www.analog.com/cn/product-category/serdes-gmsl.html>

项目内文档：

- `GMSL_MAX96724_IMX390.md`：快速使用/历史记录
- `Documentation/gmsl/ARCHITECTURE.md`：目标框架、SIPL/AIS 对标、验证策略
- `Documentation/gmsl/CHANGELOG.md`：完整修改记录
- `tools/gmsl/README.md`：寄存器工具详细说明
- `Documentation/gmsl/config.txt.example`：config.txt 示例

### F. 文件地图

```text
rpi-6.18/linux-rpi-6.18.y/
├── drivers/media/i2c/
│   ├── gmsl-core.c/.h
│   ├── gmsl-v4l2.c
│   ├── gmsl-max96724.c/.h
│   ├── gmsl-max9295.c/.h
│   ├── gmsl-max96717.c/.h
│   ├── gmsl-ser-auto.c/.h
│   ├── gmsl-imx390.c/.h
│   ├── gmsl-ar0820.c/.h
│   ├── gmsl-imx490.c/.h
│   ├── gmsl-isx031.c/.h
│   └── gmsl-desc.c/.h
├── drivers/media/platform/raspberrypi/rp1_cfe/
│   ├── cfe.c
│   ├── csi2.c
│   ├── dphy.c
│   └── pisp_fe.c
├── arch/arm/boot/dts/overlays/
│   ├── max96724-imx390-overlay.dts
│   ├── max96724-ar0820-overlay.dts
│   ├── max96724-imx490-overlay.dts
│   └── max96724-isx031-overlay.dts
├── tools/gmsl/
│   ├── gmsl-capture.sh
│   ├── gmsl-reg.c
│   ├── Makefile
│   └── README.md
└── Documentation/gmsl/
    ├── ARCHITECTURE.md
    ├── CHANGELOG.md
    ├── config.txt.example
    └── GMSL_RPI5_COMPLETE_GUIDE_CN.md   ← 本文档
```

---

**维护建议**：每次新增模组/修改框架，至少更新本文档的 4.6 支持矩阵、8.9 测试矩阵、13 适配历史；寄存器级细节放 `CHANGELOG.md`，不要把本文档写成寄存器手册。
