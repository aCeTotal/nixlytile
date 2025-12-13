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

          buildTools = with pkgs; [
            gnumake
            stdenv.cc
            pkg-config
            wayland
            wayland-scanner
            wayland-protocols
            wlroots
            libinput
            libxkbcommon
            pixman
            libdrm
            seatd
            swaybg
          ];

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
          inherit pkgs wlrootsPc buildTools buildScript;
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

            nativeBuildInputs = with pkgs; [
              pkg-config
              wayland
              wayland-scanner
              wayland-protocols
            ];

            buildInputs = with pkgs; [
              wayland
              wlroots
              libinput
              libxkbcommon
              pixman
              libdrm
            ];

            makeFlags = [ "PKG_CONFIG=${pkgs.pkg-config}/bin/pkg-config" ];

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
