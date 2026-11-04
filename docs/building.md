# Building

buh is a 32-bit DLL. With MSYS2, from a MINGW32 shell:

```
cmake -S . -B build -G Ninja
cmake --build build
```

Cross-compiling from Linux:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-w64-mingw32.cmake
cmake --build build
```

## This site

The pages are in `docs`, built with [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/). To see them
as you edit them:

```
pip install -r docs/requirements.txt
mkdocs serve
```

Pushing changes to them on `main` publishes them.
