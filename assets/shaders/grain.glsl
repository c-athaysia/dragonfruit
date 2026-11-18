precision highp float;

varying vec2 v_coords;
uniform vec2 size;
uniform vec2 u_camera;
uniform float u_zoom;

float hash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)),
             dot(p, vec2(269.5, 183.3)));
    return fract(sin(p.x * 0.0001 * p.y * 0.0001) * 43758.5453123 + sin(p.x * 0.00001 - p.y * 0.00001) * 28001.8384);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < 4; i++) {
        v += vnoise(p * freq) * amp;
        amp  *= 0.5;
        freq *= 2.17;
    }
    return v;
}

void main() {
    vec2 screen = v_coords * size;
    float zoom = u_zoom > 0.0 ? u_zoom : 1.0;

    // pinned to the plane so the texture belongs to the canvas not the screen
    // parallax under 1.0 drifts slower than the windows
    float parallax = 0.85;
    vec2 canvas = screen / zoom + u_camera * parallax;

    float g = fbm(canvas * 0.7);
    float grain = (g - 0.5) * 0.25;

    // antialiasing for the whole grain not each octave
    // the grain is pixel sized on purpose, base octave is already about 1.4 pixels at 100%
    // fading octaves one by one would change how 100% looks and it has to stay the same
    // so the whole thing fades out as the canvas shrinks instead of turning into crawling static
    // fade is 1.0 from zoom 0.55 up so 100% is untouched
    float fade = smoothstep(0.12, 0.55, zoom);

    // thin grain also looks washed out so raise the opacity going down to keep it looking the same
    // 1.0 at zoom 1 so native zoom doesnt change, up to about 2.4x far out so it doesnt just vanish
    float boost = mix(1.0, 2.4, clamp(1.0 - zoom, 0.0, 1.0));
    float alpha = 0.08 * fade * boost;

    gl_FragColor = vec4(vec3(grain * fade * boost), alpha);
}
