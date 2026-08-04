# todo

- **Update Eigen / manage the dependency better.** The tree vendors a full
  copy of Eigen 3.3.7 (2018) in `eigen/`, committed directly. It still uses
  `std::result_of`, which is deprecated in C++17 and gone in C++20 — the
  build currently papers over this with `-Wno-deprecated-declarations`
  (see build.sh). Upgrading to Eigen 3.4.x fixes that and would let the
  warning suppression be removed; while at it, consider a git submodule or
  a fetch step instead of a vendored copy so future upgrades are one-line.
