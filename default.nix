{ pkgs ? import <nixpkgs> { } }:

let
  inherit (pkgs) lib;
in
pkgs.stdenv.mkDerivation {
  pname = "serpantinum-audio";
  version = lib.strings.trim (builtins.readFile ./VERSION);

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./Makefile
      ./VERSION
      ./LICENSE
      ./src
      ./test
    ];
  };

  strictDeps = true;
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ pkgs.pipewire ];

  doCheck = true;
  checkTarget = "test";

  makeFlags = [ "PREFIX=$(out)" ];

  meta = {
    description = "PipeWire audio processor for the Serpantinum shell";
    homepage = "https://github.com/ilyamiro/serpantinum-audio";
    license = lib.licenses.agpl3Only;
    mainProgram = "serpantinum-audio";
    platforms = lib.platforms.linux;
  };
}
