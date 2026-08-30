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
// Created by Wangyunlai on 2021/5/12.
//

#include "session/session.h"
#include "common/global_context.h"
#include "storage/db/db.h"
#include "storage/default/default_handler.h"
#include "storage/trx/trx.h"

Session &Session::default_session()
{
  // 返回一个默认会话。普通网络连接在 accept 时都会以它为模板创建新的 Session。
  // static 局部变量只初始化一次，整个进程共享同一个默认会话对象。
  static Session session;
  return session;
}

// 拷贝构造只复制“当前数据库”这一项，不复制事务和当前请求等运行时状态。
Session::Session(const Session &other) : db_(other.db_) {}

Session::~Session()
{
  // 会话销毁时，如果有未结束的事务，一并销毁，避免事务对象泄漏。
  if (nullptr != trx_) {
    db_->trx_kit().destroy_trx(trx_);
    trx_ = nullptr;
  }
}

const char *Session::get_current_db_name() const
{
  // 未选择数据库时返回空字符串，避免调用方拿到空指针。
  if (db_ != nullptr)
    return db_->name();
  else
    return "";
}

Db *Session::get_current_db() const { return db_; }

void Session::set_current_db(const string &dbname)
{
  // 从全局存储处理器中按名字查找数据库，找到后设为当前数据库。
  DefaultHandler &handler = *GCTX.handler_;
  Db             *db      = handler.find_db(dbname.c_str());
  // 数据库不存在时不切换，只记录日志；这样不会破坏当前已有的 db_。
  if (db == nullptr) {
    LOG_WARN("no such database: %s", dbname.c_str());
    return;
  }

  LOG_TRACE("change db to %s", dbname.c_str());
  db_ = db;
}

void Session::set_trx_multi_operation_mode(bool multi_operation_mode)
{
  // 多语句事务模式：begin 之后多次操作再显式 commit/rollback；
  // 否则每条 SQL 结束后自动提交/回滚并销毁事务。
  trx_multi_operation_mode_ = multi_operation_mode;
}

bool Session::is_trx_multi_operation_mode() const { return trx_multi_operation_mode_; }

Trx *Session::current_trx()
{
  /*
  当前把事务与数据库绑定到了一起。这样虽然不合理，但是处理起来也简单。
  我们在测试过程中，也不需要多个数据库之间做关联。
  */
  // 懒创建事务：第一次需要事务时才通过 trx_kit 创建，并绑定当前数据库的日志处理器。
  if (trx_ == nullptr) {
    // create_trx 是事务工厂入口，会根据配置返回 MVCC 或 Vacuous 等事务实现。
    trx_ = db_->trx_kit().create_trx(db_->log_handler());
  }
  return trx_;
}

void Session::destroy_trx()
  {
    // 显式销毁当前事务对象，通常发生在自动提交或事务结束之后。
    if (trx_ != nullptr) {
      db_->trx_kit().destroy_trx(trx_);
      trx_ = nullptr;
    }
  }

thread_local Session *thread_session = nullptr;

// MiniOB 用 thread_local 保存“当前线程正在处理的会话”，
// 这样代码里不需要到处传 Session 参数，就能通过 current_session() 拿到它。
void Session::set_current_session(Session *session) { thread_session = session; }

Session *Session::current_session() { return thread_session; }

void Session::set_current_request(SessionEvent *request) { current_request_ = request; }

// current_request_ 记录本会话当前正在处理的请求，处理完后会被清空。
SessionEvent *Session::current_request() const { return current_request_; }
