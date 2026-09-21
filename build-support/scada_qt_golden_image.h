#pragma once

// Reading and writing golden images for a Qt render test, in the one place
// every product can reach.
//
// It lives in `build-support/` for the same reason
// `scada_qt_offscreen_platform.h` beside it does: the products that need it
// may not include from one another (ADR 0011), and each is a leaf that
// consumes no product at all. `graph_qt` and `view_manager_qt` carried
// byte-identical copies -- differing only in include guard and namespace --
// until 2026-09-20, and so did the 147-line test below them.
//
// That duplication was not theoretical. `SaveGoldenImage` exists because
// QImageWriter opens and truncates its destination before it discovers it has
// no codec, so a run without the PNG plugin left a 0-byte file where a tracked
// baseline was -- which the next run read as "no golden yet" and regenerated
// from whatever the current code rendered. It happened on 2026-08-08 and cost
// two of view_manager_qt's goldens. A safety fix kept in two hand-synced
// copies is exactly the one you do not want drifting.
//
// A product reaches this by putting the kit on its test targets' include path:
// one `include_directories("${SCADA_BUILD_SUPPORT_DIR}")` at the product root,
// which resolves to the tree root in the monorepo and the product root in an
// export. Unlike its neighbour this header DOES use Qt -- that costs nothing,
// because a header is only compiled where it is included, and the four
// products that add the kit to their include path are the Qt ones.
//
// `display` and `designer` are deliberately not consumers. Their golden tests
// write only scratch `actual`/`diff` artifacts and never the baseline, and
// they skip or fail rather than regenerate, so the hazard this contains does
// not arise there.

#include <QFile>
#include <QImage>
#include <QString>

#include <functional>

namespace scada::qt_test {

// Outcome of reading a golden image from disk.
//
// `kAbsent` and `kUnreadable` are deliberately distinct. A missing golden is
// the first-run case a test may regenerate; a golden that exists but does not
// decode means the baseline is damaged — a truncated file, or a build without
// the codec — and regenerating it there would silently replace the reviewed
// baseline with whatever the current code renders.
enum class GoldenLoadResult {
  kLoaded,
  kAbsent,
  kUnreadable,
};

// Reads the golden image at `path` into `image`. `image` is left untouched
// unless the result is `kLoaded`.
inline GoldenLoadResult LoadGoldenImage(const QString& path, QImage& image) {
  if (!QFile::exists(path)) {
    return GoldenLoadResult::kAbsent;
  }

  QImage loaded;
  if (!loaded.load(path)) {
    return GoldenLoadResult::kUnreadable;
  }

  image = std::move(loaded);
  return GoldenLoadResult::kLoaded;
}

// Encodes an image to a path. Injectable into SaveGoldenImage so a test can
// force the failure this API exists to contain.
using GoldenImageWriter = std::function<bool(const QImage&, const QString&)>;

// Returns `path` with `infix` inserted before its extension, so a scratch file
// beside a golden keeps the suffix. QImage::save infers the format from the
// suffix, so a plain "<path>.tmp" would be unencodable.
inline QString InsertGoldenPathInfix(const QString& path,
                                     const QString& infix) {
  const int slash = path.lastIndexOf(u'/');
  const int dot = path.lastIndexOf(u'.');
  if (dot <= slash + 1) {
    return path + infix;
  }
  return path.left(dot) + infix + path.mid(dot);
}

// Writes `image` to `path` without putting an existing file at risk: the
// encode goes to a temporary sibling, and only a fully written file is moved
// into place. Returns false, leaving any existing file as it was, if either
// step fails.
//
// Writing straight onto the golden is what this replaces. QImageWriter opens
// and truncates its destination before it discovers it has no codec for the
// format, so a failed encode over a tracked golden leaves a 0-byte file where
// the baseline was — and the next run reads that as "no golden yet" and
// regenerates one from the current render.
inline bool SaveGoldenImage(const QImage& image,
                            const QString& path,
                            const GoldenImageWriter& writer) {
  const QString temp_path = InsertGoldenPathInfix(path, ".tmp");
  QFile::remove(temp_path);
  if (!writer(image, temp_path)) {
    QFile::remove(temp_path);
    return false;
  }

  // QFile::rename does not overwrite, so an existing golden is moved aside
  // first and put back if the move into place fails.
  const QString backup_path = InsertGoldenPathInfix(path, ".bak");
  QFile::remove(backup_path);
  const bool had_existing = QFile::exists(path);
  if (had_existing && !QFile::rename(path, backup_path)) {
    QFile::remove(temp_path);
    return false;
  }

  if (!QFile::rename(temp_path, path)) {
    if (had_existing) {
      QFile::rename(backup_path, path);
    }
    QFile::remove(temp_path);
    return false;
  }

  QFile::remove(backup_path);
  return true;
}

// Writes `image` to `path` as above, encoding with QImage::save.
inline bool SaveGoldenImage(const QImage& image, const QString& path) {
  return SaveGoldenImage(image, path,
                         [](const QImage& to_write, const QString& to) {
                           return to_write.save(to);
                         });
}

}  // namespace scada::qt_test
