# libaom inspection helper

BitScope can optionally bundle the official `inspect` example built from
[libaom](https://aomedia.googlesource.com/aom/) with `CONFIG_INSPECTION=1`.
The executable is renamed to `av1inspect` inside the application bundle.
BitScope applies `inspect-output-start.patch` so reference frames can still be
decoded without serializing block JSON for frames outside the requested range.

libaom is distributed under the BSD 2-Clause License and the Alliance for Open
Media Patent License 1.0. The corresponding source version and full license
texts are available from the upstream libaom repository. No libaom source code
is copied into BitScope.
