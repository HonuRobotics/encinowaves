## Ehukai

Ehukai (from the Hawaiian ʻehukai, sea spray) is a **standalone, headless C++
library** for spectral ocean wave synthesis, maintained by
[Honu Robotics](https://github.com/HonuRobotics) for embedding in simulators
and tools (e.g. VRX / Gazebo).

> Ehukai is heavily based on
> [EncinoWaves](https://github.com/blackencino/EncinoWaves) by
> Christopher Jon Horvath, with the updates described in
> [What this library changes](#what-this-library-changes).

The basic architecture of these Waves is based on the TweakWaves application
written by Chris Horvath for Tweak Films in 2001.  This, in turn, was based
on the SIGGRAPH papers and courses by [Jerry Tessendorf][tessendorf], and by
the paper ["A Simple Fluid Solver based on the FTT" by Jos Stam][simplesolver].

[tessendorf]:   http://jerrytessendorf.blogspot.com/
[simplesolver]: http://www.dgp.toronto.edu/people/stam/reality/Research/pdf/jgt01.pdf

The TMA, JONSWAP, and Pierson Moskowitz Wave Spectra, as well as the
directional spreading functions are formulated based on the descriptions
given in "Ocean Waves: The Stochastic Approach",
by Michel K. Ochi, published by Cambridge Ocean Technology Series, 1998,2005.

This library is written as a working implementation of the paper:

> Christopher J. Horvath. 2015.   
> [Empirical directional wave spectra for computer graphics.](http://dl.acm.org/authorize?N90195)   
> In Proceedings of the 2015 Symposium on Digital Production (DigiPro '15),   
> Los Angeles, Aug. 8, 2015, pp. 29-39.    


## What this library changes

Compared to the original EncinoWaves, Ehukai keeps the spectral-synthesis
library and removes everything that made it a desktop application:

- **Headless library only** — the interactive OpenGL viewer (`SimpleSimViewer`,
  `GeepGLFW`) and its OpenEXR / GLFW / Boost / Xrandr dependencies are gone.
- **Eigen::FFT backend** — the FFTW backend is replaced by an `Eigen::FFT`
  (KissFFT) shim; no FFTW dependency.
- **Modern math** — IlmBase is replaced by Imath.
- **Standalone CMake** — exports an `ehukai::ehukai` target and a
  `find_package(ehukai)` config; dependencies are just Eigen3, TBB, Imath.
- **Headless smoke tests** under `test/` exercise the spectra → propagation
  pipeline without a GPU.

## Package installation (Ubuntu)

Prebuilt packages for Ubuntu 24.04 and 26.04 are available from the
[Honu Robotics APT repository](https://packages.honurobotics.com/). Follow
the setup instructions there, then:

```bash
sudo apt install libehukai-dev
```

## Building (Ubuntu)

Ehukai is developed and tested on **Ubuntu** (24.04 LTS recommended).

Install the build tools and dependencies:

```sh
sudo apt-get update
sudo apt-get install -y cmake build-essential \
  libeigen3-dev libtbb-dev libimath-dev
```

Configure and compile the library, running these (and the commands below) from
the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run the headless smoke tests:

```sh
ctest --test-dir build --output-on-failure
```

Optionally build and run the FFT microbenchmark (serial vs. parallel inverse FFT
at several grid sizes). It needs **Google Benchmark** and is off by default:

```sh
sudo apt-get install -y libbenchmark-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEHUKAI_BUILD_BENCHMARKS=ON
cmake --build build -j
./build/bench_FftwWrapper
```

Install (optional — headers, shared library, and the `ehukai` CMake config):

```sh
sudo cmake --install build
```


## Continuous integration

`.github/workflows/ci.yml` builds the library and runs the headless smoke tests
under two sanitizers on **Ubuntu Noble (24.04)** and **Resolute (26.04)**:

| Job       | Flags                          | Catches                                  |
| --------- | ------------------------------ | ---------------------------------------- |
| `address` | `-fsanitize=address,undefined` | heap/stack errors, leaks, undefined behaviour |
| `thread`  | `-fsanitize=thread`            | data races                               |

Pick a sanitizer at configure time with `-DSANITIZE=address|thread` and reproduce
either job locally:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSANITIZE=address
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

Two environment notes the CI bakes in, needed for the **thread** job:

- The system `libtbb` is not TSan-instrumented, so TBB's work-stealing scheduler
  trips benign races. `test/tsan.supp` (`TSAN_OPTIONS=suppressions=...`) silences
  only those; real findings are untouched.
- GCC's libtsan shadow layout collides with high-entropy ASLR
  (`FATAL: unexpected memory mapping`). Run the tests under `setarch -R` (no root)
  to disable per-process randomization.

Consume it from another CMake project:

```cmake
find_package(ehukai REQUIRED)
target_link_libraries(your_target PRIVATE ehukai::ehukai)
```


## Style and static analysis

The repo ships a `.clang-tidy` and a `CPPLINT.cfg`, so the checkers run with the
project's policy out of the box. The gate covers the files this library authors —
the FFT shim (`include/ehukai/FftwWrapper.h`) and the tests; the vendored
upstream headers are out of scope.

Install the tools:

```sh
sudo apt-get install -y cppcheck clang-tidy pipx
```

**cppcheck** and **cpplint** run via the pre-commit hook (below), so there is no
separate command to keep in sync with the config. **clang-tidy** is heavier — it
needs a compile database and is too slow for a hook — so run it manually or in
CI, from the repo root:

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
clang-tidy --quiet -p build test/test_FftwWrapper.cc test/test_headers.cc \
  test/test_Propagation.cc
```

It reads `.clang-tidy` and fails on any finding. `--quiet` drops the
`Suppressed N warnings` statistics line; the remaining `N warnings generated.`
counts are a clang frontend artifact from instantiating the vendored
Eigen/TBB/Imath templates — those headers are `-isystem`/system includes and
`HeaderFilterRegex` scopes reporting to `FftwWrapper.h`, so none of their
warnings are ever reported.

### Pre-commit hook

cpplint and cppcheck also run automatically via
[pre-commit](https://pre-commit.com) on the files this library authors. clang-tidy
is intentionally excluded — it is too slow for a hook and needs a compile
database, so run it in CI instead. The cppcheck hook uses the system `cppcheck`
from the install step above; cpplint is installed automatically by pre-commit
into an isolated environment, so no system cpplint is needed for the hook.

One-time setup, from the repo root:

```sh
pipx install pre-commit          # or: python3 -m pip install --user pre-commit
pre-commit install               # install the git hook into .git/hooks
```

After that, every `git commit` runs cpplint and cppcheck on the staged shim and
test files and blocks the commit if either reports a problem. To check the whole
tree at once — for example right after enabling the hook:

```sh
pre-commit run --all-files
```

To commit without running the hooks (use sparingly), pass `--no-verify`:

```sh
git commit --no-verify
```


### License

Copyright &copy; 2015 Christopher Jon Horvath. Ehukai modifications &copy; Honu Robotics.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
