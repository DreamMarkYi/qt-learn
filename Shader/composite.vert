#version 330 core
layout(location=0) in vec2 aPos;   // 单位四边形 [0,1]x[0,1]
uniform vec4 uRect;                // x,y,w,h 均为 NDC
uniform int  uFlipV;               // 1=翻转 V（FBO 离屏纹理自下而上时用）
out vec2 vUv;
void main() {
    // 默认翻转 V：从 QImage 上传的纹理原点在左上，需 1-y。
    // FBO 渲染结果原点已在左下，与 GL 一致，则不翻（uFlipV=1 抵消默认翻转）。
    float v = (uFlipV == 1) ? aPos.y : (1.0 - aPos.y);
    vUv = vec2(aPos.x, v);
    vec2 ndc = uRect.xy + aPos * uRect.zw;     // 映射到目标矩形
    gl_Position = vec4(ndc, 0.0, 1.0);
}