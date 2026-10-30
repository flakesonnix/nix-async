{
  description = "nix-async: plugin-only asynchronous native operations for Nix (Phase 0 probe)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/02c087f3fe673c9e317f206a627cb6b910336dbe";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      nix = pkgs.nixVersions.nix_2_34;
      plugin = pkgs.stdenv.mkDerivation {
        pname = "nix-async";
        version = "0.0.1";
        src = pkgs.lib.fileset.toSource {
          root = ./.;
          fileset = pkgs.lib.fileset.unions [
            ./CMakeLists.txt
            ./LICENSE
            ./src
            ./tests
            ./examples
          ];
        };
        nativeBuildInputs = [ pkgs.cmake pkgs.ninja pkgs.pkg-config nix ];
        buildInputs = [ nix.dev ];
        cmakeFlags = [ "-DNIX_EXECUTABLE=${nix}/bin/nix" ];
        doCheck = true;
        checkPhase = ''
          runHook preCheck
          ctest --output-on-failure
          runHook postCheck
        '';
        meta = {
          description = "Experimental native Nix plugin; Phase 0 loading probe";
          license = pkgs.lib.licenses.mit;
          platforms = [ system ];
        };
      };
    in
    {
      packages.${system}.default = plugin;
      checks.${system}.plugin = plugin;
      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ plugin ];
      };
    };
}
