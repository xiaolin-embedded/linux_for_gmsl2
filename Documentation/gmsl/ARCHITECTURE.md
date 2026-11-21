# GMSL Camera Framework Design

> 状态：Phase 1（core/ops/desc 分层）、Phase 2（board/link/sensor 描述符拆分、
> per-link 格式、稀疏 link 映射）、P0 per-link streaming 控制和 AR0820/
> MAX96717F 支持已实现。IMX390 Link A 单相机 golden test 通过；AR0820 4K
> 单次冷启动抓图通过，但 CFE/DMA 的 4K 重复 start/stop 仍待修；4 路
> mixed-link 硬件测试仍待补。本文描述的是**目标框架**；与 SIPL/AIS 对齐的
> 部分仍按 Roadmap 分阶段补齐。

## 1. 目标与非目标

### 目标

- 一块 MAX96724 板支持最多 4 路 GMSL link，可混插不同相机模组。
- 新增相机模组只增加：
  - 一个 `sensor_desc`（分辨率、格式、DT、link freq、init/start/stop）
  - 一个 `sensor_ops`（如果该 sensor 需要特殊控制）
  - 必要时一个 `serializer_ops`（MAX9295A / MAX96717 / MAX96717F）
  - 一个 DT endpoint / overlay
  - 不复制 core、不复制 V4L2 层、不复制 deser 初始序列。
- 对上游保持标准 V4L2/media 接口，不引入自定义 ioctl/ABI。
- 行为可验证：每层有明确 contract，可 mock、可单测、可上板矩阵测试。

### 非目标

- 不在内核里实现一个用户态 camera server（SIPL/AIS 的 server 部分放在用户态）。
- 不追求一次性支持所有 GMSL 芯片；先覆盖 MAX96724 + MAX9295A/MAX96717。
- 当前不引入 V4L2 streams API 的完整 routing；使用非 `FL_STREAMS` 的
  pad `enable_streams/disable_streams`，并保留 legacy `s_stream` 兼容路径。

## 2. 术语与拓扑

```text
Camera Module                         Board
┌─────────────────────┐              ┌──────────────────────────┐
│ Sensor (IMX390/AR0820) │──MIPI──▶│ Serializer (MAX9295A/96717) │
└─────────────────────┘              └────────────┬─────────────┘
                                                  │ GMSL2 coax
                                     ┌────────────▼─────────────┐
                                     │ MAX96724 (4 links)        │
                                     │  link A/B/C/D -> VC0..3   │
                                     └────────────┬─────────────┘
                                                  │ CSI-2 4 lane
                                     ┌────────────▼─────────────┐
                                     │ RP1 CFE (csi2_ch0..3)     │
                                     └──────────────────────────┘
```

- **Board**：一块 MAX96724 + 电源/时钟/CSI 连接。
- **Link**：MAX96724 的一个 GMSL 输入，编号 0..3 = A..D = pad = VC。
- **Camera module**：sensor + serializer 的组合，当前由模组厂整体提供。
- **Pipeline**：sensor → serializer → deserializer link → CSI2 VC → CFE channel。

## 3. 设计原则

1. **分层不跨层摸寄存器**
   - V4L2 层不知道寄存器地址；
   - sensor 层不写 deserializer 寄存器；
   - serializer 层不碰 V4L2；
   - core 里没有 `if (sensor == imx390)`。
2. **policy 放描述符/DT，mechanism 放 core**
   - 分辨率、格式、DT、link freq、init 表进 `sensor_desc`；
   - 板级 link 数量、serializer 类型、地址映射进 `board_desc`/DT；
   - core 只做状态机、I2C 访问、ops 调度。
3. **标准 V4L2/media 优先**
   - 4-pad subdev、media controller、async、fwnode、`get_frame_desc`、
     `get_mbus_config`、runtime PM；
   - 不加自定义 ioctl；诊断走 sysfs/debugfs。
4. **每层可独立验证**
   - ops 有 contract；desc 有表；core 可 mock；
   - 硬件矩阵按 board × link × module × serializer 组合。

## 4. 目标架构

```text
用户态：gmsl-camera CLI / libgmsl（可选）
        │  V4L2 + media-ctl
        ▼
V4L2/media controller + RP1 CFE
        │
        ▼
gmsl-v4l2        4-pad subdev，fmt/frame_desc/mbus_config/s_stream
        │
        ▼
gmsl-core        board/link 状态、I2C 地址切换/重试、ops 调度、PM、
                 link loss 恢复、错误计数
        │
        ├── max96724 deser ops
        ├── max9295 / max96717 ser ops
        └── imx390 / ar0820 sensor ops
```

### 4.1 对象模型（Phase 2 已实现）

```c
struct gmsl_board_desc {
    const char *compatible;
    const struct gmsl_des_ops *des_ops;
    unsigned int num_links;            /* 已配置 link 数，不是 4 */
    const struct gmsl_link_desc *links;
};

struct gmsl_link_desc {
    unsigned int index;                /* 0..3 = A..D = pad = VC，可稀疏 */
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

Phase 2 已把 `board_desc / link_desc / sensor_desc / ser_ops` 拆开：

- 换 serializer 不需要复制 sensor desc；
- `link_desc.index` 允许只填 Link A/C 等稀疏组合，core 按 index 写入
  `gmsl->links[]`；
- 同一块板不同 link 可以挂不同 sensor；
- V4L2 层按 pad 读写对应 link 的格式，`set_fmt` 不再覆盖所有 link；
- 未使用的 link 继承首个已配置 link 的默认格式，保证 4 个 source pad 对 CFE
  始终报告合法格式；
- 新板只写一个 `board_desc` + DT。

板级 power/POC 顺序仍在 V4L2/PM 层，计划在 Phase 3 移到 board ops。

### 4.2 Ops contract

```c
struct gmsl_des_ops {
    const char *name;
    int (*init)(struct gmsl_dev *gmsl);                  /* 板级初始化 + link lock */
    int (*configure_link)(struct gmsl_dev *g, struct gmsl_link *l);
    int (*monopolize_link)(struct gmsl_dev *g, int link);
    int (*restore_link)(struct gmsl_dev *g);
    int (*enable_pipes)(struct gmsl_dev *g, u8 mask, bool on);
};

struct gmsl_ser_ops {
    const char *name;
    u8 id;                                               /* 0x91 / 0xbf / 0xc8 */
    int (*probe)(struct gmsl_link *link);
    int (*configure)(struct gmsl_link *link);
    int (*gpio_frame_sync)(struct gmsl_link *link);
    int (*enable_stream)(struct gmsl_link *link, bool on);
};

struct gmsl_sensor_ops {
    const char *name;
    int (*probe)(struct gmsl_link *link);
    int (*init)(struct gmsl_link *link);
    int (*start_stream)(struct gmsl_link *link, bool on);
    int (*get_fmt)(struct gmsl_link *link, struct v4l2_mbus_framefmt *fmt);
    int (*get_mbus_config)(struct gmsl_link *link,
                           struct v4l2_mbus_config *cfg);
};
```

约束：

- 所有 ops 只能通过 core 的 `gmsl_read_addr()` / `gmsl_write_addr()` 访问 I2C；
- ops 不允许直接调用 `i2c_transfer()`；
- ops 不持有 V4L2 状态；V4L2 状态只在 `gmsl-v4l2` 层；
- ops 返回标准 errno，core 负责回滚/重试/日志。

## 5. DT / board description

目标 DT 形状：

```dts
gmsl: gmsl@27 {
    compatible = "vendor,max96724-gmsl";
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
                link-frequencies = /bits/ 64 <456000000>;
            };
        };
        /* port@2, port@3 ... */
    };
};
```

- `port@N` 的 `reg` = link 号 = pad 号 = VC 号。
- `remote-endpoint` 指向 sensor/模组节点；serializer 类型可以由模组 compatible
  或 `serializer` 属性决定。
- 不再需要 `zbranch`、`rotation`、`orientation` 这类 board-specific 占位。
- 目标是要有 YAML binding，例如
  `Documentation/devicetree/bindings/media/i2c/vendor,max96724-gmsl.yaml`。

## 6. 状态机

### 6.1 Probe

```text
board power on
  -> deser reset / ID / version
  -> link lock status
  -> for each locked link:
       deser configure_link (pipe/VC)
       deser monopolize_link
       ser probe / set address / ID check
       ser configure
       sensor probe / init
  -> deser restore_link
  -> register 4-pad V4L2 subdev
```

失败处理：单 link 失败只标记该 link down，不阻止其它 link probe。serializer
probe/configure、sensor init 失败都会清除对应 lock bit；只有 deser 全局初始
化失败才整体 probe 失败。

### 6.2 Stream

```text
enable_streams(pad, on):
  sensor start -> serializer configure -> deser pipe enable -> CSI output
disable_streams(pad, off):
  CSI stop -> deser pipe disable -> serializer stop -> sensor stop
```

已实现**按 pad/link 独立开关**：

- core `gmsl_core_s_stream(gmsl, link_mask, on)` 只启停 mask 内的 link，
  维护 `streaming_mask`，并只对本次新启的 link 做 frame sync；
- V4L2 提供 pad `enable_streams/disable_streams`（非 `FL_STREAMS`，每个 pad
  视为 stream 0），同时保留 legacy `s_stream` 兼容路径；
- CFE 的 csi2 直通路径按 `node->id` 调用 pad streams；首个通道打开 CSI RX，
  最后一个通道关闭，独立停止某一路不会关闭其他通道；
- FE 路径暂时仍走 legacy `s_stream`，后续再按 pad 改造。

后续如果上游/CFE 需要完整 routing，再升级到 `V4L2_SUBDEV_FL_STREAMS` +
routing，不需要改 core 的 link mask 语义。

### 6.3 Power / PM

- board 级 power（deser、POC、xclk）放 core/board ops；
- sensor/serializer 的上电顺序放 link/ser/sensor ops；
- runtime PM 在 `gmsl-v4l2`/core，禁止在寄存器读写里做电源管理；
- suspend/resume 要保存/恢复 link lock、VC 映射和 streaming 状态。

### 6.4 错误与恢复

- link loss：延时工作/IRQ 检测 `0x001a/0x000a..c` 的 lock 位变化；
- link down：停止该 link 的 stream，标记 `gmsl_links` 和错误计数；
- 恢复：重新 reset link、重新探测 serializer/sensor、恢复 pipe/VC；
- I2C NACK：core 已有 3 次重试；需要增加错误计数和 last error；
- 所有错误通过 standard V4L2 event / sysfs / dmesg 暴露，不引入自定义 ioctl。

## 7. V4L2 / media / CFE 集成

- 顶层仍然是一个 4-pad source subdev，`pad=N` 对应 link N / VC N。
- `get_frame_desc(pad)` 返回该 link 的 DT/VC。
- `get_mbus_config()` 返回 CSI-2 输出 link rate（912 Mbps），不是每路 GMSL
  link rate。
- CFE 的 `cfe_link_node_pads()` 会把 4 个 pad 依次接到 `csi2_ch0..3`；
  media pipeline 会校验所有 enabled link 的格式。
- 因此：
  - 未使用的 link/pad 也必须有合法格式，或者后续改为 routing/streams 只
    使能活动 link；
  - 当前 capture 脚本会把 4 个 csi2 sink pad 都设成同一格式，这是 CFE 的
    pipeline 校验要求，不是驱动可以绕过的。
- `gmsl_links` sysfs 保留；后续增加 `gmsl_link_err` / debugfs 寄存器 dump。

## 8. 新增相机模组流程

以 AR0820 + MAX96717 为例：

1. 确认 serializer ID：`0x91` MAX9295A / `0xbf` MAX96717 / `0xc8` 96717F。
2. 新增 `gmsl-ar0820.c`：
   - `sensor_ops`：probe/init/start/stop/get_fmt/get_mbus_config；
   - `sensor_desc`：3840x2160、UYVY8、DT 0x1e、link freq、init 表。
3. 新增/复用 `gmsl-max96717.c`：
   - `ser_ops`：ID 0xbf、地址、0x0318/0x0311/0x0331/0x031a、GPIO sync。
4. 新增 board/link desc：
   - `board_desc` 引用 `max96724_des_ops`；
   - `link_desc` 指向 AR0820 sensor desc + MAX96717 ser ops。
5. 新增 DT endpoint / overlay，`port@N` 指向 AR0820 模组。
6. 抓图工具增加分辨率/格式 profile（当前脚本写死 1920x1080）。
7. 测试：单 link A/B/C/D、混插 IMX390 + AR0820、热插拔、错误注入。

新增内容只有 desc/ops/DT/测试，不改 core、V4L2、deser 初始序列。

## 9. 对标 NVIDIA SIPL / Qualcomm AIS

| SIPL / AIS 概念 | 本框架对应 | 说明 |
|---|---|---|
| SIPL CameraModule | `gmsl_link_desc` + sensor/ser ops | 一个 link 上的相机模组 |
| SIPL DeviceBlock | `gmsl_des_ops` / `board_desc` | 解串器/板级资源 |
| SIPL CameraDevice | 4-pad V4L2 subdev + CFE pipeline | 对应用可见的 camera device |
| SIPL CameraProvider/Control | media controller + `gmsl-camera` CLI | 拓扑查询、开流、事件 |
| SIPL power/stream state machine | core state machine + runtime PM | probe/stream/suspend/resume |
| SIPL error/event | V4L2 event + sysfs/debugfs | 不新增内核 ABI |
| AIS server | 用户态 `gmsl-camera` 服务/库 | 不放进内核 |

借鉴点：

- 用 descriptor 描述“板/模组/link”，而不是把板级差异写进驱动分支；
- 用显式状态机管理 probe/stream/power/error；
- 用统一 provider/CLI 让应用只关心 camera id/link，不关心 chip 寄存器；
- 错误/状态可查询，便于产线/实验室自动化。

差异：

- SIPL/AIS 是用户态 camera 框架，包含 ISP/资源管理；本设计只做内核 GMSL
  传输层，ISP 仍交给 RPi CFE/PiSP，用户态只做采集/控制。
- 不复制 SIPL/AIS 的私有 ABI；内核侧坚持标准 V4L2/media。

## 10. 验证策略

### 静态/编译

- `checkpatch.pl`、`W=1`、`C=1` sparse；
- `dt_binding_check`、`dtbs_check`；
- 编译矩阵：board × deser × ser × sensor × overlay；
- `fdtoverlay` 对每个 overlay 套干净 base DTB。

### 单元/契约

- mock regmap + mock ops 测 core 状态机；
- 每个 desc 检查 format/link freq/frame desc；
- V4L2 层用 mock ops 测 4 pad/format/stream 顺序；
- ops 返回错误时验证回滚。

### 硬件矩阵

- 每个 link A/B/C/D 单独测；
- IMX390 + AR0820 混插不同 link；
- 同一板 4 路并发、双板双 CSI 口；
- `v4l2-compliance`、`media-ctl -p`、抓帧统计、912 Mbps；
- PM suspend/resume、反复开关流、热插拔；
- 长跑数小时看丢帧/I2C 错误。

### 故障注入

- I2C NACK、拔 FAKRA、serializer 复位、sensor 断电；
- KASAN、lockdep、DEBUG_ATOMIC_SLEEP；
- 恢复后重新出图；错误计数递增。

### CI

- GitHub Actions：编译矩阵 + checkpatch + DT 检查；
- 自托管 Pi runner：golden IMX390 抓图 + dmesg 扫描 + 帧统计；
- 每次新增模组，把 dmesg/media topology/帧统计存到
  `Documentation/gmsl/`。

## 11. 当前状态与缺口

### 已实现（Phase 1 + Phase 2 + P0 streaming）

- core / v4l2 / max96724 / max9295 / imx390 / desc 分层；
- `board_desc + link_desc + sensor_desc + ser_ops` 拆分，link 按 index 稀疏映射；
- per-link `set_fmt`，未使用 pad 继承合法默认格式；
- core per-link `streaming_mask` 和 `gmsl_core_s_stream(mask)`；
- serializer/sensor 单 link 失败隔离，不阻止其他 link probe；
- V4L2 pad `enable_streams/disable_streams`，legacy `s_stream` 兼容；
- CFE csi2 直通路径按通道启停 sensor，CSI RX 首开/末关；
- AR0820 3840x2160 UYVY desc/ops、MAX96717/96717F ser ops、
  MAX9295A/96717F 自动识别和 `max96724-ar0820` overlay；
- 4-pad V4L2 subdev、frame_desc、mbus_config、runtime PM；
- I2C 地址切换、3 次重试、quiet probe；
- `alloc_parse` link freq + `get_mbus_config`，CFE 912 Mbps；
- 模块名/compatible/overlay/config 不变；
- `W=1` 编译通过；Phase 1/2 上板 golden test 均通过；
- P0 单 link 测试：Link A、912 Mbps、120 帧 + 二次重启 60 帧、
  约 28–29 fps、无 I2C 错误、画面正立清晰；
- AR0820 4K 单次冷启动抓图通过：MAX9295A（0x91）、1600 Mbps、3840x2160
  UYVY；4K 重复 start/stop 仍卡在 CFE/DMA 重启，待修；
- 4 路 mixed-link 硬件测试待补。

### 待完善（按优先级）

| 优先级 | 缺口 | 影响 | 建议 |
|---|---|---|---|
| P0 | AR0820 4K 停止后第二次启动只收约 4 个 buffer | 4K 不能连续 start/stop | 修 CFE/DMA 4K 重启路径 |
| P0 | 4 路 mixed-link 未在硬件上验证 | 独立启停/错误隔离缺少实测 | 准备 4 个模组做矩阵测试 |
| P1 | FE 路径仍用 legacy `s_stream` | FE 模式不能按 pad 独立停流 | 后续 FE 也改 pad streams |
| P1 | 无 link loss/hot-plug 恢复 | 拔线/复位后不能自动恢复 | 加 delayed work/错误计数 |
| P1 | 无 per-link 错误计数/debugfs | 现场定位困难 | 加 sysfs/debugfs |
| P1 | 模组拓扑未建模（IMX390 在 GW5200 ISP 后） | sensor 寄存器不能直读 | 增加 module/ISP desc 和 ISP pass-through |
| P1 | power/POC 顺序仍在 V4L2 层 | 多板/多模组不好复用 | 移到 board ops |
| P1 | 抓图脚本写死 1920x1080 UYVY | AR0820 4K 不能直接用 | 加 width/height/format 参数 |
| P1 | DT 无 YAML binding、仍单 sensor 节点 | 与 SIPL/AIS 目标不一致 | 加 binding + per-link endpoint |
| P2 | 无 V4L2 controls（flip/exposure/gain） | 翻转只能用户态做 | sensor ops 按需加 controls |
| P2 | 未用 streams API/routing | 上游化/多流受限 | 评估 `V4L2_SUBDEV_FL_STREAMS` |
| P2 | 无 CI/长跑/故障注入 | 回归风险 | 建 GitHub Actions + Pi runner |
| P2 | 模块名仍 `max96724_imx390` | 通用性不足 | 后续改 `gmsl-max96724` + board desc |

## 12. Roadmap

- **Phase 1（已完成）**：结构分层，行为不变，硬件 golden test 通过。
- **Phase 2（基本完成）**：`board_desc/link_desc/sensor_desc` 拆分、稀疏 link
  映射、per-link `set_fmt/streaming`、`max96717` ser ops、`ar0820` desc/ops
  已完成并通过编译；剩余 AR0820 4K 上板抓图。
- **Phase 3**：修 CFE/DMA 的 4K 重复 start/stop；4 路 mixed-link 验证；
  link loss/hot-plug 恢复；per-link 错误计数/debugfs；FE pad streams；
  抓图 CLI 已支持 `-s WxH`。
- **Phase 4（可选）**：DT YAML binding；V4L2 streams/routing；V4L2 controls；
  与 mainline `maxim-serdes` 对齐；CI/长跑/故障注入。

## 13. 文件布局与编码约定

目标布局（Phase 2+）：

```text
drivers/media/i2c/gmsl/
  gmsl-core.c/.h
  gmsl-v4l2.c
  gmsl-board.c/.h
  gmsl-link.c/.h
  gmsl-max96724.c/.h
  gmsl-max9295.c/.h
  gmsl-max96717.c/.h
  gmsl-imx390.c/.h
  gmsl-ar0820.c/.h
  gmsl-desc.c/.h
  gmsl-debugfs.c
tools/gmsl/
  gmsl-camera.c
  gmsl-capture.sh
  gmsl-reg.c
Documentation/gmsl/
  ARCHITECTURE.md
  CHANGELOG.md
  ...
```

- 每个文件 SPDX GPL-2.0；
- 每个 ops 必须通过 core 的 I2C helper；
- core 不出现 chip 名字分支；
- desc 表不包含代码逻辑；
- 新增模组的 commit 只包含 desc/ops/DT/测试；
- 所有寄存器魔法值加注释，能对应 datasheet。

## 14. 参考

- mainline `maxim-serdes` 系列：`max_ser.c` / `max_des.c` / `max96717.c` /
  `max96724.c` / `max9296a.c`，V4L2 streams/routing、I2C ATR、internal pads。
- NVIDIA SIPL：CameraModule / DeviceBlock / CameraDevice / Provider 分层。
- Qualcomm AIS：camera server、topology、resource/error 管理。
- V4L2/media controller、V4L2 fwnode、runtime PM 文档。
- 本仓库 `vendor-full-port` 分支：厂商 14 驱动/overlay 参考。