// Copyright 2015 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "starboard/common/string.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace starboard {
namespace nplb {
namespace {

TEST(PosixCompareNoCaseNTest, SunnyDaySelf) {
  const char kString[] = "0123456789";
  EXPECT_EQ(0, strncasecmp(kString, kString, strlen(kString)));
  EXPECT_EQ(0, strncasecmp("", "", 0));
}

TEST(PosixCompareNoCaseNTest, SunnyDayEmptyLessThanNotEmpty) {
  const char kString[] = "0123456789";
  EXPECT_GT(0, strncasecmp("", kString, strlen(kString)));
}

TEST(PosixCompareNoCaseNTest, SunnyDayEmptyZeroNEqual) {
  const char kString[] = "0123456789";
  EXPECT_EQ(0, strncasecmp("", kString, 0));
}

TEST(PosixCompareNoCaseNTest, SunnyDayBigN) {
  const char kString[] = "0123456789";
  EXPECT_EQ(0, strncasecmp(kString, kString, strlen(kString) * 2));
}

TEST(PosixCompareNoCaseNTest, SunnyDayCase) {
  const char kString1[] = "aBcDeFgHiJkLmNoPqRsTuVwXyZ";
  const char kString2[] = "AbCdEfGhIjKlMnOpQrStUvWxYz";
  EXPECT_EQ(0, strncasecmp(kString1, kString2, strlen(kString1)));
  EXPECT_EQ(0, strncasecmp(kString2, kString1, strlen(kString2)));

  const char kString3[] = "aBcDeFgHiJkLmaBcDeFgHiJkLm";
  const char kString4[] = "AbCdEfGhIjKlMnOpQrStUvWxYz";
  EXPECT_GT(0, strncasecmp(kString3, kString4, strlen(kString3)));
  EXPECT_LT(0, strncasecmp(kString4, kString3, strlen(kString4)));
  EXPECT_EQ(0, strncasecmp(kString3, kString4, strlen(kString3) / 2));
  EXPECT_EQ(0, strncasecmp(kString4, kString3, strlen(kString4) / 2));
}

}  // namespace
}  // namespace nplb
}  // namespace starboard
