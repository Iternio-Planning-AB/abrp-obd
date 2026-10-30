import os
import shlex
import subprocess


def cxx():
    return shlex.split(os.environ.get("CXX", "clang++"))


def compile_cpp(output, sources, *, includes, flags=()):
    subprocess.run([
        *cxx(), "-std=c++17", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Wall", "-Wextra", *flags,
        *(arg for path in includes for arg in ("-I", str(path))),
        *map(str, sources), "-o", str(output),
    ], check=True)


def runtime_environment():
    environment = dict(os.environ)
    environment["UBSAN_OPTIONS"] = environment.get("UBSAN_OPTIONS", "") + ":halt_on_error=1"
    return environment
