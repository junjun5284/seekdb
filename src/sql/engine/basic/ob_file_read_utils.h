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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_READ_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_READ_UTILS_H_

#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "query/api/query/engine/cmd/ob_load_data_parser.h"
#include "sql/engine/cmd/ob_load_data_file_reader.h"

namespace oceanbase
{
namespace sql
{

// The format of the file a table reading function reads.
enum class ObFileReadFormat
{
  INVALID = 0,
  CSV,
  NDJSON,
  JSON,
  PARQUET,
  GLOB,
};

// The result of the schema inference of a file.
struct ObFileColumnDef
{
  common::ObString name_;
  common::ObDataType data_type_;
};

// Reads a local file and infers its column structure.
//
// The inference is deterministic: the same file always yields the same
// columns and the same types.
class ObFileReadUtils
{
public:
  ObFileReadUtils() {}
  virtual ~ObFileReadUtils() {}

  // Infers the columns of a CSV file. The header row holds the column names,
  // the following rows are sampled to determine the column types.
  static int infer_csv_schema(const common::ObString &file_path,
                              common::ObIAllocator &allocator,
                              common::ObIArray<ObFileColumnDef> &columns);

  // Infers the columns of a JSON Lines file. Every line holds one flat JSON
  // object, the columns are the union of the keys of the sampled lines.
  static int infer_ndjson_schema(const common::ObString &file_path,
                                 common::ObIAllocator &allocator,
                                 common::ObIArray<ObFileColumnDef> &columns);

  // Infers the columns of a JSON file. The file holds an array of flat JSON
  // objects or one flat JSON object.
  static int infer_json_schema(const common::ObString &file_path,
                               common::ObIAllocator &allocator,
                               common::ObIArray<ObFileColumnDef> &columns);

  // Infers the columns of a Parquet file from its footer schema.
  static int infer_parquet_schema(const common::ObString &file_path,
                                  common::ObIAllocator &allocator,
                                  common::ObIArray<ObFileColumnDef> &columns);

  // Expands a glob pattern into the matching file paths.
  static int expand_glob(const common::ObString &pattern,
                         common::ObIAllocator &allocator,
                         common::ObIArray<common::ObString> &files);

  // Checks that the path stays below the directory that secure_file_priv
  // allows, and resolves it to an absolute path.
  static int check_and_resolve_path(const common::ObString &secure_file_priv,
                                     const common::ObString &path,
                                     common::ObIAllocator &allocator,
                                     common::ObString &resolved_path);

  // Builds the CSV parser configuration used by the file reading functions.
  static int build_csv_format(common::ObIAllocator &allocator,
                               const common::ObString &field_delimiter,
                               const common::ObString &line_delimiter,
                               const common::ObString &escape_char,
                               const common::ObString &enclosed_by,
                               const int64_t skip_header,
                               ObCSVGeneralFormat &format);

  // Reads the header row of a csv file, the names of the columns are the
  // fields of that row.
  static int read_csv_header(common::ObIAllocator &allocator,
                              const common::ObString &content,
                              common::ObArray<common::ObString> &header_fields);

  // Determines the type of a text value. An empty text is a null.
  static common::ObDataType infer_value_type(const common::ObString &value,
                                             bool &is_null);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_READ_UTILS_H_
