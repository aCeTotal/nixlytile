{
      description = "Nix dev flake for hacking on dwl with a build helper";

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
            name = "dwl-build";
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
          defaultPackage = pkgs.stdenv.mkDerivation {
            pname = "dwl";
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
              mkdir -p $out/share/dwl/wallpapers
              cp -r wallpapers/* $out/share/dwl/wallpapers/
              wrapProgram $out/bin/dwl \
                --set DWL_WALLPAPER "$out/share/dwl/wallpapers/beach.jpg" \
                --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.swaybg pkgs.brightnessctl ]} \
                --prefix XDG_DATA_DIRS : "${pkgs.papirus-icon-theme}/share:${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share:${pkgs.shared-mime-info}/share"
              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "dwm-style Wayland compositor";
              homepage = "https://codeberg.org/dwl/dwl";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "dwl";
            };
          };
        in {
          default = defaultPackage;
          dwl = defaultPackage;
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
              echo "Helper: dwl-build (uses ${ps.wlrootsPc}); swaybg and seatd are available in PATH."
            '';
          };
        });

      apps = forAllSystems (system:
        let
          ps = perSystem system;
        in {
          build = {
            type = "app";
            program = "${ps.buildScript}/bin/dwl-build";
          };
        });
    };
}
