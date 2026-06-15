#version 330 core
layout(location=0) in vec2 aPos;   // 单位四边形 [0,1]x[0,1]
uniform vec4 uRect;                // x,y,w,h 均为 NDC
out vec2 vUv;
void main() {
    vUv = vec2(aPos.x, 1.0 - aPos.y);          // 翻转 V：GL 纹理原点在左下
    vec2 ndc = uRect.xy + aPos * uRect.zw;     // 映射到目标矩形
    gl_Position = vec4(ndc, 0.0, 1.0);
}