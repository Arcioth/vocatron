# Dev shell for vocatron. Enter it with: nix develop
# Pins the compiler and libraries so the build does not depend on
# whatever happens to be installed on the NixOS system.
{
  description = "vocatron — receive-only HackRF voice receiver";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in {
      devShells.${system}.default = pkgs.mkShell {
        # Tools. These land on PATH.
        packages = with pkgs; [
          cmake
          ninja
          pkg-config
          clang-tools # clang-format, clang-tidy
        ];

        # Libraries. pkg-config finds these from buildInputs.
        # libhackrf is used now. liquid-dsp and fftwFloat are the DSP
        # libraries the later milestones link; they are here so the
        # shell does not have to grow a new input at each step.
        buildInputs = with pkgs; [
          hackrf
          liquid-dsp
          fftwFloat
        ];
      };
    };
}
