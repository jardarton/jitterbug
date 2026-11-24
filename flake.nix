{
  description = "A C library that renders like the PlayStation GPU";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  # Sources the conformance recorder reads (tools/conformance-record,
  # pins.sh, programs.txt): fetched, never vendored.
  #   ps1-tests: the pin, for the console's dumps and logs, ps1-tests'
  #     licence and the GTE cases (MIT)
  #   ps1-tests-REV: the commit each program is recorded from, the one its
  #     dump or log was committed with (programs.txt); every input named
  #     ps1-tests-* is passed to the recorder
  #   psn00bsdk: the SDK version ps1-tests is built with, for its debug
  #     font and licence (MPL-2.0)
  inputs.ps1-tests = {
    url = "github:JaCzekanski/ps1-tests/f727802fead11f1daa7549285548392ef87749cb";
    flake = false;
  };
  # triangle
  inputs.ps1-tests-78ab568 = {
    url = "github:JaCzekanski/ps1-tests/78ab568408bacfad61eab6a487235be2d0a8c149";
    flake = false;
  };
  # quad
  inputs.ps1-tests-603a536 = {
    url = "github:JaCzekanski/ps1-tests/603a53630e20d787b1dc2380b34fa7a391189d6e";
    flake = false;
  };
  # lines
  inputs.ps1-tests-d1a63ee = {
    url = "github:JaCzekanski/ps1-tests/d1a63eec6a0cbcefa6bb22c2500907e6281f6eee";
    flake = false;
  };
  # rectangles
  inputs.ps1-tests-86a6f3c = {
    url = "github:JaCzekanski/ps1-tests/86a6f3c4b107277d3a1e9f094e4849d7593b68df";
    flake = false;
  };
  # transparency
  inputs.ps1-tests-da6b472 = {
    url = "github:JaCzekanski/ps1-tests/da6b472d023df0ce4dd2c3a71b5c1d7a1bef50b7";
    flake = false;
  };
  # uv-interpolation
  inputs.ps1-tests-eb10e45 = {
    url = "github:JaCzekanski/ps1-tests/eb10e4577ad58d87e1ca4f51d19cb7eb1926bf44";
    flake = false;
  };
  # clipping
  inputs.ps1-tests-5031fcb = {
    url = "github:JaCzekanski/ps1-tests/5031fcb40747075b43f2c6253dc87be33df09ad6";
    flake = false;
  };
  # texture-flip
  inputs.ps1-tests-7adb9f0 = {
    url = "github:JaCzekanski/ps1-tests/7adb9f019ff4da2a32959f0e0ecd31fc66a223ed";
    flake = false;
  };
  # texture-overflow
  inputs.ps1-tests-b0a94a8 = {
    url = "github:JaCzekanski/ps1-tests/b0a94a82a138a215617527496385ed1b5ffc4ab5";
    flake = false;
  };
  # clut-cache
  inputs.ps1-tests-5f73d61 = {
    url = "github:JaCzekanski/ps1-tests/5f73d61b27a8d51269b54dde1c456d2d2f7dc865";
    flake = false;
  };
  # vram-to-vram-overlap
  inputs.ps1-tests-93568c5 = {
    url = "github:JaCzekanski/ps1-tests/93568c5589e58b1fe442c854728324994bd30426";
    flake = false;
  };
  # mask-bit
  inputs.ps1-tests-79dfb13 = {
    url = "github:JaCzekanski/ps1-tests/79dfb131b5c3694a75cbecb64a7b8f2ef29a9ef9";
    flake = false;
  };
  # mask-bit, its words compared
  inputs.ps1-tests-d190785 = {
    url = "github:JaCzekanski/ps1-tests/d190785c9d3c6eba8c956b926ab6da4c2f14fd88";
    flake = false;
  };
  # gp0-e1
  inputs.ps1-tests-42352ae = {
    url = "github:JaCzekanski/ps1-tests/42352ae717a55a50bc1cd581946fab8ff95b0441";
    flake = false;
  };
  inputs.psn00bsdk = {
    url = "github:Lameguy64/PSn00bSDK/b0659ad85b7aa6e74d2c3eac29281636a0c2bc5e";
    flake = false;
  };
  # The reference for differential fuzzing (tools/fuzz-diff, pin.sh): its
  # psyq/gpu.c is compiled at build time, never vendored (MIT).
  inputs.psxstack = {
    url = "github:gascarcella/psxstack/40071978ec43f76757a5e7fc5c89ee680bccd3bb";
    flake = false;
  };

  outputs =
    inputs@{
      self,
      nixpkgs,
      ps1-tests,
      psn00bsdk,
      psxstack,
      ...
    }:
    let
      inherit (nixpkgs) lib;
      # The ps1-tests commits the programs are recorded from, as REV=DIR
      # for record.sh.
      programSources = lib.mapAttrsToList (_: i: "${i.rev}=${i}") (
        lib.filterAttrs (n: _: lib.hasPrefix "ps1-tests-" n) inputs
      );
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: lib.genAttrs supportedSystems (system: f nixpkgs.legacyPackages.${system});

      # The version lives in the public header.
      version =
        let
          header = builtins.readFile ./include/jitterbug.h;
          part = name: builtins.head (builtins.match ".*#define JB_VERSION_${name} ([0-9]+).*" header);
        in
        "${part "MAJOR"}.${part "MINOR"}.${part "PATCH"}";

      # The sources the builds see (only git-tracked files reach the flake).
      src = lib.fileset.toSource {
        root = ./.;
        fileset = lib.fileset.unions [
          ./CMakeLists.txt
          ./LICENSE
          ./cmake
          ./docs/images
          ./examples
          ./include
          ./src
          ./tests
          ./third_party
          ./tools
        ];
      };

      # Mesa's software Vulkan driver, for tests without a GPU.
      lavapipeIcd =
        pkgs: "${pkgs.mesa}/share/vulkan/icd.d/lvp_icd.${pkgs.stdenv.hostPlatform.uname.processor}.json";

      # No test may re-bless the seed scenes (docs/testing.md, "Seed traces
      # and re-blessing"): none runs rebless.sh, and the one that passes
      # --bless
      # (seed_bless_source) checks that the replayer refuses to write into
      # tests/seed. Run in each build's test directory, with jq.
      noRebless = ''
        ctest --show-only=json-v1 \
          | jq -e '[.tests[] | select(.name != "seed_bless_source")
                     | select(any(.command[]; test("rebless\\.sh|^--bless$|^--record-scale$")))
                     | .name]
                   | if length == 0 then true else error("tests that re-bless: \(.)") end'
      '';

      # The library, built and tested. `nix flake check` builds it with each
      # compiler, with the sanitizers (docs/determinism.md, "Where it is
      # checked"), and as a shared library.
      jitterbug =
        pkgs:
        {
          stdenv ? pkgs.clangStdenv,
          sanitize ? false,
          shared ? false,
        }:
        stdenv.mkDerivation {
          pname = "jitterbug" + lib.optionalString sanitize "-sanitize" + lib.optionalString shared "-shared";
          inherit version src;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            # jitterbug.pc is checked as it is installed.
            pkgs.validatePkgConfig
          ];
          buildInputs = [
            pkgs.vulkan-headers
            pkgs.vulkan-loader
            # The examples' windows (built, not run, by the checks).
            pkgs.sdl3
          ];
          cmakeFlags = [
            (lib.cmakeBool "JB_WERROR" true)
            (lib.cmakeBool "JB_SANITIZE" sanitize)
            # The file loaders' libFuzzer target and its short run, with the
            # sanitizers (clang; tests/CMakeLists.txt, "Files and converters").
            (lib.cmakeBool "JB_LIBFUZZER" sanitize)
            (lib.cmakeBool "BUILD_SHARED_LIBS" shared)
            (lib.cmakeBool "JB_TESTS" true)
            (lib.cmakeBool "JB_EXAMPLES" true)
            # jb-bench, for its --check (no timing in a check).
            (lib.cmakeBool "JB_TOOLS" true)
            (lib.cmakeFeature "JB_VULKAN" "ON")
            (lib.cmakeFeature "JB_LAVAPIPE_ICD" (lavapipeIcd pkgs))
            # The conformance programs not in the repository (tests/conformance).
            (lib.cmakeFeature "JB_CONFORMANCE_FETCHED" "${conformanceLists pkgs}")
            # A probe_pty_* test without a pseudo-terminal fails here
            # rather than being skipped (the sandbox has one).
            (lib.cmakeBool "JB_REQUIRE_PTY" true)
          ];
          # The sanitizers and _FORTIFY_SOURCE don't mix.
          hardeningDisable = lib.optionals sanitize [ "all" ];
          doCheck = true;
          nativeCheckInputs = [ pkgs.jq ];
          # lavapipe wants somewhere to keep its shader cache.
          preCheck = ''
            export HOME=$TMPDIR
            ${noRebless}
          '';
          meta = {
            description = "A C library that renders like the PlayStation GPU";
            license = lib.licenses.zlib;
            platforms = supportedSystems;
          };
        };

      # The emscripten build (docs/design.md, "Design choices for 0.1": the
      # browser is a first-class target), single-threaded. Emscripten needs a
      # writable cache of its compiled system libraries; the store's copy is
      # read-only, so each
      # build starts from a writable copy of it in $TMPDIR (its prebuilt
      # libraries are reused, and what is missing for these flags, such as
      # the optimised libc, is compiled there, offline).
      #   tests = true:  the library, its tests and the examples (first-light
      #                  and the demo, their pages' modules too), run under
      #                  Node by ctest (checks.wasm)
      #   tests = false: an example's web page, ready to serve: page is its
      #                  directory under examples/, and the name of its
      #                  module (packages.first-light-web, packages.demo-web)
      wasm =
        pkgs:
        {
          tests,
          page ? "first-light",
        }:
        pkgs.stdenv.mkDerivation {
          pname = if tests then "jitterbug-wasm" else "${page}-web";
          inherit version src;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.emscripten
            pkgs.nodejs
            pkgs.jq
          ];
          # The CMake hook would configure for the build machine.
          dontUseCmakeConfigure = true;
          configurePhase = ''
            runHook preConfigure
            export HOME=$TMPDIR
            export EM_CACHE=$TMPDIR/emscripten-cache
            cp -r ${pkgs.emscripten}/share/emscripten/cache $EM_CACHE
            chmod -R u+w $EM_CACHE
            emcmake cmake -S . -B build -G Ninja \
              -DCMAKE_BUILD_TYPE=Release \
              -DJB_WERROR=ON \
              -DJB_VULKAN=OFF \
              -DJB_TESTS=${if tests then "ON" else "OFF"} \
              -DJB_EXAMPLES=ON \
              -DJB_TOOLS=${if tests then "ON" else "OFF"} \
              -DJB_CONFORMANCE_FETCHED=${conformanceLists pkgs}
            runHook postConfigure
          '';
          buildPhase = ''
            runHook preBuild
            cmake --build build -j $NIX_BUILD_CORES
            runHook postBuild
          '';
          doCheck = true;
          # The tests, or for the page alone its module test (the frames
          # through the functions index.html calls, against expected.txt).
          checkPhase = ''
            runHook preCheck
          ''
          + (
            if tests then
              ''
                (cd build && ${noRebless})
                ctest --test-dir build --output-on-failure -j $NIX_BUILD_CORES
              ''
            else
              ''
                node examples/${page}/web/test-module.cjs build/examples/${page}/${page}.js \
                  examples/${page}/web/index.html examples/${page}/expected.txt
              ''
          )
          + ''
            runHook postCheck
          '';
          installPhase = ''
            runHook preInstall
            mkdir -p $out
          ''
          + (
            if tests then
              ''
                cp build/Testing/Temporary/LastTest.log $out/
                # jb-bench for Node: node $out/jb-bench.js (docs/performance.md)
                cp build/tools/jb-bench/jb-bench.{js,wasm} $out/
              ''
            else
              ''
                cp build/examples/${page}/{index.html,${page}.js,${page}.wasm} $out/
              ''
          )
          + ''
            runHook postInstall
          '';
          meta = {
            description = "jitterbug built with emscripten";
            license = lib.licenses.zlib;
            platforms = supportedSystems;
          };
        };
      # tools/jb-bench, optimised as a user's build is (CMake's Release,
      # with the stdenv's usual hardening flags), to run on any machine:
      #   nix build .#bench && result/bin/jb-bench
      # or nix copy it to another machine (docs/performance.md). The
      # check runs its --check, which times nothing.
      bench =
        pkgs:
        pkgs.clangStdenv.mkDerivation {
          pname = "jb-bench";
          inherit version src;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
          ];
          cmakeBuildType = "Release";
          cmakeFlags = [
            (lib.cmakeBool "JB_WERROR" true)
            (lib.cmakeBool "JB_TESTS" false)
            (lib.cmakeBool "JB_EXAMPLES" false)
            (lib.cmakeBool "JB_TOOLS" true)
            (lib.cmakeFeature "JB_VULKAN" "OFF")
          ];
          doInstallCheck = true;
          installCheckPhase = ''
            runHook preInstallCheck
            $out/bin/jb-bench --check
            runHook postInstallCheck
          '';
          meta = {
            description = "jitterbug's frame-time benchmark";
            license = lib.licenses.zlib;
            platforms = supportedSystems;
            mainProgram = "jb-bench";
          };
        };

      # The conformance lists recorded from the pinned sources
      # (docs/testing.md, "Conformance tests and the recorder"):
      # $out/tests/conformance/*.jbc for
      # all 13 programs, the console's dumps and logs as
      # third_party/ps1-tests/gpu has them, both licences and NOTICE.md. Three
      # programs' lists and dumps exist only here, never in the repository
      # (third_party/ps1-tests/README.md); every jitterbug build below gets
      # this output as JB_CONFORMANCE_FETCHED. The recorder records each
      # program twice and checks the two agree.
      conformanceLists =
        pkgs:
        pkgs.clangStdenv.mkDerivation {
          pname = "jitterbug-conformance-lists";
          inherit version;
          src = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [
              ./tools/conformance-record
              ./tests/conformance/jbc_format.h
            ];
          };
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
          ];
          dontUseCmakeConfigure = true;
          buildPhase = ''
            runHook preBuild
            . tools/conformance-record/pins.sh
            [ "$PS1_TESTS_REV" = ${ps1-tests.rev} ] && [ "$PSN00BSDK_REV" = ${psn00bsdk.rev} ] ||
              { echo "flake.nix and tools/conformance-record/pins.sh pin different commits"; exit 1; }
            # Every ps1-tests-* input is a commit programs.txt names, and the
            # other way round (record.sh fails on a commit it is not given).
            for a in ${lib.concatStringsSep " " programSources}; do
              awk '!/^#/' tools/conformance-record/programs.txt | grep -qw "''${a%%=*}" ||
                { echo "flake.nix: ''${a%%=*} is not in programs.txt"; exit 1; }
            done
            bash tools/conformance-record/record.sh ${ps1-tests} ${psn00bsdk} "$TMPDIR/work" "$out" \
              ${lib.concatStringsSep " " programSources}
            runHook postBuild
          '';
          dontInstall = true;
          meta = {
            description = "ps1-tests' GPU programs recorded as conformance lists, for local testing";
            platforms = supportedSystems;
          };
        };

      # The checked-in lists, dumps and logs are what the recorder makes, and
      # nothing of the programs fixtures.txt marks "fetched" is checked in.
      conformanceRecordCheck =
        pkgs:
        let
          lists = conformanceLists pkgs;
        in
        pkgs.runCommand "jitterbug-conformance-record" { } ''
          fixtures=${src}/tests/conformance/fixtures.txt
          awk '!/^#/ && NF >= 7 && $2 == "repo" { print $1 }' $fixtures | sort >repo
          awk '!/^#/ && NF >= 7 && $2 == "fetched" { print $1 }' $fixtures | sort >fetched
          awk '!/^#/ && NF >= 7 { print $1 }' $fixtures | sort >all
          (cd ${lists}/tests/conformance && ls *.jbc | sed 's/\.jbc$//') | sort >made
          diff all made
          (cd ${src}/tests/conformance && ls *.jbc | sed 's/\.jbc$//') | sort >checked-in
          diff repo checked-in
          (cd ${src}/third_party/ps1-tests/gpu && ls) | sort >checked-in-results
          diff repo checked-in-results
          n=0
          while read -r p; do
            cmp ${lists}/tests/conformance/$p.jbc ${src}/tests/conformance/$p.jbc
            (cd ${lists}/third_party/ps1-tests/gpu/$p && ls) >made-files
            (cd ${src}/third_party/ps1-tests/gpu/$p && ls) >checked-in-files
            diff made-files checked-in-files
            for f in $(cat made-files); do
              cmp ${lists}/third_party/ps1-tests/gpu/$p/$f ${src}/third_party/ps1-tests/gpu/$p/$f
            done
            n=$((n + 1))
          done <repo
          cmp ${lists}/third_party/ps1-tests/LICENSE ${src}/third_party/ps1-tests/LICENSE
          # Every line's identity (NAME WORDS CHECKSUM REFERENCE REFSUM) is the
          # recording's, fetched programs included
          # (tests/conformance/FORMAT.md, "Fixture tables").
          awk '!/^#/ && NF >= 8 { print $1, $6, $7, $3, $8 }' $fixtures | sort >table-identity
          sort ${lists}/fixtures-identity.txt >made-identity
          diff table-identity made-identity
          while read -r p; do
            if [ -e ${src}/tests/conformance/$p.jbc ] || [ -e ${src}/third_party/ps1-tests/gpu/$p ]; then
              echo "$p is fetched-only (fixtures.txt), but its list or dump is checked in"
              exit 1
            fi
          done <fetched
          if [ -e ${src}/third_party/psn00bsdk ]; then
            echo "third_party/psn00bsdk: nothing of the SDK is checked in"
            exit 1
          fi
          echo "$n checked-in lists, dumps and logs identical to the recording;" \
            "$(wc -l <fetched) fetched-only programs, none of them checked in"
          touch $out
        '';

      # Differential fuzzing against psxstack's GPU (docs/testing.md,
      # "Differential fuzzing"), on fixed seeds. What passes or fails it: the
      # generated streams'
      # hashes (hashes.txt), strict mode (600 streams of seed 1: any
      # difference fails), bounded mode (200 streams of seed 1: any pixel
      # outside the bounded residual fails), the mask check's invariant on
      # synthetic images (invariants), whole-stream transcripts that must
      # play back with no difference, the tool's fixed cases (cases.sh),
      # the mutation self-test (selftest.sh:
      # ten deliberate bugs, each caught by exactly the modes recorded,
      # line-clip by tests/line_clip.c, three fixed streams, and every
      # failure reproduced, saved and played back failing), and the tool
      # built with ASan and UBSan on shorter runs (any report fails). For the log only: psxstack against the console's dumps, the assumptions probe, a
      # short exploratory run (its labels are hypotheses, never a pass; its
      # focuses, xflip among them, are for looking at one rule and are not
      # run here) and the open-question lists through both renderers.
      fuzzDiffCheck =
        pkgs:
        pkgs.clangStdenv.mkDerivation {
          pname = "jitterbug-fuzz-diff";
          inherit version src;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
          ];
          dontUseCmakeConfigure = true;
          # The sanitized build (as the library's sanitize check).
          hardeningDisable = [ "all" ];
          buildPhase = ''
            runHook preBuild
            . tools/fuzz-diff/pin.sh
            [ "$PSXSTACK_REV" = ${psxstack.rev} ] ||
              { echo "flake.nix and tools/fuzz-diff/pin.sh pin different commits"; exit 1; }
            cmake -S tools/fuzz-diff -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DJB_WERROR=ON \
              -DPSXSTACK_DIR=${psxstack}
            cmake --build build -j $NIX_BUILD_CORES
            # All 13 lists: the checked-in ones (with lines' exclusion) and
            # the fetched-only ones from the recorder's output.
            mkdir lists
            cp tests/conformance/*.jbc tests/conformance/*.exclude lists/
            cp -n ${conformanceLists pkgs}/tests/conformance/*.jbc lists/
            [ "$(ls lists/*.jbc | wc -l)" = 13 ]
            build/fuzz-diff conformance lists ${conformanceLists pkgs}/third_party/ps1-tests/gpu
            build/fuzz-diff assumptions
            # The checks.
            build/fuzz-diff hash --streams 300 | diff - tools/fuzz-diff/hashes.txt
            build/fuzz-diff strict --seed 1 --streams 600 --repros 3 --max-failures 3
            build/fuzz-diff bounded --seed 1 --streams 200 --repros 3 --max-failures 3
            build/fuzz-diff invariants
            # A saved transcript of a stream that passes must play back the
            # same on both sides (no false alarm from the saving or play).
            mkdir -p whole/b
            build/fuzz-diff repro 1 0 --mode strict --whole --out whole
            build/fuzz-diff repro 1 0 --mode bounded --whole --out whole/b
            build/fuzz-diff play whole/*.jbc whole/b/*.jbc
            # The tool's fixed cases (cases.sh): lists play must judge
            # right (a GP1(00h) mid-list, GP0 ops of several commands or
            # part of one) and inputs it must refuse.
            sh tools/fuzz-diff/cases.sh build/fuzz-diff "$TMPDIR/cases"
            sh tools/fuzz-diff/selftest.sh ${psxstack} "$TMPDIR/selftest" 600 200
            # The same tool under ASan and UBSan, shorter: every generator,
            # strict, bounded, and exploratory streams with shrinking.
            cmake -S tools/fuzz-diff -B build-san -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
              -DJB_WERROR=ON -DJB_SANITIZE=ON -DPSXSTACK_DIR=${psxstack}
            cmake --build build-san -j $NIX_BUILD_CORES
            export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
            build-san/fuzz-diff hash --streams 300 | diff - tools/fuzz-diff/hashes.txt
            build-san/fuzz-diff strict --seed 1 --streams 100 --repros 0
            build-san/fuzz-diff bounded --seed 1 --streams 40 --repros 0
            build-san/fuzz-diff invariants
            sh tools/fuzz-diff/cases.sh build-san/fuzz-diff "$TMPDIR/cases-san"
            build-san/fuzz-diff run --seed 1 --streams 20 --repros 1 --quiet > /dev/null
            # For the log.
            build/fuzz-diff run --seed 1 --streams 60 --repros 0 --quiet
            build/fuzz-diff play tests/conformance/open/*.jbc || true
            runHook postBuild
          '';
          installPhase = ''
            touch $out
          '';
          meta = {
            description = "jitterbug against psxstack's GPU on fixed seeds";
            platforms = supportedSystems;
          };
        };
      # third_party/ps1-tests/gte is what tools/gte-cases converts from the
      # pinned ps1-tests.
      gteCasesCheck =
        pkgs:
        pkgs.runCommand "jitterbug-gte-cases"
          {
            nativeBuildInputs = [ pkgs.python3 ];
          }
          ''
            PS1_TESTS_DIR=${ps1-tests} bash ${src}/tools/gte-cases/regen.sh --check
            touch $out
          '';

      # The source release: every tracked file (what `git archive
      # --prefix=jitterbug-VERSION/ HEAD` gives from a clean commit), as
      # $out/jitterbug-VERSION.tar.gz, the same bytes from the same tree.
      # It builds and tests offline with CMake alone: the flake's other
      # inputs are only for the conformance recorder, fuzz-diff and the GTE
      # cases (checks.package).
      sourceTarball =
        pkgs:
        pkgs.runCommand "jitterbug-${version}-source" { } ''
          mkdir -p $out
          tar --sort=name --mtime=@1 --owner=0 --group=0 --numeric-owner \
            --mode=u+w,go-w,a+rX --transform 's,^\.,jitterbug-${version},' \
            -C ${self} -cf - . | gzip -9n > $out/jitterbug-${version}.tar.gz
        '';

      # What a user of the release gets (tests/package/check.cmake): the
      # source tarball configured with nothing but CMake, built, and its
      # tests run (no JB_CONFORMANCE_FETCHED: the fetched-only programs are
      # left out, as the status says); then installed, static and shared
      # and for wasm32, into a prefix that is then moved, and a consumer
      # built against each through find_package, pkg-config (not wasm) and
      # add_subdirectory, and run.
      packageCheck =
        pkgs:
        pkgs.clangStdenv.mkDerivation {
          pname = "jitterbug-package";
          inherit version;
          src = "${sourceTarball pkgs}/jitterbug-${version}.tar.gz";
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.emscripten
            pkgs.nodejs
          ];
          dontUseCmakeConfigure = true;
          buildPhase = ''
            runHook preBuild
            export HOME=$TMPDIR
            cmake -S . -B build -G Ninja
            cmake --build build -j $NIX_BUILD_CORES
            ctest --test-dir build --output-on-failure -j $NIX_BUILD_CORES
            for shared in OFF ON; do
              cmake -DSOURCE=$PWD -DWORK=$TMPDIR/package-$shared -DSHARED=$shared \
                -DGENERATOR=Ninja -DCC=$CC -P tests/package/check.cmake
            done
            # emscripten's writable cache, as in the wasm builds.
            export EM_CACHE=$TMPDIR/emscripten-cache
            cp -r ${pkgs.emscripten}/share/emscripten/cache $EM_CACHE
            chmod -R u+w $EM_CACHE
            cmake -DSOURCE=$PWD -DWORK=$TMPDIR/package-wasm -DGENERATOR=Ninja \
              -DTOOLCHAIN=${pkgs.emscripten}/share/emscripten/cmake/Modules/Platform/Emscripten.cmake \
              -DEMULATOR=${pkgs.nodejs}/bin/node -P tests/package/check.cmake
            runHook postBuild
          '';
          installPhase = ''
            mkdir -p $out
            cp build/Testing/Temporary/LastTest.log $out/
          '';
          meta = {
            description = "jitterbug's release tarball, built, tested, installed and used";
            platforms = supportedSystems;
          };
        };

      # A Nix user's build against the flake's packages: packages.default
      # (static, with the tools) and the shared library's build, through
      # find_package and pkg-config.
      packageNixCheck =
        pkgs:
        pkgs.clangStdenv.mkDerivation {
          pname = "jitterbug-package-nix";
          inherit version;
          src = lib.fileset.toSource {
            root = ./.;
            fileset = ./tests/package;
          };
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          # Not buildInputs: the compiler wrapper would then add their
          # include and library paths itself and hide a wrong .pc or CMake
          # package. check.cmake finds each through PREFIX alone.
          dontUseCmakeConfigure = true;
          buildPhase = ''
            runHook preBuild
            cmake -DPREFIX=${jitterbug pkgs { }} -DTOOLS=ON -DWORK=$TMPDIR/static -DGENERATOR=Ninja \
              -DCC=$CC -P tests/package/check.cmake
            cmake -DPREFIX=${jitterbug pkgs { shared = true; }} -DSHARED=ON -DTOOLS=ON \
              -DWORK=$TMPDIR/shared -DGENERATOR=Ninja -DCC=$CC -P tests/package/check.cmake
            runHook postBuild
          '';
          installPhase = ''
            touch $out
          '';
          meta = {
            description = "A consumer of jitterbug's Nix packages";
            platforms = supportedSystems;
          };
        };

      # The determinism checks (docs/determinism.md) beyond the builds above.
      #
      # aarch64-linux, cross-compiled from x86-64 (with gcc, or with clang)
      # and run under QEMU user-mode emulation: the library, its tests,
      # first-light and jb-bench, every test through the emulator
      # (CMAKE_CROSSCOMPILING_EMULATOR), so no binfmt registration is
      # needed. The cross toolchains and qemu come from the binary cache.
      # nixpkgs turns doCheck off when the build machine cannot run the
      # host's programs, so the tests run in a phase of their own.
      arm64 =
        pkgs:
        { clang }:
        let
          cross = pkgs.pkgsCross.aarch64-multiplatform;
          stdenv = if clang then cross.clangStdenv else cross.stdenv;
          emulator = cross.stdenv.hostPlatform.emulator pkgs;
        in
        stdenv.mkDerivation {
          pname = "jitterbug-aarch64" + lib.optionalString clang "-clang";
          inherit version src;
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.jq
            pkgs.file
          ];
          cmakeFlags = [
            (lib.cmakeBool "JB_WERROR" true)
            (lib.cmakeBool "JB_TESTS" true)
            (lib.cmakeBool "JB_EXAMPLES" true)
            (lib.cmakeBool "JB_TOOLS" true)
            (lib.cmakeFeature "JB_VULKAN" "OFF")
            (lib.cmakeFeature "CMAKE_CROSSCOMPILING_EMULATOR" emulator)
            (lib.cmakeFeature "JB_CONFORMANCE_FETCHED" "${conformanceLists pkgs}")
            # A probe_pty_* test without a pseudo-terminal fails here
            # rather than being skipped (the sandbox has one).
            (lib.cmakeBool "JB_REQUIRE_PTY" true)
          ];
          postBuild = ''
            # Every test runs through the emulator or is the build machine's
            # cmake; none may count on the kernel running aarch64 programs.
            ctest --show-only=json-v1 > tests.json
            jq -e --arg emu ${emulator} --arg cmake ${pkgs.cmake}/bin/cmake \
              '[.tests[] | select(.command[0] != $emu and .command[0] != $cmake) | .name]
               | if length == 0 then true else error("not run through the emulator: \(.)") end' \
              tests.json
            file tools/jb-bench/jb-bench | grep -q 'ARM aarch64'
            ${noRebless}
            ctest --output-on-failure -j $NIX_BUILD_CORES
          '';
          installPhase = ''
            mkdir -p $out
            cp Testing/Temporary/LastTest.log $out/
          '';
          meta = {
            description = "jitterbug for aarch64-linux, tested under QEMU";
            license = lib.licenses.zlib;
            # The host it is built for; the build machine is x86-64.
            platforms = [ "aarch64-linux" ];
          };
        };

      # The hardware probe for the console (docs/probe.md): the MIPS I
      # build of tools/probe/probe as a PS-X EXE, by its build script, which
      # also checks the EXE's header, the memory map and the instruction set
      # (mkexe.py --check). GCC and binutils for mipsel from the binary
      # cache, unwrapped: freestanding, no C library, start files or libgcc.
      #   $out/probe.exe, probe.elf, probe.map
      probeToolchain = pkgs: [
        pkgs.pkgsCross.mipsel-linux-gnu.buildPackages.gcc.cc
        pkgs.pkgsCross.mipsel-linux-gnu.buildPackages.binutils.bintools
      ];
      probe =
        pkgs:
        pkgs.stdenvNoCC.mkDerivation {
          pname = "jitterbug-probe";
          inherit version;
          src = lib.fileset.toSource {
            root = ./.;
            fileset = lib.fileset.unions [
              ./tools/probe
              ./tests/conformance/jbc_format.h
            ];
          };
          nativeBuildInputs = probeToolchain pkgs ++ [ pkgs.python3 ];
          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            bash tools/probe/probe/psx/build.sh "$TMPDIR/probe"
            mkdir -p $out
            cp "$TMPDIR"/probe/probe.{exe,elf,map} $out/
            runHook postBuild
          '';
          dontInstall = true;
          dontFixup = true;
          meta = {
            description = "jitterbug's hardware probe, a PS-X EXE for the console";
            license = lib.licenses.zlib;
            platforms = supportedSystems;
          };
        };

      # The clang build at other optimisation levels, against the same
      # stored hashes as every build: -O0 (CMake's Debug), on the build
      # machine.
      O0 =
        pkgs:
        (jitterbug pkgs { }).overrideAttrs {
          pname = "jitterbug-O0";
          cmakeBuildType = "Debug";
        };

      # -O3 for x86-64-v3: the compiler may use CMPXCHG16B, LAHF-SAHF,
      # POPCNT, SSE3 to SSE4.2, SSSE3, AVX, AVX2, BMI1, BMI2, F16C, FMA,
      # LZCNT and MOVBE anywhere (and rely on OSXSAVE), and
      # -ffp-contract=off is what keeps fused multiply-adds out of the float
      # options (docs/determinism.md, "How the code keeps it"). The tests run under
      # QEMU's user-mode emulation of a Haswell (the oldest model QEMU has
      # with all of x86-64-v3), whatever CPU the build machine has, so a
      # pass always means the suite ran. As in the arm64 checks, every test
      # goes through CMAKE_CROSSCOMPILING_EMULATOR, which postBuild asserts.
      # postBuild also asserts that the isa test runs with --expect
      # x86-64-v3 (tests/isa.c: it fails unless the build was compiled for
      # x86-64-v3 and the CPU the tests see advertises and executes each of
      # its requirements, OSXSAVE through xgetbv), that each instruction it
      # claims to run is in its binary, that the library uses the 256-bit
      # AVX registers, and that it has no fused multiply-add. The Vulkan smoke test is left out (no
      # lavapipe ICD): it draws nothing and runs in the native checks.
      x86-64-v3 =
        pkgs:
        let
          qemu = "${pkgs.qemu-user}/bin/qemu-x86_64";
          emulator = "${qemu};-cpu;Haswell-v4";
        in
        (jitterbug pkgs { }).overrideAttrs (old: {
          pname = "jitterbug-x86_64-v3";
          cmakeBuildType = "Release";
          nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.jq ];
          cmakeFlags = builtins.filter (f: !(lib.hasPrefix "-DJB_LAVAPIPE_ICD" f)) old.cmakeFlags ++ [
            (lib.cmakeFeature "CMAKE_C_FLAGS" "-march=x86-64-v3")
            (lib.cmakeFeature "CMAKE_CROSSCOMPILING_EMULATOR" emulator)
            (lib.cmakeFeature "JB_ISA_EXPECT" "x86-64-v3")
          ];
          postBuild = ''
            ctest --show-only=json-v1 > tests.json
            jq -e --arg emu ${qemu} --arg cmake ${pkgs.cmake}/bin/cmake \
              '[.tests[] | select((.command[0] != $emu or .command[1:3] != ["-cpu", "Haswell-v4"])
                                  and .command[0] != $cmake) | .name]
               | if length == 0 then true else error("not run through the emulator: \(.)") end' \
              tests.json
            jq -e '[.tests[] | select(.name == "isa") | .command[-2:]] == [["--expect", "x86-64-v3"]]' \
              tests.json
            # The instructions tests/isa.c executes, one class each, are in it.
            objdump=''${OBJDUMP:-objdump}
            $objdump -d tests/test_isa > isa.dis
            for m in cmpxchg16b lahf sahf popcnt haddpd pmulld roundsd crc32 pshufb \
                     'vaddps\s.*%ymm[0-9]+' 'vpaddd\s.*%ymm[0-9]+' andn tzcnt pdep pext vcvtps2ph \
                     vcvtph2ps vfmadd231sd lzcnt movbe xgetbv; do
              grep -qE "\s$m(\s|$)" isa.dis || { echo "test_isa has no $m"; exit 1; }
            done
            # The compiler did use x86-64-v3 in the library, and fused no
            # multiply-add (-ffp-contract=off).
            $objdump -d libjitterbug.a > lib.dis
            echo "libjitterbug.a: $(grep -c '%ymm' lib.dis) instructions on ymm registers"
            grep -q '%ymm' lib.dis
            if grep -E '\svfn?m(add|sub)' lib.dis; then
              echo "libjitterbug.a has fused multiply-adds"
              exit 1
            fi
          '';
          meta = old.meta // {
            description = "jitterbug for x86-64-v3, tested under QEMU";
          };
        });
    in
    {
      formatter = forAllSystems (pkgs: pkgs.nixfmt);

      packages = forAllSystems (pkgs: {
        default = jitterbug pkgs { };
        # examples/first-light in a browser: index.html, first-light.js and
        # first-light.wasm, to serve from any static directory.
        first-light-web = wasm pkgs { tests = false; };
        # examples/demo in a browser: index.html, demo.js and demo.wasm.
        demo-web = wasm pkgs {
          tests = false;
          page = "demo";
        };
        # tools/jb-bench, Release (docs/performance.md).
        bench = bench pkgs;
        # The conformance lists, recorded again, with the three that are not
        # in the repository (tools/conformance-record; NOTICE.md in it).
        conformance-lists = conformanceLists pkgs;
        # The hardware probe for the console (tools/probe).
        probe = probe pkgs;
        # The source release, jitterbug-VERSION.tar.gz (checks.package).
        source = sourceTarball pkgs;
      });

      checks = forAllSystems (
        pkgs:
        {
          clang = jitterbug pkgs { };
          gcc = jitterbug pkgs { stdenv = pkgs.gccStdenv; };
          sanitize = jitterbug pkgs { sanitize = true; };
          # A shared library: its exports are exactly the header's functions
          # (tests/exports.cmake).
          shared = jitterbug pkgs { shared = true; };
          # wasm32 under Node: the library's tests and the examples' hashes
          # (tests/CMakeLists.txt says what a single-threaded build leaves out).
          wasm = wasm pkgs { tests = true; };
          # The conformance lists are what the recorder makes from the pinned
          # sources (tools/conformance-record).
          conformance-record = conformanceRecordCheck pkgs;
          # jitterbug against psxstack's GPU (tools/fuzz-diff).
          fuzz-diff = fuzzDiffCheck pkgs;
          # The GTE cases are what tools/gte-cases converts from the pin.
          gte-cases = gteCasesCheck pkgs;
          # The release tarball builds and tests offline, and installs a
          # package that programs can use (tests/package).
          package = packageCheck pkgs;
          # The same through packages.default and the shared build.
          package-nix = packageNixCheck pkgs;

        }
        # Determinism (docs/determinism.md): other optimisation levels;
        # x86-64-v3 and arm64 only on an x86-64 build machine, which builds
        # them (arm64 cross-compiled) and runs their tests under QEMU (an
        # aarch64 one runs the checks above natively).
        // {
          O0 = O0 pkgs;
        }
        // lib.optionalAttrs pkgs.stdenv.buildPlatform.isx86_64 {
          x86-64-v3 = x86-64-v3 pkgs;
          arm64 = arm64 pkgs { clang = false; };
          arm64-clang = arm64 pkgs { clang = true; };
          # The probe's MIPS build (tools/probe), so that it cannot rot: on
          # x86-64, where the cross toolchain is in the binary cache.
          probe = probe pkgs;
        }
      );

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.clang
            pkgs.clang-tools
            pkgs.lld
            pkgs.gdb
            pkgs.python3
            pkgs.sdl3
            # The web builds (examples/first-light/web, examples/demo/web)
            # and their tests.
            pkgs.emscripten
            pkgs.nodejs
            # The Vulkan backend (todo.md, "Planned after 0.1").
            pkgs.vulkan-headers
            pkgs.vulkan-loader
            pkgs.spirv-cross
            pkgs.glslang
            pkgs.vulkan-validation-layers
            pkgs.vulkan-tools
          ]
          # The hardware probe's MIPS build (tools/probe/probe/psx/build.sh).
          ++ probeToolchain pkgs;
          # CMake reads this and runs the Vulkan tests on lavapipe.
          JB_LAVAPIPE_ICD = lavapipeIcd pkgs;
        };
      });
    };
}
