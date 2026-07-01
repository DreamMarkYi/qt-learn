#ifndef POSE_BUS_H
#define POSE_BUS_H

#include "PoseTypes.h"
#include <mutex>

// 线程安全的最新姿态/表情快照。
// 写入方：PoseFrameClient（主线程，收到边车数据时 publish）
// 读取方：L2dLayer::render（渲染线程，每帧读一次驱动模型）
// 只存「最新一份」，旧的直接覆盖——驱动只关心当前状态，不需要历史。
class PoseBus
{
public:
    void publishPose(const PoseData& d) { std::lock_guard<std::mutex> g(_m); _pose = d; }
    void publishFace(const FaceData& d) { std::lock_guard<std::mutex> g(_m); _face = d; }
    void publishBody(const BodyData& d) { std::lock_guard<std::mutex> g(_m); _body = d; }

    PoseData pose() const { std::lock_guard<std::mutex> g(_m); return _pose; }
    FaceData face() const { std::lock_guard<std::mutex> g(_m); return _face; }
    BodyData body() const { std::lock_guard<std::mutex> g(_m); return _body; }

private:
    mutable std::mutex _m;
    PoseData _pose;
    FaceData _face;
    BodyData _body;
};

#endif // POSE_BUS_H
