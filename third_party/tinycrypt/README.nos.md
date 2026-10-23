# TinyCrypt SHA-256 subset

Upstream: https://github.com/intel/tinycrypt

Pinned commit: `cf24c907a61c01bc2c4e1ee0ae24457c27a840a2`.

Only SHA-256, its utility source and required headers are bundled without source
changes. License: BSD 3-Clause, see LICENSE and individual file notices. This
subset hashes bounded keyboard configuration blobs on freestanding MCUs. It does
not supply randomness, Noise, signatures or a secure-boot implementation. The
host authenticated backend uses OpenSSL. Upstream is archived; no claim of ongoing
vendor support or independent security certification is made.
