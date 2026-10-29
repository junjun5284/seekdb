/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cstring>
#include <gtest/gtest.h>
#include "lib/allocator/page_arena.h"
#include "share/json/ob_json_helper.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{
using namespace sql;

// Builds a JSON document from a literal so that the test does not depend on
// the parser.
static int build_json(ObIAllocator &allocator, const char *text, ObIJsonBase *&json_base)
{
  int ret = OB_SUCCESS;
  ObString json_text(text);
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, json_text,
                                                ObJsonInType::JSON_TREE,
                                                ObJsonInType::JSON_TREE,
                                                json_base))) {
  } else {
    ret = OB_SUCCESS;
  }
  return ret;
}

// Replaces the placeholders of a template with the arguments of the array.
static int replace(ObIAllocator &allocator,
                   const char *template_text,
                   const char *args_json,
                   std::string &result)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *template_base = nullptr;
  ObIJsonBase *args_base = nullptr;
  ObJsonArray *args_array = nullptr;
  ObString replaced;
  if (OB_FAIL(build_json(allocator, template_text, template_base))) {
  } else if (OB_FAIL(build_json(allocator, args_json, args_base))) {
  } else if (args_base->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else {
    args_array = static_cast<ObJsonArray *>(args_base);
    ObString template_str = static_cast<ObJsonString *>(template_base)->get_str();
    if (OB_FAIL(ObAIFuncPromptObjectUtils::replace_placeholders(
            allocator, template_str, args_array, replaced))) {
    } else {
      result.assign(replaced.ptr(), replaced.length());
    }
  }
  return ret;
}

TEST(TestObExprAIPrompt, replace_single_placeholder)
{
  ObArenaAllocator allocator;
  std::string result;
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"1+1=?\"", "[\"2\"]", result));
  ASSERT_EQ("1+1=2", result);
}

TEST(TestObExprAIPrompt, replace_repeated_placeholder)
{
  ObArenaAllocator allocator;
  std::string result;
  // The same argument is referenced twice.
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{0}+{0}\"", "[\"x\"]", result));
  ASSERT_EQ("x+x", result);
}

TEST(TestObExprAIPrompt, replace_multiple_placeholders)
{
  ObArenaAllocator allocator;
  std::string result;
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{0}+{1}={2}\"",
                                "[\"1\",\"2\",\"3\"]", result));
  ASSERT_EQ("1+2=3", result);
}

TEST(TestObExprAIPrompt, replace_keeps_non_placeholder_text)
{
  ObArenaAllocator allocator;
  std::string result;
  // A brace that does not introduce a placeholder stays as it is.
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{a} and {0}\"", "[\"v\"]", result));
  ASSERT_EQ("{a} and v", result);
}

TEST(TestObExprAIPrompt, replace_with_json_object_argument)
{
  ObArenaAllocator allocator;
  std::string result;
  // A JSON argument keeps its JSON representation.
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{0}\"", "[{\"a\":1}]", result));
  ASSERT_EQ("{\"a\":1}", result);
}

TEST(TestObExprAIPrompt, replace_with_json_array_argument)
{
  ObArenaAllocator allocator;
  std::string result;
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{0}\"", "[[1,2]]", result));
  ASSERT_EQ("[1,2]", result);
}

TEST(TestObExprAIPrompt, replace_with_number_argument)
{
  ObArenaAllocator allocator;
  std::string result;
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{0}\"", "[42]", result));
  ASSERT_EQ("42", result);
}

TEST(TestObExprAIPrompt, replace_with_unicode_argument)
{
  ObArenaAllocator allocator;
  std::string result;
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{0}\"", "[\"\\u4e16\\u754c\"]", result));
  ASSERT_EQ("\xe4\xb8\x96\xe7\x95\x8c", result);
}

TEST(TestObExprAIPrompt, replace_with_escaped_argument)
{
  ObArenaAllocator allocator;
  std::string result;
  ASSERT_EQ(OB_SUCCESS, replace(allocator, "\"{0}\"", "[\"a\\\"b\"]", result));
  ASSERT_EQ("a\"b", result);
}

TEST(TestObExprAIPrompt, replace_invalid_placeholder_index)
{
  ObArenaAllocator allocator;
  std::string result;
  // The template refers to an argument that does not exist.
  ASSERT_EQ(OB_INVALID_ARGUMENT, replace(allocator, "\"{1}\"", "[\"x\"]", result));
}

TEST(TestObExprAIPrompt, replace_without_arguments)
{
  ObArenaAllocator allocator;
  std::string result;
  ASSERT_EQ(OB_INVALID_ARGUMENT, replace(allocator, "\"{0}\"", "[]", result));
}

TEST(TestObExprAIPrompt, construct_and_split_prompt_object)
{
  ObArenaAllocator allocator;
  ObJsonObject *prompt_object = nullptr;
  ObJsonArray *args_array = nullptr;
  ObString template_str;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator, args_array));
  {
    ObJsonString *arg_node = nullptr;
    ObString arg_text("a");
    ASSERT_EQ(OB_SUCCESS,
              ObAIFuncJsonUtils::get_json_string(allocator, arg_text, arg_node));
    ASSERT_EQ(OB_SUCCESS, args_array->append(arg_node));
    ObString template_text("hello {0}");
    ASSERT_EQ(OB_SUCCESS,
              ObAIFuncPromptObjectUtils::construct_prompt_object(
                  allocator, template_text, args_array, prompt_object));
    ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
    ASSERT_EQ(OB_SUCCESS,
              ObAIFuncPromptObjectUtils::split_prompt_object(
                  prompt_object, template_str, args_array));
    ASSERT_EQ("hello {0}", template_str.ptr());
    ASSERT_EQ(1, args_array->element_count());
  }
}

TEST(TestObExprAIPrompt, invalid_prompt_object)
{
  ObArenaAllocator allocator;
  ObIJsonBase *json_base = nullptr;
  // The template has to be a string and the arguments an array.
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "{\"template\":1,\"args\":[]}", json_base));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(
      static_cast<ObJsonObject *>(json_base)));

  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "{\"template\":\"t\"}", json_base));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(
      static_cast<ObJsonObject *>(json_base)));

  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "{\"template\":\"t\",\"args\":\"a\"}", json_base));
  ASSERT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(
      static_cast<ObJsonObject *>(json_base)));
}

TEST(TestObExprAIPrompt, prompt_object_with_json_arguments)
{
  ObArenaAllocator allocator;
  ObIJsonBase *json_base = nullptr;
  // A JSON object argument is a valid argument of a prompt.
  ASSERT_EQ(OB_SUCCESS,
            build_json(allocator, "{\"template\":\"t\",\"args\":[{\"a\":1}]}", json_base));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(
      static_cast<ObJsonObject *>(json_base)));
}

TEST(TestObExprAIPrompt, prompt_object_with_nested_arguments)
{
  ObArenaAllocator allocator;
  ObIJsonBase *json_base = nullptr;
  ASSERT_EQ(OB_SUCCESS,
            build_json(allocator, "{\"template\":\"t\",\"args\":[[1,2]]}", json_base));
  ASSERT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(
      static_cast<ObJsonObject *>(json_base)));
}

} // namespace common
} // namespace oceanbase
