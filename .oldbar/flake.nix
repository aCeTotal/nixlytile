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

            # Virtual keyboard input
            wtype              # for on-screen keyboard

            # Browser for streaming services (NRK, F1TV)
            chromium
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
            # Video player dependencies
            pkgs.ffmpeg
            pkgs.pipewire
            pkgs.libass
            pkgs.libva
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
          serverPackage = pkgs.stdenv.mkDerivation {
            pname = "nixly-server";
            version = "git";
            src = ./Server;

            nativeBuildInputs = [
              pkgs.pkg-config
              pkgs.autoPatchelfHook
              pkgs.makeWrapper
            ];

            buildInputs = [
              pkgs.sqlite
              pkgs.ffmpeg
              pkgs.curl
              pkgs.cjson
            ];

            buildPhase = ''
              runHook preBuild
              make CC=${pkgs.stdenv.cc}/bin/cc
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp nixly-server $out/bin/
              wrapProgram $out/bin/nixly-server \
                --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.ffmpeg ]}
              runHook postInstall
            '';

            meta = with pkgs.lib; {
              description = "Nixly Media Server - lossless media streaming";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "nixly-server";
            };
          };
        in {
          default = defaultPackage;
          nixlytile = defaultPackage;
          server = serverPackage;
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
          pkgs = ps.pkgs;

          gethashScript = pkgs.writeShellApplication {
            name = "nixlytile-gethash";
            runtimeInputs = with pkgs; [ git nix coreutils gnused ];
            text = ''
              set -euo pipefail

              # Get repo root
              REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || echo ".")
              cd "$REPO_ROOT"

              # Get latest commit hash
              COMMIT=$(git rev-parse HEAD)
              SHORT_COMMIT=$(git rev-parse --short HEAD)

              # Get remote URL and extract owner/repo
              REMOTE=$(git remote get-url origin 2>/dev/null || echo "local")
              OWNER_REPO=$(echo "$REMOTE" | sed -E 's|.*[:/]([^/]+)/([^/]+)(\.git)?$|\1/\2|' | sed 's/\.git$//')

              echo "=== nixlytile gethash ==="
              echo "Commit: $COMMIT"
              echo "Short:  $SHORT_COMMIT"
              echo "Remote: $REMOTE"
              echo "Repo:   $OWNER_REPO"
              echo ""

              # Check if tree is dirty
              if ! git diff --quiet HEAD 2>/dev/null; then
                echo "⚠️  Warning: Working tree has uncommitted changes!"
                echo "   Hash will be for committed state only."
                echo ""
              fi

              # Compute hash using nix-prefetch-url with GitHub archive
              echo "Fetching hash from GitHub archive..."
              if [[ "$REMOTE" == *"github.com"* ]]; then
                ARCHIVE_URL="https://github.com/$OWNER_REPO/archive/$COMMIT.tar.gz"
                HASH=$(nix-prefetch-url --unpack "$ARCHIVE_URL" 2>/dev/null)
                SRI_HASH=$(nix hash convert --hash-algo sha256 --to sri "$HASH" 2>/dev/null || echo "$HASH")

                echo ""
                echo "=== For fetchFromGitHub ==="
                echo "owner = \"$(echo "$OWNER_REPO" | cut -d/ -f1)\";"
                echo "repo = \"$(echo "$OWNER_REPO" | cut -d/ -f2)\";"
                echo "rev = \"$COMMIT\";"
                echo "hash = \"$SRI_HASH\";"
              else
                echo "Not a GitHub remote, computing local hash..."
                HASH=$(nix hash path . 2>/dev/null || echo "failed")
                echo ""
                echo "=== Local hash ==="
                echo "hash = \"$HASH\";"
              fi

              echo ""
              echo "=== Quick copy ==="
              echo "$SHORT_COMMIT"
            '';
          };
          testScript = pkgs.writeShellApplication {
            name = "nixlytile-test";
            runtimeInputs = [ self.packages.${system}.server self.packages.${system}.nixlytile ];
            text = ''
              cleanup() {
                echo "Stopping server..."
                kill "$SERVER_PID" 2>/dev/null || true
              }
              trap cleanup EXIT

              echo "Starting nixly-server..."
              nixly-server -p 8080 &
              SERVER_PID=$!
              sleep 1

              echo "Starting nixlytile..."
              nixlytile "$@"
            '';
          };
        in {
          build = {
            type = "app";
            program = "${ps.buildScript}/bin/nixlytile-build";
          };
          gethash = {
            type = "app";
            program = "${gethashScript}/bin/nixlytile-gethash";
          };
          test = {
            type = "app";
            program = "${testScript}/bin/nixlytile-test";
          };
        });
    };
}
