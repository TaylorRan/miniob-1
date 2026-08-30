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
// Created by Longda on 2021/4/14.
//

#include "event/sql_event.h"

#include "event/session_event.h"
#include "sql/stmt/stmt.h"

// SQLStageEvent 用于 SQL 的“处理阶段”，和 SessionEvent 对应：
// SessionEvent 描述一次网络请求，SQLStageEvent 则携带各阶段产物
// （语法树 sql_node_、Stmt、物理执行计划 operator_）在 parse/resolve/optimize/execute 间传递。
// 注意：SQLStageEvent 不拥有 session_event_，只持有指针，真正的 SessionEvent 由上层管理。
SQLStageEvent::SQLStageEvent(SessionEvent *event, const string &sql) : session_event_(event), sql_(sql) {}

SQLStageEvent::~SQLStageEvent() noexcept
{
  // 释放自己持有的 Stmt；SessionEvent 由外部负责释放。
  if (session_event_ != nullptr) {
    session_event_ = nullptr;
  }

  // stmt_ 是裸指针，需要手动 delete；sql_node_ 和 operator_ 是 unique_ptr，会自动释放。
  if (stmt_ != nullptr) {
    delete stmt_;
    stmt_ = nullptr;
  }
}
