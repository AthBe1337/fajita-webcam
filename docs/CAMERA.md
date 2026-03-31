# OnePlus 6T (fajita) Camera Reference

Platform: Qualcomm SDM845, Kernel 6.18+, Driver: `qcom_camss`

## Hardware

| Role | Sensor | Resolution | CCI Bus | I2C Addr | CSIPHY | Bus Type | V4L2 Subdev | Notes |
|------|--------|------------|---------|----------|--------|----------|-------------|-------|
| Front | Sony IMX371 | 4656x3496 (16MP) | i2c-16 (bus@0) | 0x10 | csiphy2 | D-PHY 4-lane | `/dev/v4l-subdev19` | rotation=90° |
| Rear (main) | Sony IMX519 | 4656x3496 (16MP) | i2c-16 (bus@0) | 0x1a | csiphy0 | C-PHY 3-lane | `/dev/v4l-subdev21` | rotation=270° |
| Rear (sub) | Sony IMX376K | 2592x1940 (5MP mode, crop 5184x3880=20MP) | i2c-17 (bus@1) | 0x10 | csiphy1 | D-PHY 4-lane | `/dev/v4l-subdev20` | rotation=270° |
| AF Motor (IMX519) | LC898217XC | — | i2c-16 | 0x72 | — | — | `/dev/v4l-subdev22` | 后置主摄对焦马达 |
| AF Motor (IMX376K) | LC898217XC | — | i2c-17 | 0x74 | — | — | `/dev/v4l-subdev23` | 后置副摄对焦马达 |

注：`/dev/v4l-subdev*` 编号可能因内核枚举顺序变化，建议先运行 `media-ctl -d /dev/media0 -p` 按 entity name 确认实际节点。

## Media Topology

```
Sensor ──IMMUTABLE──> CSIPHY ──user link──> CSID ──user link──> VFE RDI ──IMMUTABLE──> /dev/videoN
```

需要手动启用的链路（中间两段）：
- `csiphy → csid` (可选 csid0/csid1/csid2)
- `csid pad N → vfe rdi/pix` (可选 vfe0/vfe1/vfe2)

固定链路（无需配置）：
- sensor → csiphy (IMMUTABLE)
- vfe rdi → /dev/videoN (IMMUTABLE)

### Video 设备映射

| Device | Name | VFE | Output |
|--------|------|-----|--------|
| /dev/video0 | msm_vfe0_video0 | VFE0 RDI0 | Raw |
| /dev/video1 | msm_vfe0_video1 | VFE0 RDI1 | Raw |
| /dev/video2 | msm_vfe0_video2 | VFE0 RDI2 | Raw |
| /dev/video3 | msm_vfe0_video3 | VFE0 PIX | ISP processed |
| /dev/video4-7 | msm_vfe1_* | VFE1 | 同上 |
| /dev/video8-11 | msm_vfe2_* | VFE2 | 同上 |
| /dev/video12 | qcom-venus-decoder | — | 视频解码器 |
| /dev/video13 | qcom-venus-encoder | — | 视频编码器 |

## 使用方法

### 依赖

```bash
# Arch Linux
pacman -S v4l-utils ffmpeg python

# Ubuntu / Debian
apt install v4l-utils ffmpeg python3
```

### 后置主摄 (IMX519)

#### 1. 配置链路

```bash
# 启用 csiphy0 → csid0 → vfe0_rdi0
media-ctl -d /dev/media0 -l '"msm_csiphy0":1->"msm_csid0":0[1]'
media-ctl -d /dev/media0 -l '"msm_csid0":1->"msm_vfe0_rdi0":0[1]'
```

#### 2. 设置格式（全管线）

```bash
media-ctl -d /dev/media0 -V '"imx519 16-001a":0[fmt:SRGGB10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csiphy0":0[fmt:SRGGB10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csiphy0":1[fmt:SRGGB10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csid0":0[fmt:SRGGB10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csid0":1[fmt:SRGGB10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":0[fmt:SRGGB10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":1[fmt:SRGGB10_1X10/4656x3496]'
```

#### 3. 设置 V4L2 输出格式

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4656,height=3496,pixelformat=pRAA
```

#### 4. 设置曝光参数

```bash
v4l2-ctl -d /dev/v4l-subdev21 --set-ctrl exposure=6000
v4l2-ctl -d /dev/v4l-subdev21 --set-ctrl analogue_gain=800
v4l2-ctl -d /dev/v4l-subdev21 --set-ctrl digital_gain=4096
```

#### 5. 抓帧

```bash
# 必须抓多帧（第一帧可能为空）
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=output.raw
```

#### 6. 断开链路（切换摄像头前）

```bash
media-ctl -d /dev/media0 -l '"msm_csiphy0":1->"msm_csid0":0[0]'
media-ctl -d /dev/media0 -l '"msm_csid0":1->"msm_vfe0_rdi0":0[0]'
```

### 后置副摄 (IMX376K)

#### 1. 配置链路

```bash
# 启用 csiphy1 → csid0 → vfe0_rdi0
media-ctl -d /dev/media0 -l '"msm_csiphy1":1->"msm_csid0":0[1]'
media-ctl -d /dev/media0 -l '"msm_csid0":1->"msm_vfe0_rdi0":0[1]'
```

#### 2. 设置格式（全管线）

```bash
media-ctl -d /dev/media0 -V '"imx376 17-0010":0[fmt:SBGGR10_1X10/2592x1940]'
media-ctl -d /dev/media0 -V '"msm_csiphy1":0[fmt:SBGGR10_1X10/2592x1940]'
media-ctl -d /dev/media0 -V '"msm_csiphy1":1[fmt:SBGGR10_1X10/2592x1940]'
media-ctl -d /dev/media0 -V '"msm_csid0":0[fmt:SBGGR10_1X10/2592x1940]'
media-ctl -d /dev/media0 -V '"msm_csid0":1[fmt:SBGGR10_1X10/2592x1940]'
media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":0[fmt:SBGGR10_1X10/2592x1940]'
media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":1[fmt:SBGGR10_1X10/2592x1940]'
```

#### 3. 设置 V4L2 输出格式

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=2592,height=1940,pixelformat=pBAA
```

#### 4. 设置曝光参数

```bash
v4l2-ctl -d /dev/v4l-subdev20 --set-ctrl exposure=1600
v4l2-ctl -d /dev/v4l-subdev20 --set-ctrl analogue_gain=240
v4l2-ctl -d /dev/v4l-subdev20 --set-ctrl digital_gain=1024
```

#### 5. 抓帧

```bash
# 必须抓多帧（第一帧可能为空）
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=output.raw
```

#### 6. 断开链路（切换摄像头前）

```bash
media-ctl -d /dev/media0 -l '"msm_csiphy1":1->"msm_csid0":0[0]'
media-ctl -d /dev/media0 -l '"msm_csid0":1->"msm_vfe0_rdi0":0[0]'
```

### 前置摄像头 (IMX371)

#### 1. 配置链路

```bash
media-ctl -d /dev/media0 -l '"msm_csiphy2":1->"msm_csid0":0[1]'
media-ctl -d /dev/media0 -l '"msm_csid0":1->"msm_vfe0_rdi0":0[1]'
```

#### 2. 设置格式

```bash
media-ctl -d /dev/media0 -V '"imx371 16-0010":0[fmt:SBGGR10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csiphy2":0[fmt:SBGGR10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csiphy2":1[fmt:SBGGR10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csid0":0[fmt:SBGGR10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_csid0":1[fmt:SBGGR10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":0[fmt:SBGGR10_1X10/4656x3496]'
media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":1[fmt:SBGGR10_1X10/4656x3496]'
```

#### 3. 设置 V4L2 输出格式

```bash
v4l2-ctl -d /dev/video0 --set-fmt-video=width=4656,height=3496,pixelformat=pBAA
```

#### 4. 设置曝光参数

```bash
v4l2-ctl -d /dev/v4l-subdev19 --set-ctrl exposure=8000
v4l2-ctl -d /dev/v4l-subdev19 --set-ctrl analogue_gain=200
v4l2-ctl -d /dev/v4l-subdev19 --set-ctrl digital_gain=2048
```

#### 5. 抓帧

```bash
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=output.raw
```

## Sensor 控制参数

### IMX519 (后置主摄, /dev/v4l-subdev21)

| 参数 | 范围 | 默认值 | 说明 |
|------|------|--------|------|
| exposure | 20 - 6737 | 1000 | 曝光行数 |
| analogue_gain | 0 - 960 | 0 | 模拟增益 |
| digital_gain | 256 - 65535 | 256 | 数字增益 |
| horizontal_flip | 0/1 | 0 | 水平翻转 |
| vertical_flip | 0/1 | 0 | 垂直翻转 |
| test_pattern | 0-4 | 0 | 0=关, 1=纯色, 2=八色条, 3=渐变色条, 4=PN9 |
| vertical_blanking | 3273 - 8380504 | 3273 | 帧间隔（影响帧率） |
| red/green_red/blue/green_blue_pixel_value | 0 - 4095 | 4095 | 测试图案像素值 |

### IMX376K (后置副摄, /dev/v4l-subdev20)

| 参数 | 范围 | 默认值 | 说明 |
|------|------|--------|------|
| exposure | 4 - 65515 | 1600 | 曝光行数 |
| analogue_gain | 0 - 480 | 0 | 模拟增益 |
| digital_gain | 0 - 4096 | 1024 | 数字增益 |
| horizontal_flip | 0/1 | 1 | 水平翻转 |
| vertical_flip | 0/1 | 1 | 垂直翻转 |
| wide_dynamic_range | 0/1 | 0 | HDR 模式 |
| test_pattern | 0-4 | 0 | 0=关, 1=纯色, 2=八色条, 3=渐变色条, 4=PN9 |
| vertical_blanking | 2796 - 63585 | 2796 | 帧间隔（影响帧率） |

### IMX371 (前置, /dev/v4l-subdev19)

| 参数 | 范围 | 默认值 | 说明 |
|------|------|--------|------|
| exposure | 4 - 65515 | 1600 | 曝光行数 |
| analogue_gain | 0 - 480 | 0 | 模拟增益 |
| digital_gain | 0 - 4096 | 1024 | 数字增益 |
| horizontal_flip | 0/1 | 1 | 水平翻转 |
| vertical_flip | 0/1 | 1 | 垂直翻转 |
| wide_dynamic_range | 0/1 | 0 | HDR 模式 |
| test_pattern | 0-4 | 0 | 同上 |
| vertical_blanking | 56 - 62029 | 56 | 帧间隔 |

## 输出数据格式

### MIPI 10-bit Packed Bayer

RDI 直通输出的原始 Bayer 数据，每 4 像素占 5 字节：

```
Byte 0: Pixel0[9:2]
Byte 1: Pixel1[9:2]
Byte 2: Pixel2[9:2]
Byte 3: Pixel3[9:2]
Byte 4: Pixel3[1:0] | Pixel2[1:0] | Pixel1[1:0] | Pixel0[1:0]
```

| Sensor | FourCC | Bayer 排列 | 分辨率 | Stride (bytes/line) | 帧大小 (bytes) |
|--------|--------|-----------|--------|---------------------|----------------|
| IMX519 | pRAA | RGGB | 4656x3496 | 5824 | 20,360,704 |
| IMX376K | pBAA | BGGR | 2592x1940 | 3248 | 6,301,120 |
| IMX371 | pBAA | BGGR | 4656x3496 | 5824 | 20,360,704 |

### 转换为 PNG (ffmpeg)

```bash
# 先用 Python 解包为 8-bit raw，然后：
# BGGR (IMX371, IMX376K):
ffmpeg -f rawvideo -pixel_format bayer_bggr8 -video_size ${WIDTH}x${HEIGHT} \
    -i output_8bit.raw -update 1 output.png -y

# RGGB (IMX519):
ffmpeg -f rawvideo -pixel_format bayer_rggb8 -video_size ${WIDTH}x${HEIGHT} \
    -i output_8bit.raw -update 1 output.png -y
```

### 支持的 V4L2 像素格式

video 节点支持以下格式（可通过 `--set-fmt-video pixelformat=` 设置）：

| FourCC | 描述 | 推荐场景 |
|--------|------|----------|
| pBAA | 10-bit Bayer BGGR Packed | IMX371/IMX376K 默认使用 |
| pRAA | 10-bit Bayer RGGB Packed | IMX519 默认使用 |
| BA81 | 8-bit Bayer BGGR | 节省带宽，损失精度 |
| BG10 | 10-bit Bayer BGGR (unpacked, 16bpp) | 处理方便但体积翻倍 |
| GREY | 8-bit Greyscale | 灰度场景 |
| YUYV/UYVY | YUV 4:2:2 | 需经 VFE PIX 处理 |

## 注意事项

1. **第一帧为空**: `--stream-count` 至少设为 2，建议 5，使用最后一帧
2. **切换摄像头**: 必须先断开当前链路（设为 `[0]`），再启用新链路
3. **analogue_gain 默认为 0**: 不设置增益会导致画面全黑
4. **无 ISP 处理**: RDI 输出是原始 Bayer 数据，无白平衡、去马赛克、降噪；如需 ISP 处理应使用 VFE PIX 路径（video3/7/11）
5. **Sensor rotation**: IMX371(前置) rotation=90°, IMX519/IMX376K(后置) rotation=270°，应用需自行旋转
6. **同时使用多个摄像头**: 可使用不同的 CSID 和 VFE，例如后置走 csid0→vfe0，前置走 csid1→vfe1
7. **IMMUTABLE 链路不可更改**: sensor→csiphy 和 rdi→video 的链路是固定的
8. **IMX519 使用 C-PHY**: 与其他两个 sensor 的 D-PHY 不同，IMX519 通过 C-PHY 3-lane 连接 csiphy0
9. **Bayer 格式差异**: IMX519 输出 SRGGB10 (pRAA)，IMX371/IMX376K 输出 SBGGR10 (pBAA)，转换时需注意
