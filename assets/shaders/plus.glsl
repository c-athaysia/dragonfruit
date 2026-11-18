// plus grid - 2px stroke that doesnt change, arms get shorter as you zoom out
// like the dot grid but the marks have a direction
// stroke stays 2 screen pixels at every zoom so no blur
// arms shrink with zoom so it ends up a dot instead of a mess of lines
precision highp float;

varying vec2 v_coords;
uniform vec2 size;
uniform vec2 u_camera;
uniform float u_zoom;

const float SPACING = 80.0;
// half the stroke width in screen pixels, 1.0 is a 2px line
const float STROKE_HALF = 1.0;
// arm half length at zoom 1 in screen pixels
const float ARM_HALF = 6.0;
const float FADE_LO = 6.0;
const float FADE_HI = 14.0;

void main() {
    vec2 screen = v_coords * size;
    float zoom = u_zoom > 0.0 ? u_zoom : 1.0;

    vec2 canvas = screen / zoom + u_camera;
    vec2 node_canvas = floor(canvas / SPACING + 0.5) * SPACING;
    vec2 node_screen = (node_canvas - u_camera) * zoom;

    vec2 d = abs(screen - node_screen);

    // arms shrink with zoom but never below the stroke width
    // so it turns into a square dot and doesnt vanish
    float arm = max(ARM_HALF * zoom, STROKE_HALF);

    // horizontal bar and vertical bar together
    vec2 h = clamp(vec2(arm, STROKE_HALF) + 0.5 - d, 0.0, 1.0);
    vec2 v = clamp(vec2(STROKE_HALF, arm) + 0.5 - d, 0.0, 1.0);
    float mask = max(h.x * h.y, v.x * v.y);

    float pitch = SPACING * zoom;
    float fade = smoothstep(FADE_LO, FADE_HI, pitch);

    // wlroots wants premultiplied alpha so rgb has to be multiplied by alpha already
    // vec4(1.0, 1.0, 1.0, a) is full white at any alpha and the whole screen goes white
    float a = mask * fade * 0.16;
    gl_FragColor = vec4(vec3(1.0) * a, a);
}
