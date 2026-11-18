// dot grid - dots stay 2x2 screen pixels at every zoom and only the spacing changes
// the size never moves so you can read how far out you are from the spacing
precision highp float;

varying vec2 v_coords;
uniform vec2 size;
uniform vec2 u_camera;
uniform float u_zoom;

// grid spacing in canvas units, on screen its SPACING * zoom
const float SPACING = 80.0;
// half a dot in screen pixels, 1.0 is a 2x2 dot
const float DOT_HALF = 1.0;
// under this on screen spacing the dots turn to noise so fade them
const float FADE_LO = 6.0;
const float FADE_HI = 14.0;

void main() {
    vec2 screen = v_coords * size;
    float zoom = u_zoom > 0.0 ? u_zoom : 1.0;

    // nearest grid node, found in canvas space then moved back to screen space
    // thats why the size stays fixed, only the position gets scaled
    vec2 canvas = screen / zoom + u_camera;
    vec2 node_canvas = floor(canvas / SPACING + 0.5) * SPACING;
    vec2 node_screen = (node_canvas - u_camera) * zoom;

    vec2 d = abs(screen - node_screen);
    // half a pixel of blur so the dot doesnt crawl when the camera moves
    vec2 cov = clamp(DOT_HALF + 0.5 - d, 0.0, 1.0);
    float dot_mask = cov.x * cov.y;

    float pitch = SPACING * zoom;
    float fade = smoothstep(FADE_LO, FADE_HI, pitch);

    // wlroots wants premultiplied alpha so rgb has to be multiplied by alpha already
    // vec4(1.0, 1.0, 1.0, a) is full white at any alpha and the whole screen goes white
    float alpha = dot_mask * fade * 0.16;
    gl_FragColor = vec4(vec3(1.0) * alpha, alpha);
}
