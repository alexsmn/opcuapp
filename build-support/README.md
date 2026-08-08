# build-support — the shared CMake kit every product carries

Status: Living reference
Last verified against code: 2026-08-08

This directory is what lets a product configure and build **on its own**, in the
monorepo or as a standalone export, without the superproject's root
`CMakeLists.txt`. It is authored once at the tree root and copied into every
product export by `tools/export/products.toml`, the same way `ports/` and
`vcpkg-configuration.json` already are.

It is called `build-support` and not `cmake` on purpose: an export maps the
product's directory to the export root, and `scada-server-framework/`,
`designer/`, `third_party/net`, `third_party/iec60870` and
`third_party/iec61850pp` each already have a `cmake/` that would be merged into.

The design is [ADR 0011](../docs/adr/0011-standalone-product-builds.md).

## What a product does

The bootstrap goes **before** `project()`, not after:

```cmake
cmake_minimum_required(VERSION 3.25)

# Locate the shared kit: it is at this product's root in an export, and at the
# tree root in the monorepo.
set(_scada_dir "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_scada_dir}/build-support/ScadaProducts.cmake")
  get_filename_component(_scada_parent "${_scada_dir}" DIRECTORY)
  if(_scada_parent STREQUAL _scada_dir)
    message(FATAL_ERROR "build-support/ not found above ${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  set(_scada_dir "${_scada_parent}")
endwhile()
include("${_scada_dir}/build-support/ScadaProducts.cmake")
include("${_scada_dir}/build-support/ScadaProductBase.cmake")
unset(_scada_dir)
unset(_scada_parent)

scada_product_prologue()

project(scada-core VERSION 2.6.0 LANGUAGES CXX)

scada_product_base()
scada_find_products(net)   # the products this one consumes, by name
```

**The prologue must come before `project()`.** The vcpkg toolchain runs inside
`project()` and reads `VCPKG_TARGET_TRIPLET`, `VCPKG_INSTALLED_DIR` and the
manifest variables at that moment, so a machine config read afterwards would be
read too late to set any of them. Everything that needs to know the compiler —
the MSVC search paths, cppcheck — waits for `scada_product_base()`.

`scada_product_prologue()` does nothing when the product has been spliced into a
consumer: the consumer has already read the machine config, and a consumed
product must not overwrite cache variables the consumer chose.

The names are the product names in `tools/export/products.toml` — the same
vocabulary `$consumes` uses in the vcpkg manifests (ADR 0010).

`scada_find_products` puts each product's directory on `CMAKE_MODULE_PATH`, so
the existing `find_package(Transport)` / `find_package(ScadaCommon)` shims
resolve unchanged. Composition is still source splicing; only the *finding*
moved.

**`scada_find_products` and `$consumes` are not the same list, and should not
be made to match.** They answer different questions:

| | |
|---|---|
| `scada_find_products(x)` | *where is x* — puts it on the module path, and lets `scada_resolve_product(x <var>)` name a file inside it |
| `find_package(X)` | *compile x into me* — the `Find*.cmake` shim `add_subdirectory`s it, so x's dependency closure becomes mine |

`$consumes` tracks the second, which is why ADR 0010's checker derives it from
the `find_package` calls in the tree and would flag a `$consumes` entry with no
matching call. Every tier names `common` in `scada_find_products` because the
tier build reads files out of common's tree — the nodesets it stages, the e2e
harness — while compiling none of it; common arrives as code through the
framework. Adding `common` to a tier's `$consumes` would be wrong, and the
checker says so.

## Files

| File | Role |
|---|---|
| `ScadaProducts.cmake` | `scada_find_products()` — resolves product names to directories in either layout |
| `ScadaProductBase.cmake` | `scada_product_prologue()` / `scada_product_base()` — the settings that used to come from the root |
| `ScadaLocal.cmake` | Reads `.scada-local.cmake`; turns MSVC search paths into flags |
| `scada-local.cmake.example` | Template for the git-ignored machine file |

## The two layouts

`.scada-tree` at the monorepo root is the discriminator. It is in no export, and
it is also the generated name → directory table the resolver includes — one file
because the table names every product we sell, and this is the one thing a
customer never receives. Regenerate it with
`python3 tools/build/gen_product_paths.py --write`.

| | monorepo (`.scada-tree` found) | export (not found) |
|---|---|---|
| `core` | `<tree>/core` | `<product>/../core` |
| `net` | `<tree>/third_party/net` | `<product>/../net` |

So the monorepo needs the generated table (the `third_party/*` products are a
level down) and an export does not (everything is a sibling named after itself).
Either way `-DSCADA_PRODUCT_ROOT_<NAME>=<path>` overrides one product.

## Machine-specific settings

`.scada-local.cmake` beside `build-support/`, git-ignored, optional, read by
every product — see `scada-local.cmake.example`. `VCPKG_ROOT` is the one input
that must stay an environment variable, because presets name the toolchain file
and the toolchain runs before any of this.

On Windows the MSVC include and library directories reach the compiler as
`/external:I` and `/LIBPATH:` **flags**, not as an environment. That is
deliberate: the old preset `environment` block only reached the compiler through
`inheritConfigureEnvironment`, which is why any other build invocation died with
`C1083: Cannot open include file: 'type_traits'`.

## Presets

Every product carries a self-contained `CMakePresets.json` with the same names,
so instructions do not have to be per-product:

| Preset | Kind |
|---|---|
| `ninja` | configure (Ninja Multi-Config, vcpkg toolchain) |
| `debug` / `release` / `relwithdebinfo` | build |
| `test-debug` / `test-release` | test |

```bash
cmake --preset ninja
cmake --build --preset release
ctest --preset test-release
```

Output lands in that product's own `build/ninja/bin/<config>/`. There is no
shared `bin/` and no staging step; anything that needs another product's binary
is given its path.
