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
// Created by Wangyunlai on 2024/01/10.
//

#include <string.h>

#include "net/thread_handler.h"
#include "net/one_thread_per_connection_thread_handler.h"
#include "net/java_thread_pool_thread_handler.h"
#include "common/log/log.h"
#include "common/lang/string.h"

ThreadHandler * ThreadHandler::create(const char *name)
{
  // 根据配置名字创建对应的线程模型。
  // 默认是 one-thread-per-connection：一个连接一个线程，实现简单、适合学习。
  const char *default_name = "one-thread-per-connection";
  // 配置为空时使用默认线程模型，避免调用方传空值导致创建失败。
  if (nullptr == name || common::is_blank(name)) {
    name = default_name;
  }

  // 按名字匹配；这里使用不区分大小写的比较。
  if (0 == strcasecmp(name, default_name)) {
    return new OneThreadPerConnectionThreadHandler();
  } else if (0 == strcasecmp(name, "java-thread-pool")) {
    return new JavaThreadPoolThreadHandler();
  } else {
    // 无法识别的线程模型返回 nullptr，由上层记录错误并退出。
    LOG_ERROR("unknown thread handler: %s", name);
    return nullptr;
  }
}
