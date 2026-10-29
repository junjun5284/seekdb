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

static int build_json(ObIAllocator &allocator, const char *text, ObIJsonBase *&json_base)
{
  return ObJsonBaseFactory::get_json_base(&allocator, ObString(text),
                                           ObJsonInType::JSON_TREE,
                                           ObJsonInType::JSON_TREE,
                                           json_base);
}

static std::string to_string(ObIAllocator &allocator, ObIJsonBase *json_base)
{
  if (OB_ISNULL(json_base)) {
    return std::string();
  }
  ObStringBuffer buffer(&allocator);
  int ret = OB_SUCCESS;
  if (OB_FAIL(json_base->print(buffer, 0))) {
    return std::string();
  }
  return std::string(buffer.string().ptr(), buffer.string().length());
}

TEST(TestObOpenAIUtils, complete_reads_message_content)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"}}]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::ObOpenAIComplete::extract_choice_content(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("\"hi\"", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, complete_reads_legacy_text)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"choices\":[{\"text\":\"legacy\"}]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::ObOpenAIComplete::extract_choice_content(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("\"legacy\"", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, complete_without_choices)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "{\"error\":\"boom\"}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, ObOpenAIUtils::ObOpenAIComplete::extract_choice_content(
      allocator, static_cast<ObJsonObject *>(response), result));
}

TEST(TestObOpenAIUtils, embed_restores_out_of_order_response)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  // The second vector arrives first, the index tells which input it belongs to.
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"data\":[{\"index\":1,\"embedding\":[2,2]},{\"index\":0,\"embedding\":[1,1]}]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::ObOpenAIEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("[[1,1],[2,2]]", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, embed_keeps_request_order_without_index)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"data\":[{\"embedding\":[1,1]},{\"embedding\":[2,2]}]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOpenAIUtils::ObOpenAIEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("[[1,1],[2,2]]", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, embed_rejects_out_of_range_index)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"data\":[{\"index\":5,\"embedding\":[1,1]}]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, ObOpenAIUtils::ObOpenAIEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
}

TEST(TestObOpenAIUtils, embed_rejects_duplicate_index)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"data\":[{\"index\":0,\"embedding\":[1,1]},{\"index\":0,\"embedding\":[2,2]}]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, ObOpenAIUtils::ObOpenAIEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
}

TEST(TestObOpenAIUtils, embed_without_data)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "{\"object\":\"list\"}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_INVALID_DATA, ObOpenAIUtils::ObOpenAIEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
}

TEST(TestObOpenAIUtils, ollama_complete_reads_response)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"model\":\"llama3\",\"response\":\"answer\"}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::ObOllamaComplete::extract_response_text(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("\"answer\"", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, ollama_complete_reads_message_content)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"model\":\"llama3\",\"message\":{\"role\":\"assistant\",\"content\":\"chat\"}}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::ObOllamaComplete::extract_response_text(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("\"chat\"", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, ollama_complete_without_content)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "{\"done\":true}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_ERR_UNEXPECTED, ObOllamaUtils::ObOllamaComplete::extract_response_text(
      allocator, static_cast<ObJsonObject *>(response), result));
}

TEST(TestObOpenAIUtils, ollama_embed_reads_embeddings)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"embeddings\":[[1,2],[3,4]]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::ObOllamaEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("[[1,2],[3,4]]", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, ollama_embed_reads_single_embedding)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator,
      "{\"embedding\":[1,2]}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::ObOllamaEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
  ASSERT_EQ("[[1,2]]", to_string(allocator, result));
}

TEST(TestObOpenAIUtils, ollama_embed_without_embeddings)
{
  ObArenaAllocator allocator;
  ObIJsonBase *response = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "{\"done\":true}", response));
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_ERR_UNEXPECTED, ObOllamaUtils::ObOllamaEmbed::extract_embeddings(
      allocator, static_cast<ObJsonObject *>(response), result));
}

TEST(TestObOpenAIUtils, ollama_header_does_not_need_access_key)
{
  ObArenaAllocator allocator;
  ObArray<ObString> headers;
  ObString empty_key;
  ObOllamaUtils::ObOllamaComplete complete;
  ASSERT_EQ(OB_SUCCESS, complete.get_header(allocator, empty_key, headers));
  ASSERT_EQ(1, headers.count());
}

TEST(TestObOpenAIUtils, openai_header_needs_access_key)
{
  ObArenaAllocator allocator;
  ObArray<ObString> headers;
  ObString empty_key;
  ObOpenAIUtils::ObOpenAIComplete complete;
  ASSERT_EQ(OB_INVALID_ARGUMENT, complete.get_header(allocator, empty_key, headers));
}

TEST(TestObOpenAIUtils, provider_requires_access_key)
{
  ASSERT_TRUE(ObAIFuncUtils::is_provider_require_access_key("OPENAI"));
  ASSERT_TRUE(ObAIFuncUtils::is_provider_require_access_key("aliyun-openai"));
  // Ollama runs locally and does not authenticate.
  ASSERT_FALSE(ObAIFuncUtils::is_provider_require_access_key("OLLAMA"));
  ASSERT_FALSE(ObAIFuncUtils::is_provider_require_access_key("ollama"));
}

TEST(TestObOpenAIUtils, provider_names_are_case_insensitive)
{
  ObArenaAllocator allocator;
  ObAIFuncIComplete *complete_provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(
      allocator, ObString("ollama"), complete_provider));
  ASSERT_TRUE(complete_provider != nullptr);

  ObAIFuncIEmbed *embed_provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(
      allocator, ObString("OLLAMA"), embed_provider));
  ASSERT_TRUE(embed_provider != nullptr);
}

TEST(TestObOpenAIUtils, check_not_empty_text)
{
  ObArenaAllocator allocator;
  ObIJsonBase *text = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "\"hello\"", text));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::check_not_empty_text(text, "ai_complete"));

  ObIJsonBase *empty = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "\"\"", empty));
  ASSERT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_not_empty_text(empty, "ai_complete"));

  ObIJsonBase *number = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_json(allocator, "1", number));
  ASSERT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_not_empty_text(number, "ai_complete"));
}

TEST(TestObOpenAIUtils, ollama_completion_body)
{
  ObArenaAllocator allocator;
  ObJsonObject *body = nullptr;
  ObString model("llama3");
  ObString prompt;
  ObString content("hello");
  ObOllamaUtils::ObOllamaComplete complete;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(
      allocator, model, prompt, content, nullptr, body));
  ASSERT_EQ("{\"model\":\"llama3\",\"prompt\":\"hello\"}", to_string(allocator, body));
}

TEST(TestObOpenAIUtils, ollama_embedding_body)
{
  ObArenaAllocator allocator;
  ObJsonObject *body = nullptr;
  ObString model("nomic");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("a")));
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("b")));
  ObOllamaUtils::ObOllamaEmbed embed;
  ASSERT_EQ(OB_SUCCESS, embed.get_body(
      allocator, model, contents, nullptr, body));
  ASSERT_EQ("{\"model\":\"nomic\",\"input\":[\"a\",\"b\"]}", to_string(allocator, body));
}

TEST(TestObOpenAIUtils, openai_embedding_body_carries_dimension)
{
  ObArenaAllocator allocator;
  ObJsonObject *body = nullptr;
  ObJsonObject *config = nullptr;
  ObJsonInt *dimension = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator, config));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_int(allocator, 4, dimension));
  ASSERT_EQ(OB_SUCCESS, config->add("dimensions", dimension));

  ObString model("text-embedding-3-small");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("a")));
  ObOpenAIUtils::ObOpenAIEmbed embed;
  ASSERT_EQ(OB_SUCCESS, embed.get_body(
      allocator, model, contents, config, body));
  ASSERT_EQ("{\"model\":\"text-embedding-3-small\",\"input\":[\"a\"],\"dimensions\":4}",
            to_string(allocator, body));
}

} // namespace common
} // namespace oceanbase
