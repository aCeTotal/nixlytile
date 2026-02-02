{
  description = "Nixly Media Server - Lossless streaming server for movies and TV shows";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        devShells.default = pkgs.mkShell {
          name = "nixly-media-server";

          buildInputs = with pkgs; [
            # FFmpeg for media probing (not transcoding - we serve lossless)
            ffmpeg-headless

            # Database - SQLite for fast local database
            sqlite

            # C development
            gcc
            gnumake
            pkg-config

            # Networking and HTTP
            openssl
            curl

            # JSON parsing for TMDB responses
            cjson

            # Development tools
            gdb
            valgrind
          ];

          shellHook = ''
            echo "Nixly Media Server Development Environment"
            echo "==========================================="
            echo "FFmpeg:  $(ffmpeg -version | head -n1)"
            echo "SQLite:  $(sqlite3 --version)"
            echo ""
            echo "Build:   make"
            echo "Run:     ./nixly-server"
          '';
        };
      }
    );
}
