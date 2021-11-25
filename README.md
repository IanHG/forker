Small server that can do `fork() + exec()`, and pipe output back to a client.

Useful, e.g., with MPI programs, where `fork() + exec()` is not supported.
The MPI program can instead send a request to a forker server,
which will handle the `fork() + exec()` and return the output.

Tags and filenames suggest C++ code, but it is actually just plain C
(or at least very easily convertible to C).
