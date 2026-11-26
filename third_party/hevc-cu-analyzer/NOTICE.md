# HEVC CU analyzer notices

BitScope can bundle an `HM_180` helper derived from
[Gitl HEVC Analyzer](https://github.com/lheric/GitlHEVCAnalyzer). It is used
locally to decode HEVC CABAC syntax and export the real CTU/CU quadtree,
partition mode, prediction category, QP, and approximate coded bytes.

The bundled helper was modified for BitScope:

- added a `decoder_qp.txt` export containing the QP of each leaf CU;
- added `BITSCOPE_HEVC_OUTPUT_FRAMES`, allowing the reference chain to be
  decoded normally while text output is limited to requested input pictures;
- built the decoder-only target for macOS arm64;
- BitScope starts it on demand without requesting decoded YUV output.

Gitl HEVC Analyzer is distributed under its modified Apache license. The
complete terms are in `GitlHEVCAnalyzer-LICENSE.txt`. In particular, its
license asks commercial users to notify the upstream authors and
non-commercial users to reference the upstream work.

The decoder contains the HM-18 reference software. Its BSD license is in
`HM-COPYING.txt`.
