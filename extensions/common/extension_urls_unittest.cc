// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/common/extension_urls.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/origin.h"

namespace extension_urls {

// The tests in this file hard-code the expected webstore URLs. These are a bit
// of a change-detector test in some ways, but are valuable because:
// a) The webstore URLs *shouldn't* change often, and updating these tests if
//    they do is very cheap.
// b) The construction of the URLs is a bit subtle and we've had tricky bugs
//    in the past (e.g., double slashes or improperly resolved paths). These
//    ensure the construction succeeds properly with the default URL.

// Tests that the URL for the extensions category in the webstore is what
// we expect.
TEST(ExtensionWebstoreURLsTest, GetWebstoreExtensionsCategoryURL) {
  EXPECT_EQ("https://chromewebstore.google.com/category/extensions",
            GetWebstoreExtensionsCategoryURL().spec());
}

// Tests that the URL to get the block status of extensions in the webstore is
// what we expect.
TEST(ExtensionWebstoreURLsTest, GetWebstoreBlockStatusURL) {
  EXPECT_EQ(
      "https://chromewebstore.googleapis.com/v2/items:"
      "batchFetchItemBlockStatusForEnterprise",
      GetWebstoreBlockStatusURL().spec());
}

TEST(ExtensionWebstoreURLsTest, XlStoreTrustRequiresExactHttpsOrigin) {
  const GURL xl("https://xl.extensions.test.com/detail/test");
  EXPECT_TRUE(IsXlWebstoreOrigin(url::Origin::Create(xl)));
  EXPECT_TRUE(IsWebstoreDomain(xl));
  EXPECT_TRUE(IsWebstoreOrigin(url::Origin::Create(xl)));
  for (const char* spec : {"http://xl.extensions.test.com/",
                           "https://xl.extensions.test.com:8443/",
                           "https://sub.xl.extensions.test.com/",
                           "https://xl.extensions.test.com.evil.test/"}) {
    const GURL url(spec);
    EXPECT_FALSE(IsXlWebstoreOrigin(url::Origin::Create(url))) << spec;
    EXPECT_FALSE(IsWebstoreDomain(url)) << spec;
    EXPECT_FALSE(IsWebstoreOrigin(url::Origin::Create(url))) << spec;
  }
  EXPECT_TRUE(IsWebstoreOrigin(
      url::Origin::Create(GURL("https://chromewebstore.google.com/"))));
}

TEST(ExtensionWebstoreURLsTest, XlXmlUpdatesRemainSeparateFromGoogleProtocol) {
  const GURL xl("https://xl.extensions.test.com/updates/test.xml?x=version");
  EXPECT_TRUE(IsXlWebstoreUpdateUrl(xl));
  EXPECT_FALSE(IsWebstoreUpdateUrl(xl));
  EXPECT_FALSE(IsXlWebstoreUpdateUrl(
      GURL("http://xl.extensions.test.com/updates/test.xml")));
  EXPECT_FALSE(IsXlWebstoreUpdateUrl(GetWebstoreUpdateUrl()));
}

}  // namespace extension_urls
