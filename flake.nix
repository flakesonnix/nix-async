{
  description = "nix-async: independent native async runtime and experimental Nix plugin";

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
            ./include
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
          description = "Native async runtime and experimental Nix plugin loading probe";
          license = pkgs.lib.licenses.mit;
          platforms = [ system ];
        };
      };
      runtime = plugin.overrideAttrs {
        pname = "nix-async-runtime";
        nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
        buildInputs = [ ];
        cmakeFlags = [ "-DNIX_ASYNC_BUILD_PLUGIN=OFF" ];
      };
    in
    {
      packages.${system}.default = plugin;
      checks.${system} = { inherit plugin runtime; };
      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ plugin ];
      };
    };
}
