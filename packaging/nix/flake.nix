{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    scenefx = {
      url = "github:wlrfx/scenefx";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      flake-parts,
      ...
    }@inputs:
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [
        inputs.flake-parts.flakeModules.easyOverlay
      ];

      flake = {
        hmModules.dragonfruit = import ./hm-modules.nix self;
        nixosModules.dragonfruit = import ./nixos-modules.nix self;
      };

      perSystem =
        {
          config,
          pkgs,
          ...
        }:
        let
          inherit (pkgs) callPackage;
          dragonfruit = callPackage ./default.nix {
            scenefx = inputs.scenefx.packages.${pkgs.stdenv.hostPlatform.system}.default;
          };
          shellOverride = old: {
            nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.clang-tools ];
            buildInputs = old.buildInputs ++ [ ];
          };
        in
        {
          overlayAttrs = {
            inherit (config.packages) dragonfruit;
          };
          packages = {
            inherit dragonfruit;
            default = dragonfruit;
            hm-options-json = pkgs.callPackage (import ./generate-options.nix self) {
              module = ./hm-modules.nix;
              optionPrefix = "wayland.windowManager.dragonfruit.";
            };
            nixos-options-json = pkgs.callPackage (import ./generate-options.nix self) {
              module = ./nixos-modules.nix;
              optionPrefix = "programs.dragonfruit.";
            };
          };
          devShells.default = dragonfruit.overrideAttrs shellOverride;
          formatter = pkgs.nixfmt;
        };
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
    };
}
