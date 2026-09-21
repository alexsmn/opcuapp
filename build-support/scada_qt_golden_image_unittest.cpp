// The test for `scada_qt_golden_image.h` beside it. One copy, compiled by
// every product that uses the header, so each still verifies it in its own
// build while there is a single place to change. It was 147 duplicated lines
// in `graph_qt` and `view_manager_qt` until 2026-09-20.

#include "scada_qt_golden_image.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

namespace scada::qt_test {
namespace {

// Bitmap is handled by QtGui itself, so a round trip through it works in a
// build without the PNG codec — which is exactly the build these tests are
// about.
constexpr const char* kFormat = ".bmp";

QByteArray ReadFile(const QString& path) {
  QFile file{path};
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return file.readAll();
}

void WriteFile(const QString& path, const QByteArray& content) {
  QFile file{path};
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  ASSERT_EQ(file.write(content), content.size());
}

// A writer that reproduces what QImageWriter does when it has no codec for the
// format: it opens the destination — truncating it — and only then fails.
bool TruncatingFailingWriter(const QImage&, const QString& path) {
  QFile file{path};
  EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  return false;
}

class GoldenImageTest : public ::testing::Test {
 protected:
  QString Path(const QString& name) const {
    return temp_dir_.filePath(name + kFormat);
  }

  QImage MakeImage(QRgb color) const {
    QImage image{4, 4, QImage::Format_RGB32};
    image.fill(color);
    return image;
  }

  QTemporaryDir temp_dir_;
};

// The scratch file has to keep the golden's suffix: QImage::save picks its
// encoder from the suffix, so writing to "<golden>.tmp" cannot be encoded at
// all and every save would fail.
TEST(GoldenImagePathTest, InfixGoesBeforeTheExtension) {
  EXPECT_EQ(InsertGoldenPathInfix("/data/basic_graph.png", ".tmp"),
            "/data/basic_graph.tmp.png");
  EXPECT_EQ(InsertGoldenPathInfix("/data.v2/basic_graph.png", ".tmp"),
            "/data.v2/basic_graph.tmp.png");
  EXPECT_EQ(InsertGoldenPathInfix("/data.v2/golden", ".tmp"),
            "/data.v2/golden.tmp");
  EXPECT_EQ(InsertGoldenPathInfix("/data/.hidden", ".tmp"),
            "/data/.hidden.tmp");
}

TEST_F(GoldenImageTest, ReportsAbsentWhenNoFileExists) {
  QImage image;
  EXPECT_EQ(LoadGoldenImage(Path("missing"), image), GoldenLoadResult::kAbsent);
}

TEST_F(GoldenImageTest, ReportsUnreadableWhenFileIsEmpty) {
  const QString path = Path("zeroed");
  WriteFile(path, {});

  QImage image;
  EXPECT_EQ(LoadGoldenImage(path, image), GoldenLoadResult::kUnreadable);
}

TEST_F(GoldenImageTest, ReportsUnreadableWhenFileIsNotAnImage) {
  const QString path = Path("garbage");
  WriteFile(path, QByteArray{"this is not an image"});

  QImage image;
  EXPECT_EQ(LoadGoldenImage(path, image), GoldenLoadResult::kUnreadable);
}

TEST_F(GoldenImageTest, LoadsAnImageThatWasSaved) {
  const QString path = Path("round_trip");
  ASSERT_TRUE(SaveGoldenImage(MakeImage(qRgb(10, 20, 30)), path));

  QImage image;
  ASSERT_EQ(LoadGoldenImage(path, image), GoldenLoadResult::kLoaded);
  EXPECT_EQ(image.size(), QSize(4, 4));
  EXPECT_EQ(image.pixel(0, 0), qRgb(10, 20, 30));
}

// The regression this API exists for: a golden that cannot be encoded must
// leave the tracked baseline exactly as it was.
TEST_F(GoldenImageTest, FailedSaveLeavesAnExistingFileIntact) {
  const QString path = Path("existing");
  ASSERT_TRUE(SaveGoldenImage(MakeImage(qRgb(1, 2, 3)), path));
  const QByteArray original = ReadFile(path);
  ASSERT_FALSE(original.isEmpty());

  EXPECT_FALSE(
      SaveGoldenImage(MakeImage(qRgb(4, 5, 6)), path, TruncatingFailingWriter));

  EXPECT_EQ(ReadFile(path), original);
}

TEST_F(GoldenImageTest, FailedSaveLeavesNoTemporaryFiles) {
  const QString path = Path("temporaries");
  ASSERT_TRUE(SaveGoldenImage(MakeImage(qRgb(1, 2, 3)), path));

  EXPECT_FALSE(
      SaveGoldenImage(MakeImage(qRgb(4, 5, 6)), path, TruncatingFailingWriter));

  EXPECT_FALSE(QFile::exists(InsertGoldenPathInfix(path, ".tmp")));
  EXPECT_FALSE(QFile::exists(InsertGoldenPathInfix(path, ".bak")));
}

TEST_F(GoldenImageTest, FailedSaveCreatesNothingWhenNoFileExisted) {
  const QString path = Path("never_written");

  EXPECT_FALSE(
      SaveGoldenImage(MakeImage(qRgb(4, 5, 6)), path, TruncatingFailingWriter));

  EXPECT_FALSE(QFile::exists(path));
  EXPECT_FALSE(QFile::exists(InsertGoldenPathInfix(path, ".tmp")));
}

TEST_F(GoldenImageTest, SuccessfulSaveReplacesAnExistingFile) {
  const QString path = Path("replaced");
  ASSERT_TRUE(SaveGoldenImage(MakeImage(qRgb(1, 2, 3)), path));
  ASSERT_TRUE(SaveGoldenImage(MakeImage(qRgb(200, 100, 50)), path));

  QImage image;
  ASSERT_EQ(LoadGoldenImage(path, image), GoldenLoadResult::kLoaded);
  EXPECT_EQ(image.pixel(0, 0), qRgb(200, 100, 50));
  EXPECT_FALSE(QFile::exists(InsertGoldenPathInfix(path, ".tmp")));
  EXPECT_FALSE(QFile::exists(InsertGoldenPathInfix(path, ".bak")));
}

}  // namespace
}  // namespace scada::qt_test
