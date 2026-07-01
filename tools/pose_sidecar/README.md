# pose_sidecar —— 骨骼点检测边车

C++ 主程序（qt-learn）把摄像头帧通过 TCP 发到这里，本进程用 MediaPipe Pose
检测 33 个骨骼点后回传。属于「拓扑 B」：摄像头由 C++ 独占，Python 不开摄像头，
因此不会和推流争用设备。

用新版 MediaPipe **Tasks API**（PoseLandmarker），支持 Python 3.12 / 3.13。
旧的 `mp.solutions.pose` 在新环境里没有，故改走 Tasks API + `.task` 模型文件。

## 安装

```bash
cd tools/pose_sidecar
python -m venv .venv
.venv\Scripts\activate        # Windows
pip install -r requirements.txt
```

## 运行

**先启动边车，再启动 C++ 程序**（C++ 端连不上会每 2s 自动重连，所以顺序不严格）：

```bash
# 默认：pose 模式 + full 模型 + One-Euro 平滑
python pose_server.py --port 5066 --show

# 对比用 holistic（身体 + 脸 + 双手），半身/表情场景更直观
python pose_server.py --port 5066 --mode holistic --show
```

### 启动参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--mode` | `pose` | `pose`=仅身体33点；`holistic`=身体+脸+双手（对比用） |
| `--variant` | `full` | 仅 pose 模式：`lite`/`full`/`heavy`，越后越准越慢 |
| `--min-cutoff` | `1.0` | One-Euro 平滑：**越小越稳但越滞后**（抖就调小，如 0.5） |
| `--beta` | `0.3` | One-Euro：越大对快速动作越跟手（拖尾就调大） |
| `--model` | 自动 | 手动指定 `.task` 路径，覆盖默认下载 |
| `--show` | 关 | 开窗显示骨架/脸/手 |

首次启动按模式自动下载对应模型到本目录（pose 约 6MB / holistic 约 30MB）。
若网络受限，手动下载放到本目录或用 `--model` 指定：
- pose lite/full/heavy: `https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_<variant>/float16/latest/pose_landmarker_<variant>.task`
- holistic: `https://storage.googleapis.com/mediapipe-models/holistic_landmarker/holistic_landmarker/float16/latest/holistic_landmarker.task`

### 调抖动

主要靠 `--min-cutoff`。还抖就往小调：`--min-cutoff 0.5`，更稳的 `0.3`。
觉得跟手慢（滞后）就把它调回 `1.0~1.5`，或加大 `--beta` 到 `0.5`。

控制台会打印帧率；C++ 端会用 qDebug 打印鼻子和左右肩坐标，两边都有输出即链路打通。

## 协议

```
C++  -> Python :  uint32(大端) 长度 + JPEG 字节
Python -> C++  :  uint32(大端) 长度 + UTF-8 JSON
JSON: {"ok": true, "lm": [[x, y, z, vis], ... 共33个 ...]}
      x,y ∈ [0,1] 归一化（左上原点），z 相对深度，vis 可见度
```

关键点编号见 `../../PoseTypes.h`（鼻 0、左右肩 11/12、左右肘 13/14、左右腕 15/16、左右髋 23/24）。
