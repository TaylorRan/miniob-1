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
// 初始化三个成员：历史文件为空、上次保存时间为 0、每 5 秒自动保存一次历史。
MiniobLineReader::MiniobLineReader() : history_file_(""), previous_history_save_time_(0), history_save_interval_(5) {}

// 析构时最后保存一次历史记录，避免用户输入的命令丢失。
MiniobLineReader::~MiniobLineReader() { reader_.history_save(history_file_); }

MiniobLineReader &MiniobLineReader::instance()
{
  // 单例模式：static 局部变量保证整个进程只创建一次。
  static MiniobLineReader instance;
  return instance;
}

void MiniobLineReader::init(const std::string &history_file)
{
  // 记录历史文件路径，并把已有历史加载进 replxx。
  history_file_ = history_file;
  reader_.history_load(history_file_);
}

std::string MiniobLineReader::my_readline(const std::string &prompt)
{
  // 用 replxx 读取一行带提示符的输入。replxx 还提供历史记录、移动光标等功能。
  const char *cinput = nullptr;
  cinput             = reader_.input(prompt);
  // 返回 nullptr 通常表示输入结束（如 Ctrl+D），这里统一返回空字符串。
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
  // 先转小写再比较，让 EXIT、Exit、exit 都能被识别为退出命令。
  std::string lower_cmd = cmd;
  common::str_to_lower(lower_cmd);

  bool is_exit = lower_cmd.compare(0, 4, "exit") == 0 || lower_cmd.compare(0, 3, "bye") == 0 ||
                 lower_cmd.compare(0, 2, "\\q") == 0 || lower_cmd.compare(0, 11, "interrupted") == 0;

  return is_exit;
}

bool MiniobLineReader::check_and_save_history()
{
  // 不是每条命令都写盘，而是每隔一定时间才保存一次，减少磁盘 I/O。
  time_t current_time = time(nullptr);
  if (current_time - previous_history_save_time_ > history_save_interval_) {
    reader_.history_save(history_file_);
    previous_history_save_time_ = current_time;
    return true;
  }
  return false;
}
}  // namespace common
