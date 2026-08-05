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
            version = "0.20.2";
            src = ./wlroots;
            nativeBuildInputs = with pkgs; [ meson ninja pkg-config wayland-scanner glslang ];
            buildInputs = with pkgs; [
              wayland wayland-protocols libdrm libxkbcommon pixman libinput
              xwayland seatd libxcb libxcb-wm
              libgbm hwdata libliftoff libdisplay-info lcms2 libxcb-errors
              vulkan-loader vulkan-headers
            ];
            mesonFlags = [
              # nixpkgs' meson hook defaults to --buildtype=plain, which passes
              # no -O flag at all -> wlroots was built at -O0.
              "--buildtype=release"
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

      # Privileged half of game mode.
      #
      # The compositor is an unprivileged user process, so the tuning that
      # needs root — C-state latency, CPU governor/EPP, GPU power state,
      # kernel VM knobs — cannot be done by it. This module installs the
      # helper unit it starts, the polkit rule that lets it do so, and the
      # resource limits it needs for its own scheduling boosts.
      #
      # Usage in configuration.nix:
      #   imports = [ nixlytile.nixosModules.default ];
      #   programs.nixlytile.enable = true;
      #   users.users.<you>.extraGroups = [ "gamemode" ];
      nixosModules.default = { config, lib, pkgs, ... }:
        let
          cfg = config.programs.nixlytile;
          gametune = pkgs.writeShellApplication {
            name = "nixly-gametune";
            runtimeInputs = with pkgs; [ coreutils gawk gnugrep procps systemd util-linux ];
            text = builtins.readFile ./scripts/nixly-gametune;
          };
        in {
          options.programs.nixlytile = {
            enable = lib.mkEnableOption "nixlytile game mode support";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.system}.default;
              defaultText = "nixlytile.packages.\${system}.default";
              description = "The nixlytile package to install.";
            };

            gameMode = {
              enable = lib.mkOption {
                type = lib.types.bool;
                default = true;
                description = ''
                  Install nixly-gametune.service. Without it the compositor
                  logs an error on every game start and all privileged tuning
                  is skipped.
                '';
              };

              group = lib.mkOption {
                type = lib.types.str;
                default = "gamemode";
                description = ''
                  Group allowed to start and stop the tuning unit. Add your
                  user to it, otherwise polkit denies the request.
                '';
              };
            };

            resourceLimits = lib.mkOption {
              type = lib.types.bool;
              default = true;
              description = ''
                Raise RLIMIT_NICE and RLIMIT_RTPRIO for the game-mode group.
                Without this the compositor cannot renice a game below 0
                (setpriority returns EPERM) and cannot take real-time
                priority for its own frame loop.
              '';
            };
          };

          config = lib.mkIf cfg.enable (lib.mkMerge [
            {
              environment.systemPackages = [ cfg.package ];
            }

            (lib.mkIf cfg.gameMode.enable {
              users.groups.${cfg.gameMode.group} = { };

              # NT synchronisation primitives in the kernel (6.14+). Wine 11
              # and Proton 11 use /dev/ntsync automatically when it exists,
              # replacing the esync/fsync emulation and its CPU overhead.
              # No-op when the kernel has it built in.
              boot.kernelModules = [ "ntsync" ];

              systemd.services.nixly-gametune = {
                description = "nixlytile game mode tuning";
                # Never started at boot: the compositor starts it when a
                # game goes fullscreen and stops it when the game exits.
                #
                # The system profile is on PATH so vendor tools land in
                # scope: nvidia-smi lives in /run/current-system/sw/bin and
                # is the only way to lock clocks and raise the power limit
                # on NVIDIA. Harmless on AMD and Intel machines.
                path = [ "/run/current-system/sw" ];
                serviceConfig = {
                  Type = "exec";
                  ExecStart = "${gametune}/bin/nixly-gametune daemon";
                  # The daemon restores everything from its TERM trap; this
                  # is the belt-and-braces path for a killed daemon.
                  ExecStopPost = "${gametune}/bin/nixly-gametune restore";
                  RuntimeDirectory = "nixly-gametune";
                  RuntimeDirectoryPreserve = "yes";
                  # Tuning must survive the game, not the unit's own start.
                  TimeoutStopSec = "15s";
                  Restart = "no";
                };
              };

              # Per-game OOM protection. Lowering oom_score_adj needs
              # CAP_SYS_RESOURCE, so the compositor cannot do it itself; the
              # game PID is the instance name.
              systemd.services."nixly-gameprio@" = {
                description = "nixlytile OOM protection for game PID %i";
                serviceConfig = {
                  Type = "oneshot";
                  RemainAfterExit = true;
                  ExecStart = "${gametune}/bin/nixly-gametune protect %i";
                  ExecStop = "${gametune}/bin/nixly-gametune unprotect %i";
                  # Same directory as the tuning unit: that is where the
                  # saved oom_score_adj values live.
                  RuntimeDirectory = "nixly-gametune";
                  RuntimeDirectoryPreserve = "yes";
                };
              };

              # Let the game-mode group control those units, nothing else.
              security.polkit.extraConfig = ''
                polkit.addRule(function(action, subject) {
                  if (action.id == "org.freedesktop.systemd1.manage-units" &&
                      subject.isInGroup("${cfg.gameMode.group}")) {
                    var unit = action.lookup("unit");
                    if (unit && (unit == "nixly-gametune.service" ||
                        unit.indexOf("nixly-gameprio@") == 0)) {
                      return polkit.Result.YES;
                    }
                  }
                });
              '';
            })

            (lib.mkIf cfg.resourceLimits {
              security.pam.loginLimits = [
                # Negative nice for the game process. RLIMIT_NICE is
                # expressed as 20 - min_nice, so 30 allows nice -10.
                { domain = "@${cfg.gameMode.group}"; type = "-"; item = "nice"; value = "-10"; }
                # Real-time priority for the compositor's frame loop.
                { domain = "@${cfg.gameMode.group}"; type = "-"; item = "rtprio"; value = "95"; }
                # Let the compositor and the game lock buffers in memory.
                { domain = "@${cfg.gameMode.group}"; type = "-"; item = "memlock"; value = "unlimited"; }
              ];
            })
          ]);
        };

      nixosModules.nixlytile = self.nixosModules.default;
    };
}
