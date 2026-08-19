/**
 * @file tests/unit/platform/test_x11grab.cpp
 * @brief Tests for X11 capture output selection helpers.
 */

#if defined(__linux__)

  // lib includes
  #include <gtest/gtest.h>
  #include <X11/extensions/Xrandr.h>

  // local includes
  #include "src/platform/linux/x11grab.h"

TEST(X11GrabTest, SelectsOnlyConnectedOutputsWithAnActiveCrtc) {
  EXPECT_TRUE(platf::x11::output_is_usable(RR_Connected, 1));
  EXPECT_FALSE(platf::x11::output_is_usable(RR_Disconnected, 1));
  EXPECT_FALSE(platf::x11::output_is_usable(RR_UnknownConnection, 1));
  EXPECT_FALSE(platf::x11::output_is_usable(RR_Connected, None));
}

#endif  // defined(__linux__)
