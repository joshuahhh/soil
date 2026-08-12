# todo

- **Update Eigen to 3.4.x.** `setup.sh` pins 3.3.7 (2018), which still uses
  `std::result_of` — deprecated in C++17, gone in C++20. The build papers over
  it with `-Wno-deprecated-declarations` (see build.sh); 3.4.x would let that
  suppression go. It's a one-line version bump in setup.sh plus a cache-key
  bump in .github/workflows/deploy.yml.
