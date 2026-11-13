// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_jump_list.h"

#include "base/json/json_reader.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
TEST(ElectronJumpListTest, ValidatesTheWholeTransactionBeforeTouchingTheShell) {
  auto valid = base::JSONReader::Read(R"([
    {"type":"recent"}, {"type":"tasks", "items":[
      {"type":"task", "title":"New", "program":"C:\\app.exe",
       "args":"--new-window", "iconIndex":0}, {"type":"separator"}]},
    {"name":"Documents", "items":[{"type":"file","path":"C:\\文档.txt"}]}
  ])",
                                      base::JSON_PARSE_RFC);
  ASSERT_TRUE(valid);
  EXPECT_TRUE(ValidateElectronJumpList(*valid).empty());
  valid->GetList().Append(base::DictValue().Set("type", "unknown"));
  EXPECT_NE(ValidateElectronJumpList(*valid).find("ERR_INVALID_ARG_VALUE"),
            std::string::npos);
}

TEST(ElectronJumpListTest, RejectsInvalidNativeStringsAndItemTypes) {
  for (
      const char* source :
      {R"([{"type":7}])", R"([{"name":""}])",
       R"([{"type":"tasks","items":[{"type":"file","path":"a\u0000b"}]}])",
       R"([{"type":"tasks","items":[{"type":"task","title":"New"}]}])",
       R"([{"type":"tasks","items":[{"type":"separator","iconIndex":1.5}]}])"}) {
    auto value = base::JSONReader::Read(source, base::JSON_PARSE_RFC);
    ASSERT_TRUE(value);
    EXPECT_FALSE(ValidateElectronJumpList(*value).empty()) << source;
  }
}

TEST(ElectronJumpListTest, ClearingAndEmptyListsAreDistinctValidRequests) {
  EXPECT_TRUE(ValidateElectronJumpList(base::Value()).empty());
  EXPECT_TRUE(ValidateElectronJumpList(base::Value(base::ListValue())).empty());
  EXPECT_FALSE(ValidateElectronJumpList(base::Value(false)).empty());
}
}  // namespace xenon::ipc
