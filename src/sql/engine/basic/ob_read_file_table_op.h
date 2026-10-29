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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_READ_FILE_TABLE_OP_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_READ_FILE_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_read_utils.h"
#include "sql/engine/expr/ob_datum_cast.h"

namespace oceanbase
{
namespace sql
{

// The physical operator that reads a local file and produces its rows.
//
// The file is scanned in a streaming fashion, the rows are read in batches so
// that a big file does not have to be held in memory.
class ObReadFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObReadFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type),
      column_exprs_(alloc),
      file_path_(),
      file_format_(ObFileReadFormat::INVALID),
      field_delimiter_(","),
      line_delimiter_("\n"),
      escape_char_("\\"),
      enclosed_by_("\""),
      skip_header_(0),
      has_header_(true)
  {}
  INHERIT_TO_STRING_KV("op_spec", ObOpSpec, K_(file_format), K_(file_path));
  // The expressions that receive the values of the columns.
  common::ObFixedArray<ObExpr *, common::ObIAllocator> column_exprs_;
  // The path of the file that is read.
  common::ObString file_path_;
  // The format of the file.
  ObFileReadFormat file_format_;
  // The options of the csv parser.
  common::ObString field_delimiter_;
  common::ObString line_delimiter_;
  common::ObString escape_char_;
  common::ObString enclosed_by_;
  int64_t skip_header_;
  bool has_header_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObReadFileTableSpec);
};

class ObReadFileTableOp : public ObOperator
{
public:
  ObReadFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input);
  virtual ~ObReadFileTableOp();
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int switch_iterator() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override { ObOperator::destroy(); }
private:
  // Opens the file and reads the first batch of rows.
  int open_file();
  // Reads the next batch of rows, returns OB_ITER_END at the end of the file.
  int read_next_batch();
  // Writes the value of one column into its expression.
  int set_column_datum(const int64_t col_idx, const common::ObString &value);
  // Parses one csv row into the fields of the current row.
  int parse_csv_row();
  // Parses one json line into the fields of the current row.
  int parse_json_row(const common::ObString &line);
private:
  DISALLOW_COPY_AND_ASSIGN(ObReadFileTableOp);

private:
  // The specification of this operator.
  ObReadFileTableSpec *spec_;
  // The file that is read.
  ObFileReader *file_reader_;
  // The buffer that holds the part of the file that was read.
  common::ObStringBuffer file_buffer_;
  // The part of the buffer that was not consumed yet.
  int64_t buffer_offset_;
  // The size of the buffer.
  int64_t buffer_size_;
  // The values of the current row.
  common::ObArray<common::ObString> row_values_;
  // The index of the row that is returned next.
  int64_t row_index_;
  // The number of rows of the current batch.
  int64_t batch_row_count_;
  // True when the file was read completely.
  bool is_eof_;
  // The csv parser.
  ObCSVGeneralParser csv_parser_;
  // The configuration of the csv parser.
  ObCSVGeneralFormat csv_format_;
  // The names of the columns, used to map the json keys to the columns.
  common::ObArray<common::ObString> column_names_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_READ_FILE_TABLE_OP_H_
