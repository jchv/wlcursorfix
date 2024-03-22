{
  description = "Patch for GTK3/GTK4 to make it use the cursor-shape-v1 protocol";
  inputs.flake-utils.url = "github:numtide/flake-utils";
  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system}; in
      {
        packages = rec {
          gtkcursorshape = pkgs.stdenv.mkDerivation {
            pname = "gtkcursorshape";
            version = "master";
            src = ./.;
            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
            ];
            buildInputs = with pkgs; [
              wayland
              wayland-protocols
              glib
            ];
          };
          default = gtkcursorshape;
        };
      }
    );
}
