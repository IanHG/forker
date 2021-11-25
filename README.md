Small server that can do `fork() + exec()`, and pipe output back to a client.

Useful, e.g., for MPI programs, where `fork() exec()` is not supported.

Tags and filenames suggest C++ code, but should actually be plain C
(or at least easily convertible to C).
