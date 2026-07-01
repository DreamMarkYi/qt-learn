"""
pose_server.py —— 骨骼点检测边车进程（拓扑 B / 新版 MediaPipe Tasks API）

用 MediaPipe Tasks 的 PoseLandmarker，支持 Python 3.12 / 3.13。
旧的 mp.solutions.pose 在新环境里没有，这里改用 Tasks API + .task 模型文件。

职责：
  1. 作为 TCP 服务端监听 127.0.0.1:<port>
  2. C++ 端把摄像头帧编码成 JPEG，按 [4字节大端长度][JPEG字节] 发来
  3. 本进程解码 → PoseLandmarker 检测 33 个骨骼点
  4. 按同样的 [4字节大端长度][JSON] 帧格式回传关键点

协议（与 C++ 端 PoseFrameClient 严格对应，和旧版完全一致）：
  C++  -> Python :  uint32(be) len + JPEG bytes
  Python -> C++  :  uint32(be) len + UTF-8 JSON
  JSON: {"ok": true, "lm": [[x, y, z, vis], ... 共33个 ...]}
        x,y ∈ [0,1] 归一化图像坐标（左上原点），z 相对深度，vis 可见度

运行：
  pip install -r requirements.txt
  python pose_server.py --port 5066          # 首次会自动下载 lite 模型
  python pose_server.py --port 5066 --show    # 额外开窗显示骨架，调试用
"""

import argparse
import json
import math
import os
import socket
import struct
import time
import urllib.request

import cv2
import numpy as np
import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision

# lite / full / heavy 三档：lite 最快最糙，heavy 最准最慢，full 折中（默认）。
MODEL_DIR = os.path.dirname(os.path.abspath(__file__))

HOLISTIC_URL = ("https://storage.googleapis.com/mediapipe-models/holistic_landmarker/"
                "holistic_landmarker/float16/latest/holistic_landmarker.task")
HOLISTIC_PATH = os.path.join(MODEL_DIR, "holistic_landmarker.task")


def model_url(variant: str) -> str:
    return (f"https://storage.googleapis.com/mediapipe-models/pose_landmarker/"
            f"pose_landmarker_{variant}/float16/latest/"
            f"pose_landmarker_{variant}.task")


def model_path_for(variant: str) -> str:
    return os.path.join(MODEL_DIR, f"pose_landmarker_{variant}.task")


def download(url: str, path: str) -> str:
    """文件不存在就下载。返回本地路径。"""
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    print(f"[pose] 未找到模型，正在下载到 {path} …")
    urllib.request.urlretrieve(url, path)
    print(f"[pose] 下载完成 ({os.path.getsize(path) // 1024} KB)")
    return path


# ── One-Euro 滤波：对一组坐标整体平滑，静止稳、快速移动不拖尾 ──────────────
class OneEuroFilter:
    """对 numpy 数组逐元素做 One-Euro 滤波（MediaPipe 官方同款思路）。
    mincutoff 越小越平滑但越滞后；beta 越大对快速动作越跟手。"""
    def __init__(self, mincutoff=1.0, beta=0.3, dcutoff=1.0):
        self.mincutoff = mincutoff
        self.beta = beta
        self.dcutoff = dcutoff
        self.reset()

    def reset(self):
        self.x_prev = None
        self.dx_prev = None
        self.t_prev = None

    @staticmethod
    def _alpha(cutoff, dt):
        tau = 1.0 / (2.0 * math.pi * cutoff)
        return 1.0 / (1.0 + tau / dt)

    def __call__(self, x, t):
        if self.x_prev is None:
            self.x_prev = x
            self.dx_prev = np.zeros_like(x)
            self.t_prev = t
            return x
        dt = t - self.t_prev
        if dt <= 0:
            dt = 1e-3
        dx = (x - self.x_prev) / dt
        a_d = self._alpha(self.dcutoff, dt)
        dx_hat = a_d * dx + (1 - a_d) * self.dx_prev
        cutoff = self.mincutoff + self.beta * np.abs(dx_hat)
        a = self._alpha(cutoff, dt)
        x_hat = a * x + (1 - a) * self.x_prev
        self.x_prev = x_hat
        self.dx_prev = dx_hat
        self.t_prev = t
        return x_hat


# 手部 21 点连线（仅 holistic 模式画手用）
HAND_CONNECTIONS = [
    (0, 1), (1, 2), (2, 3), (3, 4), (0, 5), (5, 6), (6, 7), (7, 8),
    (0, 9), (9, 10), (10, 11), (11, 12), (0, 13), (13, 14), (14, 15), (15, 16),
    (0, 17), (17, 18), (18, 19), (19, 20), (5, 9), (9, 13), (13, 17),
]

# ── 面部表情 → Live2D 语义值 ────────────────────────────────────────────────
# 发给 C++ 的语义值顺序（也是 One-Euro 平滑的向量顺序）
FACE_KEYS = ["eyeLOpen", "eyeROpen", "eyeballX", "eyeballY",
             "mouthOpen", "mouthForm", "browL", "browR", "cheek",
             "angleX", "angleY", "angleZ"]

# 眨眼冻结阈值：任一眼睁开度(eyeLOpen/eyeROpen)低于此值即判定眨眼，
# 眨眼期间冻结头部俯仰(angleY)，沿用上一帧稳定值，进一步压掉残余“点头”抖动。
# 调大→更容易触发冻结(半眨也冻)；调小→只在闭得很死时才冻。
BLINK_HOLD_EYE_OPEN = 0.5

# solvePnP 用的 5 个「刚性」面部点 + 通用 3D 人脸坐标（单位 mm，近似）。
# pitch(低头/抬头) 基准放在鼻梁中线(鼻尖1→鼻梁6→鼻梁顶168)：这条线眨眼、
# 张嘴、挑眉都几乎不动，且相对眼角向前突出，靠「高度+深度」差稳定估 pitch。
# 水平基准只留外眼角(33/263)定 yaw/roll——外眼角是眨眼时漂移最小的点。
# 刻意不用：额头10(随挑眉/皱眉动)、内眼角133/362(贴鼻梁随眨眼上下漂)、
#           下巴152/嘴角61/291(随张嘴大幅移动，会被误判成低头)。
FACE_PNP_IDX = [1, 6, 168, 33, 263]         # 鼻尖/鼻梁中/鼻梁顶/左眼外/右眼外
FACE_PNP_3D = np.array([
    (0.0,    0.0,   0.0),     # 1   鼻尖（最靠前）
    (0.0,   22.0,  -6.0),     # 6   鼻梁中段（中线，略后于鼻尖）
    (0.0,   33.0, -12.0),     # 168 鼻梁顶/眉间（中线，仍明显前于眼角）
    (-43.3, 32.7, -26.0),     # 33  左眼外角（水平基准）
    (43.3,  32.7, -26.0),     # 263 右眼外角（水平基准）
], dtype=np.float64)


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def blend_dict(result):
    """从结果取 52 个 blendshape，返回 {名字: 分数}。无脸返回 None。"""
    bs = getattr(result, "face_blendshapes", None)
    if not bs:
        return None
    cats = bs[0] if isinstance(bs[0], list) else bs   # holistic 单人/兼容多人结构
    return {c.category_name: c.score for c in cats}


def head_pose(face_lm, w, h):
    """用 6 点 solvePnP 估头部 (yaw, pitch, roll)，单位度。失败返回 (0,0,0)。"""
    if len(face_lm) <= max(FACE_PNP_IDX):
        return (0.0, 0.0, 0.0)
    img = np.array([[face_lm[i].x * w, face_lm[i].y * h] for i in FACE_PNP_IDX],
                   dtype=np.float64)
    f = float(w)
    cam = np.array([[f, 0, w / 2.0], [0, f, h / 2.0], [0, 0, 1]], dtype=np.float64)
    dist = np.zeros((4, 1))
    # 用 EPnP：支持 4+ 点（ITERATIVE 对非共面点要 ≥6 点）。我们刻意只取 5 个
    # 刚性点(鼻梁中线+外眼角)避免表情污染，所以不能用 ITERATIVE。
    ok, rvec, tvec = cv2.solvePnP(FACE_PNP_3D, img, cam, dist,
                                  flags=cv2.SOLVEPNP_EPNP)
    if not ok:
        return (0.0, 0.0, 0.0)
    R, _ = cv2.Rodrigues(rvec)
    proj = np.hstack((R, tvec.reshape(3, 1)))
    euler = cv2.decomposeProjectionMatrix(proj)[6]
    pitch, yaw, roll = float(euler[0]), float(euler[1]), float(euler[2])
    if pitch > 90:      # decompose 的 pitch 容易翻到 ±180，绕回来
        pitch -= 180
    elif pitch < -90:
        pitch += 180
    return (yaw, pitch, roll)


def face_payload(result, w, h):
    """holistic 结果 → Live2D 语义值字典。无脸返回 {"ok": False}。"""
    bd = blend_dict(result)
    fl = result.face_landmarks
    if fl and isinstance(fl[0], list):
        fl = fl[0]
    if not bd or not fl:
        return {"ok": False}

    g = lambda k: float(bd.get(k, 0.0))
    yaw, pitch, roll = head_pose(fl, w, h)
    return {
        "ok": True,
        # 眼：blendshape 是「闭」的程度，Live2D 要「开」的程度，取 1-闭
        "eyeLOpen": _clamp(1.0 - g("eyeBlinkLeft") *2.2,  0.0, 1.0),
        "eyeROpen": _clamp(1.0 - g("eyeBlinkRight") *2.2, 0.0, 1.0),
        # 视线：左右/上下各取两眼平均
        "eyeballX": _clamp((g("eyeLookOutLeft") + g("eyeLookInRight")) / 2
                           - (g("eyeLookInLeft") + g("eyeLookOutRight")) / 2, -1.0, 1.0),
        "eyeballY": _clamp((g("eyeLookUpLeft") + g("eyeLookUpRight")) / 2
                           - (g("eyeLookDownLeft") + g("eyeLookDownRight")) / 2, -1.0, 1.0),
        # 嘴：张合 + 形状(笑为正、嘟嘴为负)
        "mouthOpen": _clamp(g("jawOpen") * 3.2, 0.0, 1.0),
        "mouthForm": _clamp((g("mouthSmileLeft") + g("mouthSmileRight")) / 2
                            - g("mouthPucker"), -1.0, 1.0),
        # 眉：上挑为正、下压为负
        "browL": _clamp((g("browOuterUpLeft") - g("browDownLeft")) *2, -1.0, 1.0),
        "browR": _clamp((g("browOuterUpRight") - g("browDownRight")) *2, -1.0, 1.0),
        "cheek": _clamp(g("cheekPuff"), 0.0, 1.0),
        # 头部：度数，符号可能需按你画面镜像情况翻转(见 README)
        "angleX": _clamp(yaw,    -30.0, 30.0),
        "angleY": _clamp(-pitch *2  , -30.0, 30.0),   # 取反：上下点头方向反了
        "angleZ": _clamp(-roll,  -30.0, 30.0),
    }


# ── 身体 33 点 → Live2D 身体参数 ───────────────────────────────────────────
# 目前只算 ParamBodyAngleZ：身体左右摆，由两肩(11左/12右)连线相对水平的倾角驱动。
BODY_Z_GAIN  = 1.2     # 倾角(度) → ParamBodyAngleZ 增益，越大摆得越夸张
BODY_Z_LIMIT = 10.0    # ParamBodyAngleZ 标准范围 ±10
BODY_MIN_VIS = 0.5     # 两肩可见度都需 ≥ 此值，否则判定不可靠、不更新


def body_payload(lm, vis, w, h):

    """已平滑身体点 lm(N,3,归一化) + 可见度 vis → 身体语义值。无效返回 {"ok": False}。"""
    L, R = 11, 12                                  # 左肩 / 右肩（MediaPipe 编号）
    if lm is None or len(lm) <= R:
        return {"ok": False}
    if vis[L] < BODY_MIN_VIS or vis[R] < BODY_MIN_VIS:
        return {"ok": False}
    # 按 x 排序取画面左/右肩，保证 dx>0，倾角落在 [-90,90]，不受画面左右镜像影响。
    a, b = (L, R) if lm[L][0] <= lm[R][0] else (R, L)
    dx = (lm[b][0] - lm[a][0]) * w                 # 转像素空间再求角(x/y 归一化基准不同)
    dy = (lm[b][1] - lm[a][1]) * h
    roll = math.degrees(math.atan2(dy, dx))        # >0：画面右侧肩更低
    # 符号按需翻转（摄像头未镜像时画面是镜像的，方向反了就把这里的负号去掉）
    angleZ = _clamp(-roll * BODY_Z_GAIN, -BODY_Z_LIMIT, BODY_Z_LIMIT)
    return {"ok": True, "angleZ": float(angleZ)}


# MediaPipe Pose 33 点之间的骨架连线（关节->关节），用于可视化画骨头。
POSE_CONNECTIONS = [
    # 脸
    (0, 1), (1, 2), (2, 3), (3, 7), (0, 4), (4, 5), (5, 6), (6, 8), (9, 10),
    # 躯干
    (11, 12), (11, 23), (12, 24), (23, 24),
    # 左臂
    (11, 13), (13, 15), (15, 17), (15, 19), (15, 21), (17, 19),
    # 右臂
    (12, 14), (14, 16), (16, 18), (16, 20), (16, 22), (18, 20),
    # 左腿
    (23, 25), (25, 27), (27, 29), (27, 31), (29, 31),
    # 右腿
    (24, 26), (26, 28), (28, 30), (28, 32), (30, 32),
]


def draw_landmarks(frame, pts, connections, line_color, pt_color, pt_r=3) -> None:
    """通用：在 BGR 图上画一组关键点（先连线后画点）。pts 为 landmark 对象列表。"""
    if not pts:
        return
    h, w = frame.shape[:2]
    px = [(int(p.x * w), int(p.y * h)) for p in pts]
    for a, b in connections:
        if a < len(px) and b < len(px):
            cv2.line(frame, px[a], px[b], line_color, 2)
    for x, y in px:
        cv2.circle(frame, (x, y), pt_r, pt_color, -1)


def recv_exactly(conn: socket.socket, n: int) -> bytes | None:
    """从 conn 精确读取 n 字节；对端关闭则返回 None。"""
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def recv_frame(conn: socket.socket) -> bytes | None:
    """读取一帧：4 字节大端长度 + 该长度的 JPEG 字节。"""
    header = recv_exactly(conn, 4)
    if header is None:
        return None
    (length,) = struct.unpack(">I", header)
    if length == 0 or length > 32 * 1024 * 1024:
        return None
    return recv_exactly(conn, length)


def send_json(conn: socket.socket, obj: dict) -> None:
    """按 4 字节大端长度 + UTF-8 JSON 发回。"""
    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    conn.sendall(struct.pack(">I", len(payload)) + payload)


def extract_pose(mode: str, result):
    """从检测结果里取出 33 个身体点。两种模式结构不同：
       pose     模式: result.pose_landmarks 是 list[ list[lm] ]，取第一个人
       holistic 模式: result.pose_landmarks 是扁平 list[lm]，直接用。"""
    pls = result.pose_landmarks
    if not pls:
        return None
    if mode == "holistic":
        return pls
    return pls[0]


def serve_client(conn, landmarker, mode, smoother, face_smoother, show) -> None:
    frames = 0
    t0 = time.time()
    last_ts = 0                                   # VIDEO 模式要求时间戳严格递增(ms)
    hold_angles = None                            # 眨眼冻结用：上一帧稳定的{angleX/Y/Z}
    win = f"{mode} (press q to close window)"
    while True:
        jpeg = recv_frame(conn)
        if jpeg is None:
            print("[pose] 客户端断开")
            return

        arr = np.frombuffer(jpeg, dtype=np.uint8)
        frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)   # BGR
        if frame is None:
            send_json(conn, {"ok": False})
            continue

        rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        ts = int(time.time() * 1000)
        if ts <= last_ts:
            ts = last_ts + 1
        last_ts = ts

        result = landmarker.detect_for_video(mp_image, ts)
        pts = extract_pose(mode, result)

        # ---- 身体 33 点 ----
        packet = {"ok": bool(pts)}
        if pts:
            raw = np.array([[p.x, p.y, p.z] for p in pts], dtype=np.float32)
            sm = smoother(raw, ts / 1000.0)
            vis = [float(getattr(pts[i], "visibility", 0.0) or 0.0)
                   for i in range(len(pts))]
            packet["lm"] = [[float(sm[i, 0]), float(sm[i, 1]), float(sm[i, 2]), vis[i]]
                            for i in range(len(pts))]
            # 身体语义值（目前只有 ParamBodyAngleZ，由两肩连线倾角算，用已平滑点）
            packet["body"] = body_payload(sm, vis, frame.shape[1], frame.shape[0])
        else:
            smoother.reset()                          # 丢检测就清滤波状态，避免回来时拖影

        # ---- 面部表情（仅 holistic）----
        if mode == "holistic":
            fp = face_payload(result, frame.shape[1], frame.shape[0])
            if fp.get("ok"):
                # 眨眼检测 → 冻结头部角度：任一眼睁开度低于阈值即判定眨眼，此时不更新
                # angleX/Y/Z，沿用上一帧稳定值，切断眨眼对转头/点头/歪头的残余影响。
                blinking = (fp["eyeLOpen"] < BLINK_HOLD_EYE_OPEN
                            or fp["eyeROpen"] < BLINK_HOLD_EYE_OPEN)
                if blinking and hold_angles is not None:
                    for k in ("angleX", "angleY", "angleZ"):
                        fp[k] = hold_angles[k]        # 冻结：眨眼期间锁住三个头部角
                else:
                    hold_angles = {k: fp[k] for k in ("angleX", "angleY", "angleZ")}
                vec = np.array([fp[k] for k in FACE_KEYS], dtype=np.float32)
                sm = face_smoother(vec, ts / 1000.0)  # 表情/头角单独一套 One-Euro
                for i, k in enumerate(FACE_KEYS):
                    fp[k] = float(sm[i])
            else:
                face_smoother.reset()
                hold_angles = None                    # 丢脸就清掉冻结基准
            packet["face"] = fp

        send_json(conn, packet)

        # ---- 调试可视化 / 帧率统计 ----
        if show:
            if pts:
                draw_landmarks(frame, pts, POSE_CONNECTIONS, (0, 200, 255), (0, 255, 0), 4)
            if mode == "holistic":
                draw_landmarks(frame, getattr(result, "face_landmarks", None),
                               [], (200, 200, 200), (200, 200, 200), 1)
                draw_landmarks(frame, getattr(result, "left_hand_landmarks", None),
                               HAND_CONNECTIONS, (255, 180, 0), (0, 255, 255), 3)
                draw_landmarks(frame, getattr(result, "right_hand_landmarks", None),
                               HAND_CONNECTIONS, (255, 180, 0), (0, 255, 255), 3)
            cv2.imshow(win, frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                cv2.destroyAllWindows()
                show = False

        frames += 1
        if frames % 60 == 0:
            dt = time.time() - t0
            print(f"[pose] [{mode}] {frames} 帧, 平均 {frames / dt:.1f} fps")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5066)
    ap.add_argument("--mode", default="pose", choices=["pose", "holistic"],
                    help="pose: 仅身体33点(默认) / holistic: 身体+脸+双手，便于对比")
    ap.add_argument("--variant", default="full", choices=["lite", "full", "heavy"],
                    help="pose 模式的模型档位：lite 快 / full 折中(默认) / heavy 最准")
    ap.add_argument("--model", default=None,
                    help="手动指定 .task 路径，覆盖 --variant/--mode 的默认模型")
    ap.add_argument("--min-cutoff", type=float, default=1.0,
                    help="One-Euro 最小截止频率：越小越平滑越滞后(默认1.0)")
    ap.add_argument("--beta", type=float, default=0.3,
                    help="One-Euro beta：越大对快速动作越跟手(默认0.3)")
    ap.add_argument("--show", action="store_true", help="开窗显示骨架")
    args = ap.parse_args()

    # 按模式选模型并构建对应 Landmarker（都用 VIDEO 模式）
    if args.mode == "holistic":
        model_path = args.model if args.model else download(HOLISTIC_URL, HOLISTIC_PATH)
        options = vision.HolisticLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=model_path),
            running_mode=vision.RunningMode.VIDEO,
            output_face_blendshapes=True,        # ← 表情驱动的核心：输出 52 个 blendshape
        )
        make_landmarker = lambda: vision.HolisticLandmarker.create_from_options(options)
    else:
        model_path = (args.model if args.model
                      else download(model_url(args.variant), model_path_for(args.variant)))
        options = vision.PoseLandmarkerOptions(
            base_options=mp_python.BaseOptions(model_asset_path=model_path),
            running_mode=vision.RunningMode.VIDEO,
            num_poses=1,
            min_pose_detection_confidence=0.5,
            min_tracking_confidence=0.5,
        )
        make_landmarker = lambda: vision.PoseLandmarker.create_from_options(options)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    print(f"[pose] 模式={args.mode}  监听 {args.host}:{args.port}，等待 C++ 端连接…")

    try:
        with make_landmarker() as landmarker:
            while True:
                conn, addr = srv.accept()
                conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                print(f"[pose] 已连接: {addr}")
                smoother = OneEuroFilter(args.min_cutoff, args.beta)        # 身体点
                face_smoother = OneEuroFilter(args.min_cutoff, args.beta)   # 表情/头角
                try:
                    serve_client(conn, landmarker, args.mode, smoother,
                                 face_smoother, args.show)
                except ConnectionError as e:
                    print(f"[pose] 连接异常: {e}")
                finally:
                    conn.close()
    except KeyboardInterrupt:
        print("\n[pose] 退出")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
