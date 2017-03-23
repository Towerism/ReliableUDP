#include <gtest/gtest.h>

#include <stub.h>

TEST(StubTests, OneEqualsOne)
{
  Stub::DoStuff();
  EXPECT_EQ(1, 1);
}
