#include "core/cancellation.h"

#include "gtest/gtest.h"

namespace slop {
namespace {

TEST(CancellationRequestTest, InitialState) {
  CancellationRequest req;
  EXPECT_FALSE(req.IsCancelled());
}

TEST(CancellationRequestTest, Cancel) {
  CancellationRequest req;
  req.Cancel();
  EXPECT_TRUE(req.IsCancelled());
}

TEST(CancellationRequestTest, MultipleCancel) {
  CancellationRequest req;
  req.Cancel();
  req.Cancel();
  EXPECT_TRUE(req.IsCancelled());
}

TEST(CancellationRequestTest, RegisterCallbackBeforeCancel) {
  CancellationRequest req;
  bool called = false;
  req.RegisterCallback([&] { called = true; });
  EXPECT_FALSE(called);
  req.Cancel();
  EXPECT_TRUE(called);
}

TEST(CancellationRequestTest, RegisterCallbackAfterCancel) {
  CancellationRequest req;
  req.Cancel();
  bool called = false;
  req.RegisterCallback([&] { called = true; });
  EXPECT_TRUE(called);
}

TEST(CancellationRequestTest, MultipleCallbacks) {
  CancellationRequest req;
  int count = 0;
  req.RegisterCallback([&] { count++; });
  req.RegisterCallback([&] { count++; });
  req.Cancel();
  EXPECT_EQ(count, 2);
}

}  // namespace
}  // namespace slop
