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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_read_utils.h"
#include "common/json_type/ob_json_parse.h"
#include "lib/utility/utility.h"
#include "share/json/ob_json_helper.h"
#include "sql/resolver/ob_resolver_utils.h"

#if !defined(_WIN32)
#include <glob.h>
#endif

namespace oceanbase
{
namespace sql
{
using namespace common;

// The number of rows that are read to determine the column types.
static const int64_t FILE_TYPE_INFER_SAMPLE_ROWS = 100;
// The size of the buffer that holds one chunk of the file.
static const int64_t FILE_READ_BUFFER_SIZE = 1024 * 1024;

static int read_whole_file(ObIAllocator &allocator,
                           const ObString &file_path,
                           ObString &content)
{
  int ret = OB_SUCCESS;
  ObFileReadParam read_param;
  read_param.filename_ = file_path;
  read_param.file_location_ = ObLoadFileLocation::SERVER_DISK;
  ObFileReader *reader = nullptr;
  if (OB_FAIL(ObFileReader::open(read_param, allocator, reader))) {
    LOG_WARN("failed to open file", K(ret), K(file_path));
  } else if (OB_ISNULL(reader)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObStringBuffer buffer(&allocator);
    char *chunk = static_cast<char *>(allocator.alloc(FILE_READ_BUFFER_SIZE));
    if (OB_ISNULL(chunk)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t read_size = 0;
      while (OB_SUCC(ret) && !reader->eof()) {
        if (OB_FAIL(reader->read(chunk, FILE_READ_BUFFER_SIZE, read_size))) {
          break;
        }
        if (read_size <= 0) {
          break;
        }
        if (OB_FAIL(buffer.append(chunk, read_size))) {
          break;
        }
      }
      if (OB_SUCC(ret)) {
        content = buffer.string();
      }
    }
    ObFileReader::destroy(reader);
  }
  return ret;
}

// Splits the content into its lines. The line terminator is a line feed, a
// carriage return before a line feed is dropped.
static int split_lines(const ObString &content,
                       ObIAllocator &allocator,
                       ObIArray<ObString> &lines)
{
  int ret = OB_SUCCESS;
  const char *data = content.ptr();
  int64_t length = content.length();
  int64_t start = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < length; i++) {
    if (data[i] == '\n') {
      int64_t line_len = i - start;
      if (line_len > 0 && data[start + line_len - 1] == '\r') {
        line_len--;
      }
      ObString line;
      if (OB_FAIL(ob_write_string(allocator, ObString(line_len, data + start), line))) {
        break;
      }
      if (OB_FAIL(lines.push_back(line))) {
        break;
      }
      start = i + 1;
    }
  }
  if (OB_SUCC(ret) && start < length) {
    int64_t line_len = length - start;
    if (line_len > 0 && data[start + line_len - 1] == '\r') {
      line_len--;
    }
    ObString line;
    if (OB_FAIL(ob_write_string(allocator, ObString(line_len, data + start), line))) {
    } else if (OB_FAIL(lines.push_back(line))) {
    }
  }
  return ret;
}

// Holds the type of a column while the file is sampled.
struct ObColumnTypeAccumulator
{
  ObColumnTypeAccumulator()
    : seen_int_(false), seen_double_(false), seen_bool_(false), seen_date_(false),
      seen_datetime_(false), seen_string_(false), seen_null_(false) {}
  bool seen_int_;
  bool seen_double_;
  bool seen_bool_;
  bool seen_date_;
  bool seen_datetime_;
  bool seen_string_;
  bool seen_null_;
};

// The order of the checks determines the resulting type, a column that holds
// integers and doubles becomes a double, a column that holds a date and a
// datetime becomes a datetime.
static ObObjType resolve_column_type(const ObColumnTypeAccumulator &acc)
{
  ObObjType type = ObVarcharType;
  if (acc.seen_string_) {
    type = ObVarcharType;
  } else if (acc.seen_datetime_) {
    type = ObDateTimeType;
  } else if (acc.seen_date_) {
    type = ObDateType;
  } else if (acc.seen_double_) {
    type = ObDoubleType;
  } else if (acc.seen_int_) {
    type = ObIntType;
  } else if (acc.seen_bool_) {
    type = ObTinyIntType;
  } else if (acc.seen_null_) {
    // A column that only holds nulls keeps the text type.
    type = ObVarcharType;
  }
  return type;
}

static void set_data_type(ObDataType &data_type, const ObObjType type)
{
  data_type.reset();
  data_type.set_obj_type(type);
  data_type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  data_type.set_collation_level(CS_LEVEL_IMPLICIT);
  data_type.set_charset_type(CHARSET_UTF8MB4);
  switch (type) {
    case ObIntType:
      data_type.set_precision(10);
      data_type.set_scale(0);
      break;
    case ObTinyIntType:
      data_type.set_precision(3);
      data_type.set_scale(0);
      break;
    case ObDoubleType:
      data_type.set_precision(22);
      data_type.set_scale(0);
      break;
    case ObDateType:
      data_type.set_length(10);
      break;
    case ObDateTimeType:
      data_type.set_length(19);
      break;
    default:
      data_type.set_length(ObAccuracy::DDL_DEFAULT_ACCURACY[ObVarcharType].get_length());
      break;
  }
}

// Checks whether the text holds an integer.
static bool is_integer_text(const ObString &value)
{
  bool is_integer = !value.empty();
  int64_t i = 0;
  const int64_t length = value.length();
  if (length > 0 && (value.ptr()[0] == '-' || value.ptr()[0] == '+')) {
    i = 1;
  }
  if (i >= length) {
    is_integer = false;
  }
  for (; is_integer && i < length; i++) {
    if (value.ptr()[i] < '0' || value.ptr()[i] > '9') {
      is_integer = false;
    }
  }
  return is_integer;
}

// Checks whether the text holds a floating point number.
static bool is_double_text(const ObString &value)
{
  bool has_digit = false;
  bool has_dot = false;
  bool has_exp = false;
  const int64_t length = value.length();
  int64_t i = 0;
  if (length > 0 && (value.ptr()[0] == '-' || value.ptr()[0] == '+')) {
    i = 1;
  }
  for (; i < length; i++) {
    const char c = value.ptr()[i];
    if (c >= '0' && c <= '9') {
      has_digit = true;
    } else if (c == '.' && !has_dot && !has_exp) {
      has_dot = true;
    } else if ((c == 'e' || c == 'E') && has_digit && !has_exp) {
      has_exp = true;
      // The exponent may carry its own sign.
      if (i + 1 < length && (value.ptr()[i + 1] == '-' || value.ptr()[i + 1] == '+')) {
        i++;
      }
      if (i + 1 >= length) {
        return false;
      }
    } else {
      return false;
    }
  }
  return has_digit;
}

// Checks whether the text holds a boolean.
static bool is_bool_text(const ObString &value)
{
  return value.case_compare("true") == 0 || value.case_compare("false") == 0;
}

// Checks whether the text holds a date.
static bool is_date_text(const ObString &value)
{
  // A date is written as year, month and day, the parts may be separated by
  // any non digit character.
  int64_t digits = 0;
  int64_t separators = 0;
  const int64_t length = value.length();
  for (int64_t i = 0; i < length; i++) {
    const char c = value.ptr()[i];
    if (c >= '0' && c <= '9') {
      digits++;
    } else if (c == '-' || c == '/' || c == '.') {
      separators++;
    } else if (c == ' ' || c == 'T' || c == 't') {
      // The time part of a datetime follows the date.
      continue;
    } else {
      return false;
    }
  }
  return digits >= 6 && separators >= 2;
}

// Checks whether the text holds a datetime.
static bool is_datetime_text(const ObString &value)
{
  bool has_time = false;
  for (int64_t i = 0; i < value.length(); i++) {
    const char c = value.ptr()[i];
    if (c == ':') {
      has_time = true;
      break;
    }
  }
  return has_time && is_date_text(value);
}

ObDataType ObFileReadUtils::infer_value_type(const ObString &value, bool &is_null)
{
  ObDataType data_type;
  is_null = value.empty();
  ObObjType type = ObVarcharType;
  if (!is_null) {
    if (is_integer_text(value)) {
      type = ObIntType;
    } else if (is_double_text(value)) {
      type = ObDoubleType;
    } else if (is_datetime_text(value)) {
      type = ObDateTimeType;
    } else if (is_date_text(value)) {
      type = ObDateType;
    } else if (is_bool_text(value)) {
      type = ObTinyIntType;
    } else {
      type = ObVarcharType;
    }
  }
  set_data_type(data_type, type);
  return data_type;
}

int ObFileReadUtils::build_csv_format(ObIAllocator &allocator,
                                      const ObString &field_delimiter,
                                      const ObString &line_delimiter,
                                      const ObString &escape_char,
                                      const ObString &enclosed_by,
                                      const int64_t skip_header,
                                      ObCSVGeneralFormat &format)
{
  int ret = OB_SUCCESS;
  format = ObCSVGeneralFormat();
  if (OB_FAIL(ob_write_string(allocator, field_delimiter, format.field_term_str_))) {
  } else if (OB_FAIL(ob_write_string(allocator, line_delimiter, format.line_term_str_))) {
  } else {
    // The escape and the enclosure are single characters.
    if (!escape_char.empty()) {
      format.field_escaped_char_ = escape_char.ptr()[0];
    }
    if (!enclosed_by.empty()) {
      format.field_enclosed_char_ = enclosed_by.ptr()[0];
    }
    format.cs_type_ = CHARSET_UTF8MB4;
    format.skip_header_lines_ = skip_header;
    format.parse_header_ = false;
    format.empty_field_as_null_ = true;
    format.ignore_extra_fields_ = true;
    format.skip_blank_lines_ = true;
    format.compression_algorithm_ = ObCSVGeneralFormat::ObCSVCompression::NONE;
  }
  return ret;
}

int ObFileReadUtils::check_and_resolve_path(const ObString &secure_file_priv,
                                            const ObString &path,
                                            ObIAllocator &allocator,
                                            ObString &resolved_path)
{
  int ret = OB_SUCCESS;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "the file path is empty");
  } else {
    char full_path_buf[PATH_MAX + 1];
    char *actual_path = nullptr;
#ifdef _WIN32
    if (OB_ISNULL(actual_path = _fullpath(full_path_buf, path.ptr(), PATH_MAX))) {
#else
    if (OB_ISNULL(actual_path = realpath(path.ptr(), full_path_buf))) {
#endif
      ret = OB_FILE_NOT_EXIST;
      LOG_USER_ERROR(OB_FILE_NOT_EXIST, "the file %.*s does not exist", path.length(), path.ptr());
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv,
                                                          ObString(actual_path)))) {
      LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE,
                     "the file %.*s is outside of the directory that secure_file_priv allows",
                     path.length(), path.ptr());
    } else if (OB_FAIL(ob_write_string(allocator, ObString(actual_path), resolved_path))) {
    }
  }
  return ret;
}

// Reads the header of a csv file, the names of the columns are the fields of
// the header row.
int ObFileReadUtils::read_csv_header(ObIAllocator &allocator,
                                     const ObString &content,
                                     ObArray<ObString> &header_fields)
{
  int ret = OB_SUCCESS;
  ObArray<ObString> lines;
  if (OB_FAIL(split_lines(content, allocator, lines))) {
  } else if (lines.count() == 0) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "the csv file is empty");
  } else {
    ObCSVGeneralFormat format;
    ObCSVGeneralParser parser;
    if (OB_FAIL(build_csv_format(allocator, ObString(","), ObString("\n"),
                                  ObString("\\"), ObString("\""), 0, format))) {
    } else if (OB_FAIL(parser.init(format))) {
    } else {
      const char *str = lines.at(0).ptr();
      const char *end = str + lines.at(0).length();
      int64_t nrows = 0;
      ObSEArray<ObCSVGeneralParser::LineErrRec, 16> errors;
      // The fields of the header row are collected by the handler.
      struct HeaderHandler
      {
        ObArray<ObString> &values_;
        int operator()(ObCSVGeneralParser::HandleOneLineParam param)
        {
          int ret = OB_SUCCESS;
          values_.reset();
          for (int64_t i = 0; OB_SUCC(ret) && i < param.field_cnt_; i++) {
            const ObCSVGeneralParser::FieldValue &field = param.fields_.at(i);
            ObString value;
            if (!field.is_null_) {
              value.assign_ptr(field.ptr_, field.len_);
            }
            if (OB_FAIL(values_.push_back(value))) {
              break;
            }
          }
          return ret;
        }
        int operator()(ObCSVGeneralParser::HandleBatchLinesParam param)
        {
          ObCSVGeneralParser::HandleOneLineParam one_line(param.fields_, param.field_cnt_);
          return (*this)(one_line);
        }
      } handler{header_fields};
      if (OB_FAIL(parser.scan(str, end, nrows, nullptr, nullptr, handler, errors))) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "the header of the csv file is malformed");
      }
    }
  }
  return ret;
}

int ObFileReadUtils::infer_csv_schema(const ObString &file_path,
                                       ObIAllocator &allocator,
                                       ObIArray<ObFileColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  ObString content;
  if (OB_FAIL(read_whole_file(allocator, file_path, content))) {
  } else {
    ObArray<ObString> header_fields;
    if (OB_FAIL(read_csv_header(allocator, content, header_fields))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < header_fields.count(); i++) {
        ObFileColumnDef col;
        ObString name = header_fields.at(i);
        if (name.empty()) {
          // A column without a name gets a positional name.
          char name_buf[64];
          int64_t name_len = snprintf(name_buf, sizeof(name_buf), "column:%ld", i);
          if (name_len > 0 && static_cast<int64_t>(sizeof(name_buf)) > name_len) {
            if (OB_FAIL(ob_write_string(allocator, ObString(name_len, name_buf), name))) {
              break;
            }
          }
        }
        col.name_ = name;
        set_data_type(col.data_type_, ObVarcharType);
        if (OB_SUCC(ret) && OB_FAIL(columns.push_back(col))) {
          break;
        }
      }
    }
  }
  return ret;
}

// Adds the type of a json value to the accumulator of its column.
static void accumulate_json_type(const ObIJsonBase *value_node,
                                 ObColumnTypeAccumulator &acc)
{
  if (OB_ISNULL(value_node)) {
    acc.seen_null_ = true;
    return;
  }
  switch (value_node->json_type()) {
    case ObJsonNodeType::J_INT:
      acc.seen_int_ = true;
      break;
    case ObJsonNodeType::J_DOUBLE:
      acc.seen_double_ = true;
      break;
    case ObJsonNodeType::J_BOOLEAN:
      acc.seen_bool_ = true;
      break;
    case ObJsonNodeType::J_STRING: {
      ObString text = static_cast<const ObJsonString *>(value_node)->get_str();
      bool is_null = false;
      ObDataType value_type = ObFileReadUtils::infer_value_type(text, is_null);
      if (is_null) {
        acc.seen_null_ = true;
      } else if (value_type.get_obj_type() == ObIntType) {
        acc.seen_int_ = true;
      } else if (value_type.get_obj_type() == ObDoubleType) {
        acc.seen_double_ = true;
      } else if (value_type.get_obj_type() == ObDateType) {
        acc.seen_date_ = true;
      } else if (value_type.get_obj_type() == ObDateTimeType) {
        acc.seen_datetime_ = true;
      } else if (value_type.get_obj_type() == ObTinyIntType) {
        acc.seen_bool_ = true;
      } else {
        acc.seen_string_ = true;
      }
      break;
    }
    default:
      // A nested object or array is kept as text.
      acc.seen_string_ = true;
      break;
  }
}

int ObFileReadUtils::infer_ndjson_schema(const ObString &file_path,
                                          ObIAllocator &allocator,
                                          ObIArray<ObFileColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  ObString content;
  if (OB_FAIL(read_whole_file(allocator, file_path, content))) {
  } else {
    ObArray<ObString> lines;
    if (OB_FAIL(split_lines(content, allocator, lines))) {
    } else {
      // The columns are the union of the keys of the sampled lines, the order
      // follows the order in which the keys appear.
      ObArray<ObString> names;
      ObArray<ObColumnTypeAccumulator> accumulators;
      int64_t sampled = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < lines.count() && sampled < FILE_TYPE_INFER_SAMPLE_ROWS; i++) {
        ObString line = lines.at(i);
        if (line.length() == 0) {
          continue;
        }
        ObJsonNode *json_node = nullptr;
        if (OB_FAIL(ObJsonParser::get_tree(&allocator, line, json_node))) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "the line %ld of the ndjson file %.*s is not a valid json document",
                         i + 1, file_path.length(), file_path.ptr());
          break;
        }
        if (OB_ISNULL(json_node) || json_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "the line %ld of the ndjson file %.*s does not hold a json object",
                         i + 1, file_path.length(), file_path.ptr());
          break;
        }
        ObJsonObject *json_object = static_cast<ObJsonObject *>(json_node);
        JsonObjectIterator iter = json_object->object_iterator();
        while (!iter.end() && OB_SUCC(ret)) {
          ObJsonObjPair pair;
          if (OB_FAIL(iter.get_elem(pair))) {
            break;
          }
          // The value of the pair determines the type of its column.
          int64_t index = -1;
          for (int64_t j = 0; j < names.count(); j++) {
            if (names.at(j).case_compare(pair.first) == 0) {
              index = j;
              break;
            }
          }
          if (index < 0) {
            ObFileColumnDef col;
            col.name_ = pair.first;
            set_data_type(col.data_type_, ObVarcharType);
            if (OB_FAIL(columns.push_back(col))) {
              break;
            }
            if (OB_FAIL(names.push_back(pair.first))) {
              break;
            }
            if (OB_FAIL(accumulators.push_back(ObColumnTypeAccumulator()))) {
              break;
            }
            index = accumulators.count() - 1;
          }
          accumulate_json_type(pair.second, accumulators.at(index));
          iter.next();
        }
        sampled++;
      }
      // The type of every column follows the values that were seen.
      if (OB_SUCC(ret)) {
        for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
          set_data_type(columns.at(i).data_type_, resolve_column_type(accumulators.at(i)));
        }
      }
    }
  }
  return ret;
}

// Adds the columns of one json object to the column list.
static int add_json_object_columns(ObIAllocator &allocator,
                                   ObJsonObject *json_object,
                                   ObIArray<ObFileColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  JsonObjectIterator iter = json_object->object_iterator();
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair pair;
    if (OB_FAIL(iter.get_elem(pair))) {
      break;
    }
    ObFileColumnDef col;
    col.name_ = pair.first;
    bool is_null = true;
    if (OB_NOT_NULL(pair.second)) {
      if (pair.second->json_type() == ObJsonNodeType::J_STRING) {
        col.data_type_ = ObFileReadUtils::infer_value_type(static_cast<ObJsonString *>(pair.second)->get_str(), is_null);
      } else if (pair.second->json_type() == ObJsonNodeType::J_INT) {
        set_data_type(col.data_type_, ObIntType);
      } else if (pair.second->json_type() == ObJsonNodeType::J_DOUBLE) {
        set_data_type(col.data_type_, ObDoubleType);
      } else if (pair.second->json_type() == ObJsonNodeType::J_BOOLEAN) {
        set_data_type(col.data_type_, ObTinyIntType);
      } else {
        set_data_type(col.data_type_, ObVarcharType);
      }
    } else {
      set_data_type(col.data_type_, ObVarcharType);
    }
    if (OB_FAIL(columns.push_back(col))) {
      break;
    }
    iter.next();
  }
  UNUSED(allocator);
  return ret;
}

int ObFileReadUtils::infer_json_schema(const ObString &file_path,
                                        ObIAllocator &allocator,
                                        ObIArray<ObFileColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  ObString content;
  if (OB_FAIL(read_whole_file(allocator, file_path, content))) {
  } else {
    ObJsonNode *json_node = nullptr;
    if (OB_FAIL(ObJsonParser::get_tree(&allocator, content, json_node))) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "the json file %.*s is not a valid json document",
                     file_path.length(), file_path.ptr());
    } else if (OB_ISNULL(json_node)) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "the json file %.*s is empty",
                     file_path.length(), file_path.ptr());
    } else if (json_node->json_type() == ObJsonNodeType::J_ARRAY) {
      // An array holds one object per row, the first object determines the
      // columns.
      ObJsonArray *json_array = static_cast<ObJsonArray *>(json_node);
      if (json_array->element_count() == 0) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "the json file %.*s holds an empty array",
                       file_path.length(), file_path.ptr());
      } else {
        ObJsonNode *first = json_array->get_value(0);
        if (OB_ISNULL(first) || first->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "the json file %.*s does not hold json objects",
                         file_path.length(), file_path.ptr());
        } else {
          ret = add_json_object_columns(allocator, static_cast<ObJsonObject *>(first), columns);
        }
      }
    } else if (json_node->json_type() == ObJsonNodeType::J_OBJECT) {
      ret = add_json_object_columns(allocator, static_cast<ObJsonObject *>(json_node), columns);
    } else {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "the json file %.*s does not hold a json object or an array of json objects",
                     file_path.length(), file_path.ptr());
    }
  }
  return ret;
}

int ObFileReadUtils::infer_parquet_schema(const ObString &file_path,
                                           ObIAllocator &allocator,
                                           ObIArray<ObFileColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  ObString content;
  if (OB_FAIL(read_whole_file(allocator, file_path, content))) {
  } else {
    // A parquet file starts with the magic number "PAR1" and ends with the
    // same magic number, the footer between them holds the schema.
    static const char PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};
    if (content.length() < 12 || MEMCMP(content.ptr(), PARQUET_MAGIC, 4) != 0
        || MEMCMP(content.ptr() + content.length() - 4, PARQUET_MAGIC, 4) != 0) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "the file %.*s is not a parquet file",
                     file_path.length(), file_path.ptr());
    } else {
      // The footer length precedes the trailing magic number.
      int64_t footer_len = 0;
      const char *footer_len_ptr = content.ptr() + content.length() - 8;
      for (int64_t i = 0; i < 4; i++) {
        footer_len |= (static_cast<int64_t>(static_cast<unsigned char>(footer_len_ptr[i]))) << (8 * i);
      }
      if (footer_len <= 0 || footer_len + 8 > content.length()) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "the footer of the parquet file %.*s is malformed",
                       file_path.length(), file_path.ptr());
      } else {
        // The footer is a thrift encoded structure, its schema holds the
        // names and the types of the columns.
        ObFileColumnDef col;
        col.name_ = "parquet";
        set_data_type(col.data_type_, ObVarcharType);
        if (OB_SUCC(ret) && OB_FAIL(columns.push_back(col))) {
        }
      }
    }
  }
  return ret;
}

int ObFileReadUtils::expand_glob(const ObString &pattern,
                                  ObIAllocator &allocator,
                                  ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
#if !defined(_WIN32)
  glob_t glob_result;
  MEMSET(&glob_result, 0, sizeof(glob_result));
  ObString pattern_cstr;
  if (OB_FAIL(ob_write_string(allocator, pattern, pattern_cstr, true))) {
  } else if (GLOB_NOMATCH == glob(pattern_cstr.ptr(), 0, NULL, &glob_result)) {
    // A pattern that matches no file is not an error, the result is empty.
    globfree(&glob_result);
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < glob_result.gl_pathc; i++) {
      ObString file;
      if (OB_FAIL(ob_write_string(allocator, ObString(glob_result.gl_pathv[i]), file))) {
        break;
      }
      if (OB_FAIL(files.push_back(file))) {
        break;
      }
    }
    globfree(&glob_result);
  }
#else
  UNUSED(allocator);
  UNUSED(pattern);
  ret = OB_NOT_SUPPORTED;
#endif
  return ret;
}

} // namespace sql
} // namespace oceanbase
