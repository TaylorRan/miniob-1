/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Willaaaaaaa in 2025
//

#include "common/linereader/line_reader.h"
#include "common/lang/string.h"

namespace common {
MiniobLineReader::MiniobLineReader() : history_file_(""), previous_history_save_time_(0), history_save_interval_(5) {}

MiniobLineReader::~MiniobLineReader() { reader_.history_save(history_file_); }

MiniobLineReader &MiniobLineReader::instance()
{
  static MiniobLineReader instance;
  return instance;
}

void MiniobLineReader::init(const std::string &history_file)
{
  history_file_ = history_file;
  reader_.history_load(history_file_);
}

std::string MiniobLineReader::my_readline(const std::string &prompt)
{
  // 用 replxx 读取一行带提示符的输入。replxx 还提供历史记录、移动光标等功能。
  const char *cinput = nullptr;
  cinput             = reader_.input(prompt);
  if (cinput == nullptr) {
    return "";
  }

  std::string line = cinput;
  cinput           = nullptr;

  // 空行直接返回空字符串，由调用方决定如何处理。
  if (line.empty()) {
    return "";
  }

  // 只有包含非空白字符的输入才算有效命令，才会写入历史记录。
  bool is_valid_input = false;
  for (auto c : line) {
    if (!isspace(c)) {
      is_valid_input = true;
      break;
    }
  }

  if (is_valid_input) {
    reader_.history_add(line);
    check_and_save_history();
  }

  return line;
}

bool MiniobLineReader::is_exit_command(const std::string &cmd)
{
  std::string lower_cmd = cmd;
  common::str_to_lower(lower_cmd);

  bool is_exit = lower_cmd.compare(0, 4, "exit") == 0 || lower_cmd.compare(0, 3, "bye") == 0 ||
                 lower_cmd.compare(0, 2, "\\q") == 0 || lower_cmd.compare(0, 11, "interrupted") == 0;

  return is_exit;
}

bool MiniobLineReader::check_and_save_history()
{
  time_t current_time = time(nullptr);
  if (current_time - previous_history_save_time_ > history_save_interval_) {
    reader_.history_save(history_file_);
    previous_history_save_time_ = current_time;
    return true;
  }
  return false;
}
}  // namespace common
