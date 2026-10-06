Vendored from https://github.com/Theldus/Tiny2FA
Commit: b6fb16f26d0e4f0b7ff5efe039d6e19bdc8370ad

Original C sources and headers, MIT / Apache-2.0 (see LICENSE and file headers).
The C++ wrapper deliberately does not use t2_generate_secret_key: its fallback
uses rand() and its random-device read is unchecked. Generation uses a checked
Linux getrandom() call instead. Verification uses t2_get_key with explicit time
to implement replay protection and deterministic tests.
