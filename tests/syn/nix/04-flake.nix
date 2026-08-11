{
  description = "small detection-shaped flake";
  outputs = { self }: { packages.x86_64-linux.default = import ./package.nix; };
}
