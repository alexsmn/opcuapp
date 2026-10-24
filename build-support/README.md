# build-support — the shared CMake kit every product carries

<!-- doc-citations: external scripts/ -->

Status: Living reference
Last verified against code: 2026-10-24 (the CMake floor, skipping the linter);
2026-09-27 (multi-config library pinning); the rest 2026-08-08

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
cmake_minimum_required(VERSION 3.27)

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

## The CMake floor: 3.27, the same for every product

Every product's root `CMakeLists.txt` and `CMakePresets.json`
(`cmakeMinimumRequired`) declare **3.27**. It is one number on purpose: a
product spliced into a consumer runs its own `cmake_minimum_required()`, so the
real floor of any build is the highest one in its closure. That made the old
spread (3.15 to 3.27) a list of numbers nobody could rely on. `common` already
asked for 3.27 and almost everything consumes it, so for most products 3.27 was
the real floor already.

Measured on 2026-10-24 by configuring every product on macOS with CMake 3.25.2,
3.26.4, 3.27.9 and 3.31.10:

- **3.25 cannot configure** `core` or anything that consumes it, even with every
  declaration lowered to match. Generation stops at `Target "Boost::filesystem"
  not found`, raised from `core/metrics`, `display/view` and the client against
  vcpkg's Boost 1.91. A bare `find_package(Boost COMPONENTS log)` resolves on
  3.25, so the mechanism is not pinned down -- the product closure is what
  fails.
- **3.26 configures everything** once the declarations allow it. 3.27 is
  declared rather than 3.26 so that no product ever moves its policy settings
  backwards: lowering `common` would turn CMP0144-CMP0151 back to OLD there.
  Raising a product's floor only ever switches policies to NEW.

Raising a floor switches on every policy introduced up to it, in that
product's own directories. To find out what that changed, every product was
configured with CMake 4.4.2 before and after the raise, and the generated
`compile_commands.json` and `*.ninja` files were compared. Exactly one thing
differed: [CMP0151](https://cmake.org/cmake/help/latest/policy/CMP0151.html)
(3.27). It adds the AUTOMOC include directory as a *system* include, so the
Qt products that declared less than 3.27 (`display`, `designer`, `client`,
`graph_qt`, `view_manager_qt`) now pass `-isystem …_autogen/include_<Config>`
where they used to pass `-I`. `common` already did. All the other products,
the tiers among them, produced byte-identical build files after normalising
paths. That comparison is macOS only; no MSVC build was run.

Ubuntu 24.04 packages CMake 3.28.3, which meets the floor; 22.04's 3.22.1 does
not ([Launchpad](https://launchpad.net/ubuntu/+source/cmake), verified
2026-10-24).

**Nothing enforces the single number.** If a product ever needs something
newer, raise every product and every preset together, and redo both
measurements above. Bumping one product quietly raises the floor of every
product that consumes it, which is how `common`'s 3.27 became most of the
tree's floor without anyone saying so.

## Keeping a spliced product away from the linter

Wrap the `find_package()` that splices a consumed product or a vendor SDK in
`scada_skip_linting_begin()` / `scada_skip_linting_end()`, never in
`set(CMAKE_SKIP_LINTING ON/OFF)`. That variable only exists from CMake 4.2, so
below 4.2 the bracketed product was cppchecked anyway. And the `OFF` turned
linting back on inside a product that was itself being skipped, on every
version. The helpers blank the linter variables themselves and restore the
previous state. `scada_skip_linting_check` (ctest, at the root) builds a small
project with a stand-in linter and asserts which sources it was handed.

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

### Multi-config and vcpkg: which build of a library gets linked

"Ninja Multi-Config" leaves `CMAKE_BUILD_TYPE` undefined, and vcpkg's toolchain
reads that as a Debug build: `z_vcpkg_add_vcpkg_to_cmake_path` in
`scripts/buildsystems/vcpkg.cmake` puts `<triplet>/debug` ahead of `<triplet>`
in `CMAKE_PREFIX_PATH` whenever `CMAKE_BUILD_TYPE` is undefined
([vcpkg.cmake at f3e10653](https://github.com/microsoft/vcpkg/blob/f3e10653cc27d62a37a3763cd84b38bca07c6075/scripts/buildsystems/vcpkg.cmake),
the vcpkg this tree builds with; read 2026-09-27). It stops applying if vcpkg
ever orders these per configuration, which it cannot do for a single
`find_library` result. A package with its own CMake config is unaffected, since
its targets carry per-config locations. **A library found by one
`find_library` is not**: it gets the debug path for every configuration. Until
2026-09-27 that meant every product's Release build linked vcpkg's debug
OpenSSL (vcpkg's wrapper does a bare `find_library` everywhere but MSVC), and the
client's also linked the debug `libb2` that Qt Core finds through pkg-config.
Nothing failed; the binaries were just slower, the shipped Linux tiers
included, since `build_products.py` configures through the same preset.

`scada_pin_vcpkg_single_config_libraries()`, run from `scada_product_base()`
before any `find_package()`, answers those two lookups per configuration. It is
a list of known cases, not a general mechanism: a newly added dependency that is
found by `find_library` or pkg-config will have the same defect. To check a
product, grep its link lines after a configure:

```bash
grep -o 'vcpkg_installed/[^ ]*/debug/lib/[^ ]*' build/ninja/CMakeFiles/impl-Release.ninja | sort -u
```

Empty is right. `scada_single_config_libraries_check` (ctest, at the root)
pins the two known cases against CMake's own `FindOpenSSL`.
