{
  description = "Nixlytile - a tiling Wayland compositor";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  inputs.nixlypkgs.url = "github:aCeTotal/nixlypkgs";
  inputs.nixlypkgs.inputs.nixpkgs.follows = "nixpkgs";

  outputs = { self, nixpkgs, nixlypkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      lib = nixpkgs.lib;
      forAllSystems = f: lib.genAttrs systems f;

      perSystem = system:
        let
          pkgs = import nixpkgs {
            inherit system;
            overlays = [
              nixlypkgs.overlays.default
            ];
          };

          wlrootsLocal = pkgs.stdenv.mkDerivation {
            pname = "wlroots";
            version = "0.20.0-rc4";
            src = ./wlroots;
            nativeBuildInputs = with pkgs; [ meson ninja pkg-config wayland-scanner glslang ];
            buildInputs = with pkgs; [
              wayland wayland-protocols libdrm libxkbcommon pixman libinput
              xwayland seatd libxcb libxcb-wm
              libgbm hwdata libliftoff libdisplay-info lcms2 libxcb-errors
              vulkan-loader vulkan-headers
            ];
            mesonFlags = [
              "-Dexamples=false"
              "-Dxwayland=enabled"
              "-Dbackends=drm,libinput"
              "-Drenderers=vulkan"
              "-Dallocators=gbm"
            ];
          };

          wlrootsPc = "wlroots-0.20";

          buildTools = with pkgs; [
            gnumake
            stdenv.cc
            pkg-config
            fcft
            wayland
            wayland-scanner
            wayland-protocols
            wlrootsLocal
            libinput
            libxkbcommon
            tllist
            pixman
            libdrm
            systemd
            libxcb
            libxcb-wm
            xwayland
            seatd
            cairo
            librsvg
            gdk-pixbuf
            glib
          ];

          buildScript = pkgs.writeShellApplication {
            name = "nixlytile-build";
            runtimeInputs = buildTools;
            text = ''
              set -euo pipefail
              echo "Using wlroots pkg-config name: ${wlrootsPc} (${wlrootsLocal.version})"
              make clean
              make \
                PKG_CONFIG=pkg-config \
                WLR_INCS="$(pkg-config --cflags ${wlrootsPc})" \
                WLR_LIBS="$(pkg-config --libs ${wlrootsPc})" \
                "$@"
            '';
          };
        in {
          inherit pkgs wlrootsPc wlrootsLocal buildTools buildScript;
        };
    in {
      packages = forAllSystems (system:
        let
          ps = perSystem system;
          pkgs = ps.pkgs;
          runtimeDeps = with pkgs; [
            xwayland
            foot
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
              pkgs.addDriverRunpath
            ];

            buildInputs = [
              pkgs.wayland
              ps.wlrootsLocal
              pkgs.libinput
              pkgs.libxkbcommon
              pkgs.fcft
              pkgs.tllist
              pkgs.pixman
              pkgs.libdrm
              pkgs.libxcb
              pkgs.libxcb-wm
              pkgs.xwayland
              pkgs.systemd
              pkgs.libglvnd
              pkgs.cairo
              pkgs.librsvg
              pkgs.gdk-pixbuf
              pkgs.glib
            ];

            makeFlags = [ "PKG_CONFIG=${pkgs.pkg-config}/bin/pkg-config" ];
            dontWrapQtApps = true;

            buildPhase = ''
              runHook preBuild
              make clean
              make \
                WLR_INCS="$(${pkgs.pkg-config}/bin/pkg-config --cflags ${ps.wlrootsPc})" \
                WLR_LIBS="$(${pkgs.pkg-config}/bin/pkg-config --libs ${ps.wlrootsPc})"
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              make PREFIX=$out MANDIR=$out/share/man DATADIR=$out/share install
              wrapProgram $out/bin/nixlytile \
                --set WLR_RENDERER vulkan \
                --prefix PATH : ${pkgs.lib.makeBinPath runtimeDeps}
              runHook postInstall
            '';

            postFixup = ''
              addDriverRunpath $out/bin/nixlytile
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
              echo "nixlytile-build available (uses ${ps.wlrootsPc})"
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
