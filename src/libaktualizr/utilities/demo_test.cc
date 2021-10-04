#include <gtest/gtest.h>

#include "utilities/demo.h"

TEST(Demo, Basic) {
  Demo dut{5};
  EXPECT_EQ(dut.Add(10), 15);
}

#ifndef __NO_MAIN__
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
