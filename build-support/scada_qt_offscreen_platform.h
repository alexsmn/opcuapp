#pragma once

// The headless default every Qt test binary in this tree applies, in the one
// place every product can reach.
//
// It lives in `build-support/` because the products that need it may not
// include from one another (ADR 0011) and several of them are leaves that
// consume no product at all -- `display`, `graph_qt` and `view_manager_qt`
// each build Qt tests and each would otherwise carry a copy. It sat in two
// byte-identical copies, `designer/test/` and `display/display/test/`, until
// 2026-09-20, when adding the other two would have made four (backlog 808).
// The kit is already where the screen description beside this file lives, and
// every export carries `build-support/` at its own root, so one copy here
// reaches all five layouts.
//
// A product puts this directory on its test targets' include path -- one
// `include_directories("${SCADA_BUILD_SUPPORT_DIR}")` at the product root,
// which is set unconditionally by `ScadaProducts.cmake` in both the monorepo
// and an export. That is deliberately NOT done by
// `scada_qt_import_offscreen_platform_into_tests()`, which returns early on a
// Qt with no offscreen plugin: tying the include path to that guard would
// turn a missing plugin into a compile error in the file that exists to cope
// with one.
//
// Nothing here may need a Qt header: the mains that include it are listed as
// plain source files by binaries that link no library of ours at all.

#if !defined(_WIN32)
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#endif

namespace scada::qt_test {

// Put Qt on a headless platform with a desktop-sized screen, for a test
// process that has not been told otherwise.
//
// Two separate decisions, and they are guarded differently on purpose:
// WHICH PLATFORM, which is a macOS question, and WHICH SCREEN, which is not.
//
// WHICH PLATFORM. On macOS, `offscreen` unless the caller chose one. On cocoa
// every process that constructs a QApplication registers as a foreground
// application, so a `ctest` sweep -- one process per case, several at once
// under `-j` -- bounces a Dock icon and steals focus once per case, and every
// widget a test shows is mapped on the real screen. Offscreen renders to
// memory instead. `setenv` with overwrite=0 leaves an explicit
// QT_QPA_PLATFORM alone, so `QT_QPA_PLATFORM=cocoa` still runs a test on the
// native platform on purpose. Windows and Linux keep whatever they had:
// Windows is the platform the products are styled for, and a Linux CI run
// gets a real (virtual) display from xvfb.
//
// The plugin has to be linked in for a static Qt to find it:
// `scada_qt_import_offscreen_platform_into_tests()` in the root CMakeLists
// does that for every test executable in the build and passes the screen
// description's path as SCADA_QT_OFFSCREEN_PLATFORM_CONFIG. That definition
// is the only thing this helper keys on: it is set by the same CMake call
// that links the plugin, on the same target, so its presence proves the
// plugin is there, and its absence -- a build tree configured before that
// call existed, or an export whose kit lacks it -- leaves the platform alone
// and the tests run on cocoa as they always did. Forcing `offscreen` without
// it would abort every Qt test at startup with `Could not find the Qt
// platform plugin "offscreen"` in a tree that merely had not been
// reconfigured yet.
//
// WHICH SCREEN, and this is the half that is NOT macOS-only. The plugin's
// built-in screen is 800x600, and `QWidget::restoreGeometry()` clamps a
// restored window to the screen it lands on, so a test that saves a 1000-wide
// window gets 798 back. `configfile=` names a 1920x1080 description instead.
// A caller who sets `QT_QPA_PLATFORM=offscreen` by hand -- the obvious thing
// on a headless runner, and the whole of backlog 799 -- would otherwise win
// over that and silently bring the small screen with them, so an unadorned
// `offscreen` is upgraded to carry the screen description rather than being
// left alone. Measured 2026-09-20: four Designer tests failed under a bare
// `offscreen` and pass under the upgraded one.
//
// The upgrade is deliberately exact. `cocoa`, `minimal`, and an `offscreen`
// that already carries options are all left untouched, so
// `QT_QPA_PLATFORM=offscreen:configfile=<your own json>` is how you ask for a
// different screen -- including the plugin's own 800x600, by writing a file
// that says so. What is no longer reachable is the built-in default under the
// bare name, which nothing in this tree wanted.
//
// The screen description is a source-tree path baked in at configure time,
// and the plugin refuses to start when the file is not there -- measured
// 2026-09-07: `Could not find platform config file <path>` and the process
// aborts. So look before naming it: a binary run after its source tree moved
// falls back to the plugin's built-in 800x600 screen, still headless, rather
// than to that abort.
//
// Windows is left out of both halves. It keeps its native platform by design,
// and doing the screen half there would mean `_putenv_s`/`_access` in a
// header no one can compile from the machines this is developed on.
inline void DefaultToOffscreenPlatform() {
#if !defined(_WIN32) && defined(SCADA_QT_OFFSCREEN_PLATFORM_CONFIG)
  const bool have_screen =
      access(SCADA_QT_OFFSCREEN_PLATFORM_CONFIG, R_OK) == 0;
  const char* const with_screen =
      "offscreen:configfile=" SCADA_QT_OFFSCREEN_PLATFORM_CONFIG;

#if defined(__APPLE__)
  setenv("QT_QPA_PLATFORM", have_screen ? with_screen : "offscreen",
         /*overwrite=*/0);
#endif

  // Whatever the platform ended up being -- chosen above, or by the caller --
  // an unadorned `offscreen` gets the screen description attached.
  const char* const chosen = getenv("QT_QPA_PLATFORM");
  if (have_screen && chosen != nullptr &&
      std::strcmp(chosen, "offscreen") == 0) {
    setenv("QT_QPA_PLATFORM", with_screen, /*overwrite=*/1);
  }
#endif
}

}  // namespace scada::qt_test
