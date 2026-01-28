#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "sqlite3.h"

#include <gtest/gtest.h>
TEST(SqliteTest, CanOpenAndQueryVersion) {
  sqlite3* db;
  int rc = sqlite3_open(":memory:", &db);
  ASSERT_EQ(rc, SQLITE_OK);
  sqlite3_close(db);
}
