# Complete lexical sampler.
/* a closed block comment */
let
  value = if true && false then null else builtins.map (x: x + 1) [1 2.5];
  local = ./src/default.nix;
  parent = ../shared/lib.nix;
  absolute = /etc/nix/nix.conf;
  search = <nixpkgs/lib>;
  remote = https://example.org/archive.tar.gz;
in rec { inherit value; result = value // { ok = value != 0; }; }
