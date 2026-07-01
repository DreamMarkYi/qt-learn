#ifndef POSE_TYPES_H
#define POSE_TYPES_H

#include <array>
#include <QMetaType>

// MediaPipe Pose 输出的 33 个关键点。每个点 = {x, y, z, visibility}。
//   x, y ∈ [0,1] 归一化图像坐标（左上原点）
//   z    相对深度（以髋部中点为参考，越小越靠近相机）
//   vis  可见度 [0,1]
struct PoseData {
    bool ok = false;                                   // 本帧是否检测到人
    std::array<std::array<float, 4>, 33> lm{};         // [idx] = {x,y,z,vis}
};

// 面部表情语义值（Python 端已从 blendshape + solvePnP 算好并平滑）。
// 直接对应 Live2D 标准参数，C++ 端写进模型即可。
struct FaceData {
    bool  ok       = false;
    float eyeLOpen = 1.f, eyeROpen = 1.f;   // 0..1  → ParamEyeLOpen / ParamEyeROpen
    float eyeballX = 0.f, eyeballY = 0.f;   // -1..1 → ParamEyeBallX / ParamEyeBallY
    float mouthOpen= 0.f;                   // 0..1  → ParamMouthOpenY
    float mouthForm= 0.f;                   // -1..1 → ParamMouthForm
    float browL    = 0.f, browR    = 0.f;   // -1..1 → ParamBrowLY / ParamBrowRY
    float cheek    = 0.f;                   // 0..1  → ParamCheek
    float angleX   = 0.f, angleY   = 0.f, angleZ = 0.f;  // 度 → ParamAngleX/Y/Z
};

// 身体语义值（Python 端已从骨骼点算好）。直接对应 Live2D 身体参数。
struct BodyData {
    bool  ok     = false;
    float angleZ = 0.f;   // 度 → ParamBodyAngleZ（身体左右摆，由两肩连线倾角驱动）
};

// 常用关键点索引（MediaPipe Pose 官方编号）
enum PoseLandmark {
    PL_NOSE          = 0,
    PL_LEFT_SHOULDER = 11,
    PL_RIGHT_SHOULDER= 12,
    PL_LEFT_ELBOW    = 13,
    PL_RIGHT_ELBOW   = 14,
    PL_LEFT_WRIST    = 15,
    PL_RIGHT_WRIST   = 16,
    PL_LEFT_HIP      = 23,
    PL_RIGHT_HIP     = 24,
};

Q_DECLARE_METATYPE(PoseData)
Q_DECLARE_METATYPE(FaceData)
Q_DECLARE_METATYPE(BodyData)

#endif // POSE_TYPES_H
