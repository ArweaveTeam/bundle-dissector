{ pkgs, lib, callPackage, buildGoModule }:

let builder = callPackage ./builder {};

in buildGoModule rec {
  pname = "bundle-dissector";
  version = "0.0.0";
  vendorSha256 = null;
  src = ./.;
  modules = ./gomod2nix.toml;
}
