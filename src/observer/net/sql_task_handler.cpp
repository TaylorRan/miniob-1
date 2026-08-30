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

#include "net/sql_task_handler.h"
#include "net/communicator.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"

RC SqlTaskHandler::handle_event(Communicator *communicator)
{
  // 工作线程入口：读取一条请求 -> 处理这条 SQL -> 把结果写回客户端。
  SessionEvent *event = nullptr;
  // read_event 从 socket 读出一条完整 SQL；失败则直接返回，通常意味着连接异常。
  RC rc = communicator->read_event(event);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (nullptr == event) {
    // 没有读到请求（如 CLI 空输入），什么都不做直接返回成功。
    return RC::SUCCESS;
  }

  // 先交给 SessionStage 做会话级的准备工作，并把当前线程会话绑定到 event 的会话。
  session_stage_.handle_request2(event);

  // SQLStageEvent 在 SessionEvent 之上封装 SQL 文本，供后续各阶段传递。
  SQLStageEvent sql_event(event, event->query());

  rc = handle_sql(&sql_event);
  if (OB_FAIL(rc)) {
    // 处理失败时记录错误码，但仍会走到下面 write_result 把错误状态回给客户端。
    LOG_TRACE("failed to handle sql. rc=%s", strrc(rc));
    event->sql_result()->set_return_code(rc);
  }

  bool need_disconnect = false;

  // 处理完成后，把结果（状态码、表头、数据行）写回客户端。
  rc = communicator->write_result(event, need_disconnect);
  LOG_INFO("write result return %s", strrc(rc));
  // 本次请求处理完毕，清空当前请求和当前会话，避免残留状态影响下一条 SQL。
  event->session()->set_current_request(nullptr);
  Session::set_current_session(nullptr);

  delete event;

  // 如果通信层标记需要断开（如读写错误），返回失败让上层结束工作线程。
  if (need_disconnect) {
    return RC::INTERNAL;
  }
  return RC::SUCCESS;
}

RC SqlTaskHandler::handle_sql(SQLStageEvent *sql_event)
{
  // MiniOB 的 SQL 处理流水线，像工厂流水线一样按顺序经过五个阶段：
  //   query_cache：查询缓存，命中则直接返回结果；
  //   parse：      词法+语法解析，SQL 文本 -> ParsedSqlNode 语法树；
  //   resolve：    语义解析，语法树 -> Stmt（绑定表、字段等信息）；
  //   optimize：   优化，Stmt -> 逻辑计划 -> 物理执行计划；
  //   execute：    执行，把执行计划挂到结果对象上，真正取数发生在写回结果阶段。
  RC rc = query_cache_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do query cache. rc=%s", strrc(rc));
    return rc;
  }

  rc = parse_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do parse. rc=%s", strrc(rc));
    return rc;
  }

  rc = resolve_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do resolve. rc=%s", strrc(rc));
    return rc;
  }

  rc = optimize_stage_.handle_request(sql_event);
  // optimize 阶段对 DDL 等语句可能返回 UNIMPLEMENTED，这是允许的；
  // 真正失败时才中断流程。
  if (rc != RC::UNIMPLEMENTED && rc != RC::SUCCESS) {
    LOG_TRACE("failed to do optimize. rc=%s", strrc(rc));
    return rc;
  }

  rc = execute_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do execute. rc=%s", strrc(rc));
    return rc;
  }

  return rc;
}
