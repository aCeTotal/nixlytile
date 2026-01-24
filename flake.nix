{
      description = "Nixlytile - a tiling Wayland compositor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      lib = nixpkgs.lib;
      forAllSystems = f: lib.genAttrs systems f;

      perSystem = system:
        let
          pkgs = import nixpkgs { inherit system; };
          wlrootsPc = "wlroots-${lib.versions.majorMinor pkgs.wlroots.version}";

          iconDeps = with pkgs; [
            cairo
            librsvg
            gdk-pixbuf
            hicolor-icon-theme
            adwaita-icon-theme
            papirus-icon-theme
            libpng
            libjpeg
          ];

          buildTools = with pkgs;
            [
              gnumake
              stdenv.cc
              fcft
              wayland
              wayland-protocols
              wlroots
              libinput
              libxkbcommon
              tllist
              pixman
              libdrm
              systemd
              xorg.libxcb
              xorg.xcbutilwm
              xwayland
              seatd
              swaybg
            ]
            ++ iconDeps;

          buildScript = pkgs.writeShellApplication {
            name = "nixlytile-build";
            runtimeInputs = buildTools;
            text = ''
              set -euo pipefail
              echo "Using wlroots pkg-config name: ${wlrootsPc} (${pkgs.wlroots.version})"
              make clean
              make \
                PKG_CONFIG=pkg-config \
                WLR_INCS="$(pkg-config --cflags ${wlrootsPc})" \
                WLR_LIBS="$(pkg-config --libs ${wlrootsPc})" \
                "$@"
            '';
          };
        in {
          inherit pkgs wlrootsPc buildTools buildScript iconDeps;
        };
    in {
      packages = forAllSystems (system:
        let
          ps = perSystem system;
          pkgs = ps.pkgs;
          runtimeDeps = with pkgs; [
            # Core utilities
            swaybg
            brightnessctl
            xdg-utils          # xdg-open

            # File manager
            xfce.thunar
            xfce.thunar-volman
            xfce.thunar-archive-plugin

            # Network management
            networkmanager     # nmcli
            networkmanagerapplet

            # Audio
            pipewire
            wireplumber
            pavucontrol

            # Bluetooth
            blueman

            # System utilities
            findutils          # find (for file search)
            coreutils          # rm, mkdir, etc.
            gnused
            gnugrep

            # Terminal
            alacritty
            foot

            # Notifications
            libnotify          # notify-send
          ];
          defaultPackage = pkgs.stdenv.mkDerivation {
            pname = "nixlytile";
            version = "git";
            src = ./.;

            nativeBuildInputs = [
              pkgs.pkg-config
              pkgs.wayland
              pkgs.wayland-scanner
              pkgs.wayland-protocols
              pkgs.makeWrapper
            ];

          buildInputs = [
            pkgs.wayland
            pkgs.wlroots
            pkgs.libinput
            pkgs.libxkbcommon
            pkgs.fcft
              pkgs.tllist
              pkgs.pixman
              pkgs.libdrm
            pkgs.xorg.libxcb
            pkgs.xorg.xcbutilwm
            pkgs.xwayland
            pkgs.systemd
            pkgs.brightnessctl
          ] ++ ps.iconDeps;

            makeFlags = [ "PKG_CONFIG=${pkgs.pkg-config}/bin/pkg-config" ];
            dontWrapQtApps = true;

            buildPhase = ''
              runHook preBuild
              make \
                WLR_INCS="$(${pkgs.pkg-config}/bin/pkg-config --cflags ${ps.wlrootsPc})" \
                WLR_LIBS="$(${pkgs.pkg-config}/bin/pkg-config --libs ${ps.wlrootsPc})"
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              make PREFIX=$out MANDIR=$out/share/man DATADIR=$out/share install
              wrapProgram $out/bin/nixlytile \
                --prefix PATH : ${pkgs.lib.makeBinPath runtimeDeps} \
                --prefix XDG_DATA_DIRS : "${pkgs.papirus-icon-theme}/share:${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share:${pkgs.shared-mime-info}/share"
              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "Nixlytile - a tiling Wayland compositor";
              homepage = "https://github.com/total/nixlytile";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "nixlytile";
            };
          };
        in {
          default = defaultPackage;
          nixlytile = defaultPackage;
        });

      devShells = forAllSystems (system:
        let
          ps = perSystem system;
          pkgs = ps.pkgs;
        in {
          default = pkgs.mkShell {
            packages = ps.buildTools ++ [ ps.buildScript ];
            shellHook = ''
              export WLR_PKG=${ps.wlrootsPc}
              echo "Helper: nixlytile-build (uses ${ps.wlrootsPc}); swaybg and seatd are available in PATH."
            '';
          };
        });

      apps = forAllSystems (system:
        let
          ps = perSystem system;
        in {
          build = {
            type = "app";
            program = "${ps.buildScript}/bin/nixlytile-build";
          };
        });
    };
}
