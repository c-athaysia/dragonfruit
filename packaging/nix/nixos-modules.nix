self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.dragonfruit;
in
{
  options = {
    programs.dragonfruit = {
      enable = lib.mkEnableOption "Dragonfruit, an infinite-canvas Wayland compositor forked from Mango";
      addLoginEntry = lib.mkOption {
        type = lib.types.bool;
        default = true;
        description = "Whether to add a login entry to the display manager for dragonfruit. Only has effect if a display manager is configured (e.g. SDDM, GDM via `services.displayManager`).";
      };
      package = lib.mkOption {
        type = lib.types.package;
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.dragonfruit;
        description = "The dragonfruit package to use";
      };
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      cfg.package
    ];

    xdg.portal = {
      enable = lib.mkDefault true;

      config = {
        dragonfruit = {
          default = [
            "gtk"
          ];
          # except those
          "org.freedesktop.impl.portal.Secret" = [ "gnome-keyring" ];
          "org.freedesktop.impl.portal.ScreenCast" = [ "wlr" ];
          "org.freedesktop.impl.portal.Screenshot" = [ "wlr" ];

          # wlr does not have this interface
          "org.freedesktop.impl.portal.Inhibit" = [ ];
        };
      };
      extraPortals = with pkgs; [
        xdg-desktop-portal-wlr
        xdg-desktop-portal-gtk
      ];

      wlr.enable = lib.mkDefault true;

      configPackages = [ cfg.package ];
    };

    security.polkit.enable = lib.mkDefault true;

    programs.xwayland.enable = lib.mkDefault true;

    services = {
      displayManager.sessionPackages = lib.mkIf cfg.addLoginEntry [ cfg.package ];

      graphical-desktop.enable = lib.mkDefault true;
    };
  };
}
