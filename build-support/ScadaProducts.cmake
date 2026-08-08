# Product resolution — how one product finds the products it consumes.
#
# Composition in this tree is source splicing: every `Find*.cmake` shim is
# `if(NOT TARGET x) add_subdirectory(${CMAKE_CURRENT_LIST_DIR} x) endif()`, and
# `find_package` locates that shim through `CMAKE_MODULE_PATH`. Until ADR 0011
# the superproject's root `CMakeLists.txt` was the only thing that assembled
# that path, which is why no product configured on its own.
#
# `scada_find_products(<name>...)` does the assembly instead, from inside the
# product, in both layouts a product is built in:
#
#   monorepo   <tree>/scada-tier-iec104 consumes <tree>/third_party/net
#   export     <root>/scada-tier-iec104 consumes <root>/../net
#
# The names are the product names in `tools/export/products.toml` — the same
# vocabulary `$consumes` uses in the vcpkg manifests (ADR 0010).
#
# See docs/adr/0011-standalone-product-builds.md.

include_guard(GLOBAL)

# Where this kit sits. In the monorepo it is the tree root; in an export it is
# the product root, because the export drops `build-support/` inside the
# product. Everything below is relative to it.
get_filename_component(SCADA_BUILD_SUPPORT_DIR "${CMAKE_CURRENT_LIST_DIR}"
                       ABSOLUTE)
get_filename_component(SCADA_PRODUCT_SEARCH_ROOT
                       "${SCADA_BUILD_SUPPORT_DIR}/.." ABSOLUTE)

# `.scada-tree` marks the monorepo root and is exported by nothing, so its
# presence is what distinguishes the two layouts. It is also CMake code — the
# generated name -> path table — because in the monorepo the products do not all
# sit at the root and the resolver needs to be told where each one is. An export
# needs no table: every product is a sibling named after itself. Keeping the
# table in the one file no export carries is also what stops our full product
# list from shipping to a customer who bought one product.
if(EXISTS "${SCADA_PRODUCT_SEARCH_ROOT}/.scada-tree")
  set(SCADA_PRODUCT_LAYOUT "tree")
  include("${SCADA_PRODUCT_SEARCH_ROOT}/.scada-tree")
else()
  set(SCADA_PRODUCT_LAYOUT "siblings")
endif()

include("${SCADA_BUILD_SUPPORT_DIR}/ScadaLocal.cmake")

# Turns a product name into the suffix used in variable names:
# `scada-server-framework` -> `SCADA_SERVER_FRAMEWORK`, `graph_qt` -> `GRAPH_QT`.
function(scada_product_id name out_var)
  string(TOUPPER "${name}" _id)
  string(MAKE_C_IDENTIFIER "${_id}" _id)
  set(${out_var} "${_id}" PARENT_SCOPE)
endfunction()

# Resolves one product name to its source directory, or fails with a message
# that says what was looked for and how to override it.
#
# An explicit `SCADA_PRODUCT_ROOT_<ID>` always wins — that is the escape hatch
# for a checkout that does not sit where the layout expects (a differently named
# clone, a second worktree, a customer's own tree).
function(scada_resolve_product name out_var)
  scada_product_id("${name}" _id)

  if(SCADA_PRODUCT_ROOT_${_id})
    set(_dir "${SCADA_PRODUCT_ROOT_${_id}}")
    if(NOT EXISTS "${_dir}/CMakeLists.txt")
      message(FATAL_ERROR
        "SCADA_PRODUCT_ROOT_${_id} points at '${_dir}', which has no "
        "CMakeLists.txt.")
    endif()
    set(${out_var} "${_dir}" PARENT_SCOPE)
    return()
  endif()

  if(SCADA_PRODUCT_LAYOUT STREQUAL "tree")
    if(NOT DEFINED SCADA_PRODUCT_TREE_PATH_${_id})
      message(FATAL_ERROR
        "Unknown product '${name}'. It is not in .scada-tree, which is "
        "generated from tools/export/products.toml — add the product there and "
        "re-run tools/build/gen_product_paths.py --write.")
    endif()
    set(_dir "${SCADA_PRODUCT_SEARCH_ROOT}/${SCADA_PRODUCT_TREE_PATH_${_id}}")
  else()
    set(_dir "${SCADA_PRODUCT_SEARCH_ROOT}/../${name}")
  endif()
  get_filename_component(_dir "${_dir}" ABSOLUTE)

  if(NOT EXISTS "${_dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "Product '${name}' not found: looked in '${_dir}' (${SCADA_PRODUCT_LAYOUT} "
      "layout, rooted at '${SCADA_PRODUCT_SEARCH_ROOT}').\n"
      "In a standalone build every consumed product must be checked out beside "
      "this one. Point CMake at it explicitly with "
      "-DSCADA_PRODUCT_ROOT_${_id}=<path>.")
  endif()

  set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()

# Puts each named product's directory on `CMAKE_MODULE_PATH`, so the
# `find_package` calls further down resolve. A macro rather than a function
# because it edits `CMAKE_MODULE_PATH` in the caller's directory scope, which is
# what the `find_package` calls in that directory and its children read.
macro(scada_find_products)
  foreach(_scada_product_name IN ITEMS ${ARGN})
    scada_resolve_product("${_scada_product_name}" _scada_product_dir)
    list(APPEND CMAKE_MODULE_PATH "${_scada_product_dir}")
  endforeach()
  unset(_scada_product_name)
  unset(_scada_product_dir)
endmacro()
