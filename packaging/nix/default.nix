{
  lib,
  libX11,
  libinput,
  libxcb,
  libdrm,
  libxkbcommon,
  pcre2,
  pango,
  cairo,
  glib,
  cjson,
  pixman,
  pkg-config,
  stdenv,
  wayland,
  wayland-protocols,
  wayland-scanner,
  libxcb-wm,
  xwayland,
  meson,
  ninja,
  scenefx,
  wlroots_0_20,
  libGL,
  enableXWayland ? true,
  debug ? false,
}:
stdenv.mkDerivation {
  pname = "dragonfruit";
  version =
    let
      line = lib.findFirst (lib.hasInfix "version :") "" (lib.splitString "\n" (builtins.readFile ../../meson.build));
    in
    builtins.head (builtins.match " *version : '([^']+)',?" line);

  src = lib.cleanSourceWith {
    src = ../..;
    filter =
      path: type:
      let
        base = baseNameOf (toString path);
      in
      (lib.cleanSourceFilter path type) && base != "build" && base != "result";
    name = "source";
  };

  mesonFlags = [
    (lib.mesonEnable "xwayland" enableXWayland)
    (lib.mesonBool "asan" debug)
  ];

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
  ];

  buildInputs = [
    libinput
    libxcb
    libxkbcommon
    pcre2
    pango
    cairo
    glib
    cjson
    pixman
    wayland
    wayland-protocols
    wlroots_0_20
    scenefx
    # glesv2: draw/shader_bg.c issues raw GLES2 calls into the scenefx FBO.
    libGL
    libdrm
  ]
  ++ lib.optionals enableXWayland [
    libX11
    libxcb-wm
    xwayland
  ];

  passthru = {
    providedSessions = [ "dragonfruit" ];
  };

  meta = {
    mainProgram = "dragonfruit";
    description = "An infinite-canvas Wayland compositor forked from Mango";
    homepage = "https://codeberg.org/central-lambda-group/dragonfruit";
    license = lib.licenses.gpl3Plus;
    maintainers = [ ];
    platforms = lib.platforms.linux;
  };
}
