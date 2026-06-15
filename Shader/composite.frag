#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
uniform int uSwizzleBGRA;          // 1=输入是BGRA，需交换R/B
out vec4 FragColor;
void main() {
    vec4 c = texture(uTex, vUv);
    FragColor = (uSwizzleBGRA == 1) ? c.bgra : c;
}