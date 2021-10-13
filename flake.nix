{
  description = "golang based tool to analyse and unbundle ANS104";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    pre-commit-hooks.url = "github:cachix/pre-commit-hooks.nix";
    pre-commit-hooks.inputs.nixpkgs.follows = "nixpkgs";
    gomod2nix.url = "github:tweag/gomod2nix";
  };

  outputs = { self, nixpkgs, flake-utils, pre-commit-hooks, gomod2nix }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        overlays = [ gomod2nix.overlay ];

        pkgs = (import nixpkgs {
          inherit overlays system;
        });
      in
        {

          defaultPackage = pkgs.callPackage ./default.nix {};

          devShell = pkgs.mkShell {
            name = "bundle-dissector-development";
            packages = with pkgs; [
              go
              pkgs.gomod2nix
            ];

            shellHook = ''
              ${pkgs.fish}/bin/fish --interactive -C \
                '${pkgs.any-nix-shell}/bin/any-nix-shell fish --info-right | source'
              exit $?
            '';
          };
        });


}
