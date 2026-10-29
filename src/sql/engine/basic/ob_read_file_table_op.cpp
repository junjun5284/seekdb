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
#include "ob_read_file_table_op.h"
#include "common/json_type/ob_json_parse.h"
#include "lib/utility/utility.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

// The size of the buffer that holds the part of the file that was read.
static const int64_t READ_FILE_BUFFER_SIZE = 1024 * 1024;

ObReadFileTableOp::ObReadFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
  : ObOperator(exec_ctx, spec, input),
    spec_(nullptr),
    file_reader_(nullptr),
    file_buffer_(&exec_ctx.get_allocator()),
    buffer_offset_(0),
    buffer_size_(0),
    row_values_(),
    row_index_(0),
    batch_row_count_(0),
    is_eof_(false)
{
  spec_ = static_cast<ObReadFileTableSpec *>(&const_cast<ObOpSpec &>(spec));
}

ObReadFileTableOp::~ObReadFileTableOp()
{
  if (OB_NOT_NULL(file_reader_)) {
    ObFileReader::destroy(file_reader_);
    file_reader_ = nullptr;
  }
}

int ObReadFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(open_file())) {
    LOG_WARN("failed to open the file of the table function", K(ret), K(spec_->file_path_));
  }
  return ret;
}

int ObReadFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  row_index_ = 0;
  batch_row_count_ = 0;
  return ret;
}

int ObReadFileTableOp::switch_iterator()
{
  return OB_SUCCESS;
}

int ObReadFileTableOp::inner_close()
{
  if (OB_NOT_NULL(file_reader_)) {
    ObFileReader::destroy(file_reader_);
    file_reader_ = nullptr;
  }
  return OB_SUCCESS;
}

int ObReadFileTableOp::open_file()
{
  int ret = OB_SUCCESS;
  ObFileReadParam read_param;
  read_param.filename_ = spec_->file_path_;
  read_param.file_location_ = ObLoadFileLocation::SERVER_DISK;
  if (OB_FAIL(ObFileReader::open(read_param, ctx_.get_allocator(), file_reader_))) {
    LOG_USER_ERROR(OB_FILE_NOT_EXIST, "failed to open the file %.*s",
                   spec_->file_path_.length(), spec_->file_path_.ptr());
  } else if (OB_ISNULL(file_reader_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObFileReadUtils::build_csv_format(ctx_.get_allocator(),
                                                       spec_->field_delimiter_,
                                                       spec_->line_delimiter_,
                                                       spec_->escape_char_,
                                                       spec_->enclosed_by_,
                                                       spec_->skip_header_,
                                                       csv_format_))) {
  } else if (OB_FAIL(csv_parser_.init(csv_format_))) {
  }
  return ret;
}

// Reads the next chunk of the file into the buffer.
int ObReadFileTableOp::read_next_batch()
{
  int ret = OB_SUCCESS;
  file_buffer_.reset();
  buffer_offset_ = 0;
  buffer_size_ = 0;
  row_values_.reset();
  row_index_ = 0;
  batch_row_count_ = 0;
  if (OB_ISNULL(file_reader_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    char *chunk = static_cast<char *>(ctx_.get_allocator().alloc(READ_FILE_BUFFER_SIZE));
    if (OB_ISNULL(chunk)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t read_size = 0;
      if (OB_FAIL(file_reader_->read(chunk, READ_FILE_BUFFER_SIZE, read_size))) {
        LOG_WARN("failed to read the file", K(ret), K(spec_->file_path_));
      } else if (read_size > 0) {
        if (OB_FAIL(file_buffer_.append(chunk, read_size))) {
        } else {
          buffer_size_ = read_size;
        }
      } else {
        is_eof_ = true;
      }
    }
  }
  return ret;
}

// Parses one csv row into the fields of the current row.
int ObReadFileTableOp::parse_csv_row()
{
  int ret = OB_SUCCESS;
  const char *str = file_buffer_.string().ptr() + buffer_offset_;
  const char *end = file_buffer_.string().ptr() + buffer_size_;
  int64_t nrows = 0;
  ObSEArray<ObCSVGeneralParser::LineErrRec, 16> errors;
  struct RowHandler
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
  } handler{row_values_};
  if (OB_FAIL(csv_parser_.scan(str, end, nrows, nullptr, nullptr, handler, errors, is_eof_))) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "the file %.*s holds a malformed csv row",
                   spec_->file_path_.length(), spec_->file_path_.ptr());
  } else if (nrows <= 0) {
    ret = OB_ITER_END;
  } else {
    // The parser consumed the row, the offset moves past it.
    buffer_offset_ = str - file_buffer_.string().ptr();
    batch_row_count_ = row_values_.count();
  }
  return ret;
}

// Parses one json line into the fields of the current row.
int ObReadFileTableOp::parse_json_row(const ObString &line)
{
  int ret = OB_SUCCESS;
  ObJsonNode *json_node = nullptr;
  if (OB_FAIL(ObJsonParser::get_tree(&ctx_.get_allocator(), line, json_node))) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "the file %.*s holds a malformed json document",
                   spec_->file_path_.length(), spec_->file_path_.ptr());
  } else if (OB_ISNULL(json_node) || json_node->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
    LOG_USER_ERROR(OB_INVALID_DATA, "the file %.*s does not hold json objects",
                   spec_->file_path_.length(), spec_->file_path_.ptr());
  } else {
    ObJsonObject *json_object = static_cast<ObJsonObject *>(json_node);
    // The values of the columns are found by the name of the column, a key
    // that the object does not hold leaves the column empty.
    row_values_.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < column_names_.count(); i++) {
      ObString value;
      ObJsonNode *value_node = json_object->get_value(column_names_.at(i));
      if (OB_NOT_NULL(value_node)) {
        if (value_node->json_type() == ObJsonNodeType::J_STRING) {
          value = static_cast<ObJsonString *>(value_node)->get_str();
        } else {
          ObStringBuffer value_buf(&ctx_.get_allocator());
          if (OB_FAIL(value_node->print(value_buf, 0))) {
            break;
          }
          value = value_buf.string();
        }
      }
      if (OB_FAIL(row_values_.push_back(value))) {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      batch_row_count_ = row_values_.count();
    }
  }
  return ret;
}

// Writes the value of one column into its expression.
int ObReadFileTableOp::set_column_datum(const int64_t col_idx, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (col_idx >= spec_->column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObExpr *expr = spec_->column_exprs_.at(col_idx);
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (value.empty()) {
      // An empty field is a null value.
      datum.set_null();
    } else {
      datum.set_string(value);
    }
  }
  return ret;
}

int ObReadFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  bool filtered = false;
  while (OB_SUCC(ret)) {
    if (row_index_ < batch_row_count_) {
      // The current batch still holds a row.
      const int64_t row = row_index_++;
      for (int64_t i = 0; OB_SUCC(ret) && i < spec_->column_exprs_.count(); i++) {
        ObString value;
        if (i < row_values_.count()) {
          value = row_values_.at(i);
        }
        if (OB_FAIL(set_column_datum(i, value))) {
          break;
        }
      }
      if (OB_SUCC(ret)) {
        // The filters of the query are evaluated on the produced row.
        if (OB_FAIL(filter_row(eval_ctx_, spec_->filters_, filtered))) {
          break;
        } else if (!filtered) {
          break;
        }
      }
    } else {
      // The batch is exhausted, the next one is read.
      if (is_eof_) {
        ret = OB_ITER_END;
        break;
      }
      if (OB_FAIL(read_next_batch())) {
        break;
      }
      if (buffer_size_ <= 0) {
        if (is_eof_) {
          ret = OB_ITER_END;
        }
        break;
      }
      if (spec_->file_format_ == ObFileReadFormat::CSV) {
        // The header holds the column names, it is not a row.
        if (OB_FAIL(parse_csv_row())) {
          break;
        }
        if (OB_SUCC(ret) && spec_->skip_header_ > 0) {
          // The rows that the header spans are not data.
          spec_->skip_header_--;
          row_values_.reset();
          batch_row_count_ = 0;
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
