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

          buildTools = with pkgs; [
            gnumake
            stdenv.cc
            pkg-config
            fcft
            wayland
            wayland-scanner
            wayland-protocols
            glslang
            hwdata
            libinput
            libxkbcommon
            tllist
            pixman
            libdrm
            systemd
            libxcb
            libxcb-wm
            libxcb-errors
            xwayland
            seatd
            libgbm
            libdisplay-info
            vulkan-loader
            cairo
            librsvg
            gdk-pixbuf
            glib
            vulkan-headers
            qrencode
            quirc
            libjpeg_turbo
          ];

          gazeModel = name: hash: pkgs.fetchurl {
            url = "https://github.com/google/mediapipe/raw/v0.8.9/mediapipe/modules/${name}";
            inherit hash;
          };

          gazeModels = pkgs.linkFarm "nixlytile-gaze-models" [
            {
              name = "face_detection_short_range.tflite";
              path = gazeModel "face_detection/face_detection_short_range.tflite"
                "sha256-O8GC658zkl2eWLXI1ZMIp2D0reqPKCNw5CjFEhLCZjM=";
            }
            {
              name = "face_landmark.tflite";
              path = gazeModel "face_landmark/face_landmark.tflite"
                "sha256-xgP6YUkhmj6Uh9yavXoMJEdMdyYyc9JIaDeM30CqJtE=";
            }
            {
              name = "iris_landmark.tflite";
              path = gazeModel "iris_landmark/iris_landmark.tflite"
                "sha256-0XRNKgnCX1AdOeuk+v9H5T7MqIUsXOGbzo7qw5NXUh8=";
            }
          ];

          buildScript = pkgs.writeShellApplication {
            name = "nixlytile-build";
            runtimeInputs = buildTools;
            text = ''
              set -euo pipefail
              make clean
              make PKG_CONFIG=pkg-config GAZE_MODEL_DIR=${gazeModels} "$@"
            '';
          };
        in {
          inherit pkgs buildTools buildScript gazeModels;
        };
    in {
      packages = forAllSystems (system:
        let
          ps = perSystem system;
          pkgs = ps.pkgs;
          runtimeDeps = with pkgs; [
            matugen
            xwayland
            foot
            wl-clipboard
            grim
            slurp
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
              pkgs.glslang
              pkgs.makeWrapper
              pkgs.addDriverRunpath
            ];

            buildInputs = [
              pkgs.wayland
              pkgs.libinput
              pkgs.libxkbcommon
              pkgs.fcft
              pkgs.tllist
              pkgs.pixman
              pkgs.libdrm
              pkgs.libxcb
              pkgs.libxcb-wm
              pkgs.libxcb-errors
              pkgs.xwayland
              pkgs.seatd
              pkgs.libgbm
              pkgs.libdisplay-info
              pkgs.hwdata
              pkgs.vulkan-loader
              pkgs.systemd
              pkgs.libglvnd
              pkgs.cairo
              pkgs.librsvg
              pkgs.gdk-pixbuf
              pkgs.glib
              pkgs.vulkan-headers
              pkgs.qrencode
              pkgs.quirc
              pkgs.libjpeg_turbo
            ];

            makeFlags = [ "PKG_CONFIG=${pkgs.pkg-config}/bin/pkg-config" ];
            dontWrapQtApps = true;

            buildPhase = ''
              runHook preBuild
              make clean
              make -j$NIX_BUILD_CORES \
                ICON_FONT_DIR=${pkgs.nerd-fonts.symbols-only}/share/fonts \
                GAZE_MODEL_DIR=${ps.gazeModels}
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              make PREFIX=$out MANDIR=$out/share/man DATADIR=$out/share install
              wrapProgram $out/bin/nixlytile \
                --set NIXLY_RENDERER vulkan \
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
              echo "nixlytile-build available"
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
          prewarm = pkgs.writeShellApplication {
            name = "nixly-prewarm";
            runtimeInputs = with pkgs; [ coreutils findutils gawk gnugrep inotify-tools procps util-linux vulkan-tools ];
            text = builtins.readFile ./scripts/nixly-prewarm;
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

            fanControl = {
              enable = lib.mkOption {
                type = lib.types.bool;
                default = true;
                description = ''
                  Fan control for the statusbar Fans popup: the
                  nixly-fand root helper (MSI EC fan tables, root-owned
                  hwmon pwm files, NVML fan control on desktop NVIDIA),
                  ec_sys with write support for the EC tables, and
                  relaxed permissions on the common fan sysfs knobs so
                  the compositor can write them directly.
                '';
              };
            };

            diskControl = {
              enable = lib.mkOption {
                type = lib.types.bool;
                default = true;
                description = ''
                  Disk management for the statusbar Storage popup: the
                  nixly-diskd root helper (partitioning via parted,
                  mkfs for ext4/btrfs/xfs/vfat/exfat/ntfs, mount).
                  Mutating commands are refused on the disks holding
                  the system mounts.
                '';
              };
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

            prewarm = {
              enable = lib.mkOption {
                type = lib.types.bool;
                default = true;
                description = ''
                  Background shader pre-processing for installed Steam
                  games: a user timer replays every game's downloaded
                  Vulkan pipeline caches into the per-game GPU driver
                  caches at idle priority, so first launches don't stall
                  on shader compilation.  The compositor additionally
                  runs `nixly-prewarm readahead` at the moment Play is
                  pressed to pull the game's working set into RAM.
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

            (lib.mkIf cfg.prewarm.enable {
              # steam-run gives Steam's fossilize_replay the FHS env it
              # needs; the script itself must be on PATH for the
              # compositor's launch-time readahead.
              environment.systemPackages = [ prewarm pkgs.steam-run ];

              systemd.user.services.nixly-prewarm = {
                description = "nixlytile Steam shader prewarm";
                serviceConfig = {
                  Type = "oneshot";
                  ExecStart = "${prewarm}/bin/nixly-prewarm run";
                  Nice = 19;
                  IOSchedulingClass = "idle";
                  CPUSchedulingPolicy = "batch";
                };
                # fossilize_replay via steam-run + vendor Vulkan ICDs
                path = [ "/run/current-system/sw" ];
              };

              systemd.user.timers.nixly-prewarm = {
                description = "nixlytile Steam shader prewarm timer";
                wantedBy = [ "timers.target" ];
                timerConfig = {
                  # Soon after login, then periodically — new/updated
                  # pipeline caches downloaded by Steam get processed
                  # without waiting for Steam's own idle heuristics.
                  OnStartupSec = "5min";
                  OnUnitInactiveSec = "6h";
                };
              };

              # Install watcher: inotify on every Steam library's
              # shadercache — the moment a new/updated game's pipeline
              # caches land, they are replayed into the driver caches.
              # The timer above stays as a backstop for missed events.
              systemd.user.services.nixly-prewarm-watch = {
                description = "nixlytile Steam shader prewarm install watcher";
                wantedBy = [ "default.target" ];
                serviceConfig = {
                  ExecStart = "${prewarm}/bin/nixly-prewarm watch";
                  Restart = "on-failure";
                  RestartSec = 30;
                  Nice = 19;
                  IOSchedulingClass = "idle";
                  CPUSchedulingPolicy = "batch";
                };
                path = [ "/run/current-system/sw" ];
              };
            })

            (lib.mkIf cfg.fanControl.enable {
              # Raw EC access for the MSI fan tables. nixly-fand only
              # accepts writes to the fan-table byte ranges, so the
              # dangerous surface of write_support stays behind it.
              boot.kernelModules = [ "ec_sys" ];
              boot.extraModprobeConfig = ''
                options ec_sys write_support=1
              '';

              systemd.services.nixly-fand = {
                description = "nixlytile fan control helper";
                wantedBy = [ "multi-user.target" ];
                after = [ "systemd-modules-load.service" ];
                serviceConfig = {
                  Type = "exec";
                  ExecStart = "${cfg.package}/bin/nixly-fand";
                  Restart = "on-failure";
                  RestartSec = 5;
                };
              };

              # Direct unprivileged writes for the common knobs — the
              # compositor falls back to the helper socket for anything
              # still root-owned.
              services.udev.extraRules = ''
                SUBSYSTEM=="hwmon", ACTION=="add", RUN+="${pkgs.runtimeShell} -c 'chmod 666 /sys%p/pwm* 2>/dev/null || true'"
              '';
              systemd.tmpfiles.rules = [
                "z /sys/devices/platform/msi-ec/fan_mode 0666 - - -"
                "z /sys/devices/platform/msi-ec/cooler_boost 0666 - - -"
              ];
            })

            (lib.mkIf cfg.diskControl.enable {
              systemd.services.nixly-diskd = {
                description = "nixlytile disk management helper";
                wantedBy = [ "multi-user.target" ];
                path = with pkgs; [
                  parted
                  util-linux    # wipefs, mount, umount
                  e2fsprogs
                  btrfs-progs
                  xfsprogs
                  dosfstools
                  exfatprogs
                  ntfs3g
                  systemd       # udevadm settle
                ];
                serviceConfig = {
                  Type = "exec";
                  ExecStart = "${cfg.package}/bin/nixly-diskd";
                  Restart = "on-failure";
                  RestartSec = 5;
                };
              };
            })

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

              # GPU power limit at hardware max from boot. An idle GPU still
              # clocks down on its own, so the raised ceiling costs nothing
              # until a load arrives — it only removes the cap under load.
              # Clock locks stay game-mode-only (nixly-gametune): locking
              # clocks 24/7 would burn power at idle.
              systemd.services.nixly-gpumax = {
                description = "nixlytile GPU power limit at hardware max";
                wantedBy = [ "multi-user.target" ];
                after = [ "systemd-modules-load.service" ];
                path = [ "/run/current-system/sw" ];
                serviceConfig = {
                  Type = "oneshot";
                  RemainAfterExit = true;
                  ExecStart = "${gametune}/bin/nixly-gametune gpumax";
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
