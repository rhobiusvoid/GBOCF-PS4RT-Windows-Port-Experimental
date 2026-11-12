# Windows cross-build notes (Linux host, clang + MinGW-w64 posix, libstdc++)

Lessons that cost debugging time:
- Compile C++ with `-femulated-tls` and link against the shared libgcc (`-lgcc_s`), so the exe and
  libstdc++-6.dll share one `__emutls_get_address`. Otherwise `std::call_once` / `std::future`
  find a null `__once_call` and the process faults at address 0 inside `pthread_once`.
- Crypto++ must be compiled with the same `CRYPTOPP_DISABLE_*` defines as the library (layout
  differs otherwise: heap corruption in ps4rt-pkg).
- Link `ps4rt.exe` with `-Wl,--disable-high-entropy-va` and clear that flag in bundled DLLs
  (the launcher does this), or reserving the guest range fails with error 487.
- Link as a console app so `STOP:` messages are visible.
- `link.sh` is the exact ps4rt.exe link line used (paths are from the build machine).
