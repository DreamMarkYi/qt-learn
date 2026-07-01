#version 330 core
// NV12 → RGB 解码。两平面：Y(R8 全分辨率) + UV(RG8 半分辨率，.r=U .g=V)。
// 颜色空间 / 取值范围由 uniform 传入，避免写死导致偏色。
in vec2 vUv;
uniform sampler2D uTexY;     // R8，亮度，全分辨率
uniform sampler2D uTexUV;    // RG8，色度，半分辨率（双线性自动上采样）
uniform int uColorSpace;     // 0=BT.601, 1=BT.709
uniform int uFullRange;      // 0=limited(视频范围 16-235), 1=full(0-255)
out vec4 FragColor;

void main() {
    float Y = texture(uTexY,  vUv).r;
    float U = texture(uTexUV, vUv).r;
    float V = texture(uTexUV, vUv).g;

    // 1) 先把 Y'CbCr 归一到 full-range（[0,1] 的 Y、[-0.5,0.5] 的色度）
    float Yc, Uc, Vc;
    if (uFullRange == 1) {
        Yc = Y;
        Uc = U - 0.5;
        Vc = V - 0.5;
    } else {
        Yc = (Y - 0.0627451) * 1.164384;     // (Y-16/255) * 255/219
        Uc = (U - 0.501961)  * 1.138393;     // (C-128/255) * 255/224
        Vc = (V - 0.501961)  * 1.138393;
    }

    // 2) 再套对应色彩空间的 full-range 矩阵
    vec3 rgb;
    if (uColorSpace == 1) {   // BT.709
        rgb = vec3(Yc + 1.574800 * Vc,
                   Yc - 0.187324 * Uc - 0.468124 * Vc,
                   Yc + 1.855600 * Uc);
    } else {                  // BT.601
        rgb = vec3(Yc + 1.402000 * Vc,
                   Yc - 0.344136 * Uc - 0.714136 * Vc,
                   Yc + 1.772000 * Uc);
    }

    FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
