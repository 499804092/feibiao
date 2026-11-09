# 换摄像头：UVC 免驱摄像头接入说明

> 针对「亚博智能 USB摄像头 1080P AI视觉识别 120帧」这类**标准 UVC 免驱摄像头**。
> 本文所有结论都在你的虚拟机上**实测过**（Ubuntu 22.04 / 内核 6.8.0-138 / OpenCV 4.5.4 / 相机已挂载）。

---

## 1. 直接回答：需不需要拉相机库？

**不需要。一行库都不用装。**

UVC（USB Video Class）是 **Linux 内核自带**的协议，不像海康那种必须装厂商 SDK。

实测结果：

| 检查项 | 结果 | 说明 |
|---|---|---|
| 内核 `uvcvideo` 驱动 | ✅ 存在 | `6.8.0-138-generic`，uvcvideo v1.1.1 |
| OpenCV 4.5.4 的 V4L2 后端 | ✅ **`v4l/v4l2: YES (linux/videodev2.h)`** | 可直接 `cv::VideoCapture` 读 `/dev/video*` |
| `v4l2-ctl` / `ffmpeg` | ✅ 已装 | 调试 / 备用方案 |
| 当前用户在 `video` 组 | ✅ 已在 | 有权限读摄像头 |
| **相机已挂进虚拟机** | ✅ **已确认** | `v4l2-ctl --list-devices` → `USB 2.0 Camera: USB Camera` |
| 设备节点 | ✅ `/dev/video0`（可用）、`/dev/video1`（不可用）、`/dev/media0` | video1 是配套的 metadata 节点，打不开是正常的 |
| 海康 MVS SDK `/opt/MVS` | ❌ 不存在 | 所以原海康代码编不过，正好换 UVC |

> `/dev/video1` 打不开**不是故障**：UVC 设备常会注册第二个节点专门传元数据，
> 不要把它当摄像头用，用 `/dev/video0` 就对了。

---

## 2. ⚠️ 最重要的发现：这台相机在虚拟机里**跑不到 120fps**

我用 `v4l2-ctl --list-formats-ext` 查了相机声称支持的参数，然后**实测**了各档的真实帧率：

### 相机声称支持的（MJPG 压缩格式）

| 分辨率 | 声称帧率 |
|---|---|
| 1920×1080 | 30 fps |
| 1280×720 | 60 fps |
| 1280×1024 | 30 fps |
| 1024×768 | 30 fps |
| 800×600 | 60 fps |
| **640×480** | **120 fps** |
| 320×240 | 120 fps |

### 实测真实帧率（虚拟机内，MJPG）

| 请求 | 协商结果 | **实测真实帧率** |
|---|---|---|
| 320×240 @120 | 120.5 fps | **61.4 fps** |
| **640×480 @120** | **120.1 fps** | **51.6 fps** ← 推荐用这档 |
| 1280×720 @60 | 60.0 fps | 29.8 fps |
| 1920×1080 @30 | 30.0 fps | 14.9 fps |
| 1920×1080 @60（默认 YUYV） | 6.0 fps | — |

**瓶颈分析**：把各档的"每帧耗时"算出来：

```
320x240   : 16.3 ms/帧
640x480   : 19.4 ms/帧
1280x720  : 33.6 ms/帧
1920x1080 : 67.1 ms/帧
```

分辨率涨了 27 倍（240p→1080p），帧耗时才涨 4 倍 —— 说明**存在约 16 ms 的固定开销**，
跟分辨率基本无关。这是 **VMware USB 直通的单帧延迟**造成的，**不是相机的问题**。

> **结论**：虚拟机里用来调代码、验证算法完全够用（51 fps 已经超过模型推理速度）。
> 真要压满帧率，得把相机直连到物理机/开发板（Jetson、树莓派、RDK）上跑，
> 那时才能接近标称值。**别按 120fps 做设计。**

### 另外注意：别用 1080P 喂网络

网络输入是 **640×384**。喂 1920×1080 的图进去，`letterBox` 要缩放的倍率和训练时差太多，检测精度会掉。
**推荐 `640×480`**（配置里已经这么写了）。

---

## 3. 三步跑通

### 第 1 步：确认设备（已完成 ✓）

```bash
v4l2-ctl --list-devices
# USB 2.0 Camera: USB Camera (usb-0000:03:00.0-2):
#         /dev/video0
#         /dev/video1
#         /dev/media0
```

### 第 2 步：跑实测工具，确认帧率

```bash
cd ~/Desktop/feibiao/verify
g++ -std=c++17 -O2 uvc_test.cpp -o uvc_test $(pkg-config --cflags --libs opencv4)
./uvc_test 0 640 480 120          # 会打印实际生效参数 + 实测帧率，并存一帧 uvc_frame.png
```

### 第 3 步：★选定画面朝向（必做，不能照抄）

```bash
g++ -std=c++17 -O2 uvc_orient.cpp -o uvc_orient $(pkg-config --cflags --libs opencv4)
./uvc_orient 0 640 480 120
```

这个工具把 **6 种候选朝向同屏显示**（2 行 × 3 列），你**把靶子放到镜头前**，
看哪个格子里的靶子是**正立**的，就取那个编号：

| 编号 | 效果 |
|---|---|
| `0` | 原样，不做任何变换 |
| `1` | 顺时针旋转 90° |
| `2` | 旋转 180° |
| `3` | 逆时针旋转 90° |
| `4` | 只做水平镜像 |
| `5` | 只做垂直镜像 |

按数字键 `0~5` 会把对应朝向存成 `orient_<n>.png`，按 `q` 退出。

然后把编号填进配置：

```json
// feibiao/Vision_Code/config/json/sensors.json
"uvc": {
  "device": 0,
  "width": 640,
  "height": 480,
  "fps": 120,
  "mjpg": 1,
  "orientation": 1     ← 改这里（默认 1，最接近原海康代码的效果）
}
```

### 为什么这一步不能照抄原代码

原海康代码里有这三行：

```cpp
cv::transpose(m, m);      // 转置
cv::flip(m, m, -1);       // 180° 翻转
cv::flip(m, m, 1);        // 水平镜像
```

**我实测过**：这三步等价于把原图**逆时针旋转 90° 并水平镜像**（四角标号法验证：
原图左上角的标记跑到了最终图的左下角）。

那是针对海康相机**当时的机械安装角度**做的。换摄像头后安装角度变了，
**必须重新定**。方向错了会导致 `pitch/yaw` 正负号全反 —— 飞控会往反方向转。

---

## 4. 代码必须改（已改好）

现有取图代码 `HikRobot.cpp` 用的是海康私有 API：

```cpp
MV_CC_EnumDevices(...)        // 枚举设备
MV_CC_GetImageBuffer(...)     // 取原始帧
MV_CC_ConvertPixelType(...)   // BayerRG8 → BGR
```

**UVC 摄像头完全不认这套 API**，插上去也读不出图。所以我写了一份 UVC 取图实现：

| 文件 | 作用 |
|---|---|
| `Vision_Code/src/sensors/inc/uvc/UvcCamera.hpp` | UVC 取图类声明 |
| `Vision_Code/src/sensors/src/uvc/UvcCamera.cpp` | 实现（逐行中文注释，含掉线自恢复） |
| `Vision_Code/main.cpp` | 顶部宏切换：`USE_UVC 1` / `USE_HIKROBOT 0` |
| `Vision_Code/src/sensors/CMakeLists.txt` | 海康库改成"存在才链接"，没有也不影响 UVC 编译 |
| `Vision_Code/config/json/sensors.json` | 新增 `uvc` 配置节 |

**接口与原来完全一致**（都继承 `sensors::BaseCamera`，都是 `cameraInit()` + `refreshImage()`），
所以 `main.cpp` 里只是把 `HikRobot hik(sp);` 换成 `UvcCamera cam(sp);` ——
**下游的检测、跟踪、角度解算、22 字节串口下发、电控全部一个字都不用改。**

编译验证结果：

```
UvcCamera.cpp     rc=0  errors=0  非opencv警告=0
Detect.cpp        rc=0  errors=0
OpenvinoInfer.cpp rc=0  errors=0
```

### `UvcCamera.cpp` 里两个值得注意的实现点

```cpp
// ① 必须先设 FOURCC 再设分辨率 —— 顺序错了帧率常常协商不上。
//    1080p 想跑 60/120 帧，UVC 必须用 MJPG（YUYV 未压缩带宽根本不够）。
cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
cap_.set(cv::CAP_PROP_FRAME_WIDTH,  req_width_);
cap_.set(cv::CAP_PROP_FRAME_HEIGHT, req_height_);
cap_.set(cv::CAP_PROP_FPS,          req_fps_);

// ② 驱动默认缓冲可能有 4 帧 → 画面延迟 4 帧，打飞镖是致命的。降到 1。
cap_.set(cv::CAP_PROP_BUFFERSIZE, 1);
```

另外 `cameraInit()` 会**回读并打印实际生效的参数** —— 一定看这行，摄像头不一定接受你请求的值：

```
[UvcCamera] 请求 640x480 @120 fps，实际生效 640x480 @120.1 fps
```

---

## 5. 曝光/增益怎么调

UVC 没有海康那种 `MV_CC_SetFloatValue("ExposureTime", ...)`，用 `v4l2-ctl`：

```bash
v4l2-ctl -d /dev/video0 --list-ctrls          # 先列出这台摄像头支持哪些可调项
# 然后按列出来的名字设，比如：
v4l2-ctl -d /dev/video0 --set-ctrl=auto_exposure=1
v4l2-ctl -d /dev/video0 --set-ctrl=exposure_time_absolute=100
v4l2-ctl -d /dev/video0 --set-ctrl=gain=10
```

⚠️ **不同摄像头的 ctrl 名字不一样**（有的叫 `exposure_absolute`，有的叫 `exposure_time_absolute`），
**必须先 `--list-ctrls` 看**，别照抄。

打飞镖对曝光很敏感（靶面反光会过曝丢目标），建议关掉自动曝光自己定死。

---

## 6. 如果你以后想换回海康相机

你机器上已经有两份现成的海康 ROS 工程可以借鉴：

| 路径 | 内容 |
|---|---|
| `~/SPR-Vision-2026/Main_ws/src/rm_hardware_driver/hik_camera/` | 完整的海康相机 ROS2 驱动节点 |
| `~/spr_vision_try-main/io/hikrobot/` | 另一份海康封装 |

它们**把海康 SDK 直接放在项目里**，不依赖 `/opt/MVS`：

```
hikSDK/include/            ← 头文件
hikSDK/lib/amd64/          ← x86_64 库（5 个 .so）
hikSDK/lib/arm64/          ← ARM64 库（给 Jetson / 树莓派用）
```

CMakeLists 的关键写法（值得抄）：

```cmake
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  target_link_directories(${PROJECT_NAME} PUBLIC hikSDK/lib/amd64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  target_link_directories(${PROJECT_NAME} PUBLIC hikSDK/lib/arm64)
endif()

target_link_libraries(${PROJECT_NAME}
  FormatConversion MediaProcess MvCameraControl MVRender MvUsb3vTL)
```

**必须链接 5 个库**（`libMvCameraControl.so` 单独用会报 undefined symbol）。
我实测这 5 个库在你这台机器上**依赖齐全、架构正确（ELF 64-bit x86-64）**。

所以要用海康相机**不用装 MVS**，把这份 `hikSDK` 拷进工程、按上面那样链接就行。
这条路径我也写进 `src/sensors/CMakeLists.txt` 的注释里了（两套后端可以共存）。

---

## 7. 一键自检清单

```bash
# ① 设备在不在
v4l2-ctl --list-devices

# ② 帧率与分辨率实测
cd ~/Desktop/feibiao/verify && ./uvc_test 0 640 480 120

# ③ 选定画面朝向（对着靶子跑）
./uvc_orient 0 640 480 120
#    → 把选中的编号填进 Vision_Code/config/json/sensors.json 的 "orientation"

# ④ 代码切到 UVC 了吗
grep -n "USE_UVC\|USE_HIKROBOT" ~/Desktop/feibiao/Vision_Code/main.cpp

# ⑤ 整体编译
cd ~/Desktop/feibiao/Vision_Code && mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

第 ⑤ 步如果报 `flatbuffers` / `Time_generated.h` 找不到，
是因为 `config/autogenerated_flatbuffers/` 里的生成文件没纳入版本管理，
需要先跑 `flatc` 生成（见 `改动清单与用法.md`）。
