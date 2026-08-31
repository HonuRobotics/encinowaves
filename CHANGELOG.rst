^^^^^^^^^^^^^^^^^^^^^^^^^^^^
Changelog for package ehukai
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

0.0.3 (2026-08-31)
------------------
* Declare the Imath dependency as the ``libimath-dev`` rosdep key.
* Make the Debian packaging opt-in via ``EHUKAI_ENABLE_DEB_PACKAGING`` so its
  copyright, changelog and lintian overrides stop appearing in every install.
* Install ``package.xml`` to ``share/ehukai``.
* Give the manifest an author, a license file and a bugtracker URL.
* Add a dependabot configuration for automated dependency updates.

0.0.2 (2026-08-12)
------------------
* Rename the project to Ehukai across packaging, documentation and the API,
  exporting ``ehukai::ehukai``.
* Add a ``package.xml`` so colcon builds the library as a workspace peer.
* Default to an optimized build type when none is given, so a plain colcon
  build no longer compiles the hot paths unoptimized.
* Point installation instructions at the Honu Robotics APT repository.
* Fix the distribution suffix flag names the release workflow passes.

0.0.1 (2026-07-29)
------------------
* Extract EncinoWaves as a standalone, headless C++ library: drop the OpenGL
  viewer and its OpenEXR, GLFW, Boost and Xrandr dependencies.
* Replace the FFTW backend with an ``Eigen::FFT`` shim, and IlmBase with Imath.
* Parallelize and cache the FFT shim.
* Speed up the InitialState directional spreading evaluation.
* Fix a Donelan-Banner negative swell sign bug, JONSWAP gamma variance, and the
  spreading modal frequency fetch unit mismatch against the JONSWAP spectrum.
* Deduplicate the cos-power spreadings and quiet the library.
* Add headless smoke tests plus AddressSanitizer and ThreadSanitizer coverage.
* Add cpplint, cppcheck and pre-commit configuration.
* Add Debian packaging and the APT repository release workflow.
