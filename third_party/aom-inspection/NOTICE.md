# libaom inspection helper

BitScope can optionally bundle the official `inspect` example built from
[libaom](https://aomedia.googlesource.com/aom/) with `CONFIG_INSPECTION=1`.
The executable is renamed to `av1inspect` inside the application bundle.
BitScope applies `inspect-output-start.patch` so reference frames can still be
decoded without serializing block JSON for frames outside the requested range.

Windows source builds also apply `nasm-help-compat.patch`: query `-hO` for
optimization help and `-hf` for object formats, so NASM 3's split help output
does not cause libaom 3.13.1 to reject a supported assembler. This retains
the capability checks and optimized assembly. The build also runs
`tests/aom-nasm-smoke.cmake` to test detection and actual `-Ox -f win64` output.

libaom is distributed under the BSD 2-Clause License and the Alliance for Open
Media Patent License 1.0. The corresponding source version and full license
texts are available from the upstream libaom repository. No libaom source code
is copied into BitScope.
