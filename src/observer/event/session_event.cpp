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
// Created by Longda on 2021/4/13.
//

#include "session_event.h"
#include "net/communicator.h"

// SessionEvent 代表“一次客户端请求”。
// 它保存这条 SQL 文本、所属会话、执行结果和调试信息，贯穿整个处理过程。
// 构造时从 Communicator 取得 Session，并据此创建本次请求的 SqlResult。
SessionEvent::SessionEvent(Communicator *comm) : communicator_(comm), sql_result_(communicator_->session()) {}

SessionEvent::~SessionEvent() {}

// 返回处理本次请求所用的通信对象，用于回写结果。
Communicator *SessionEvent::get_communicator() const { return communicator_; }

// 返回本次请求所属的会话，业务代码靠它获取当前数据库、事务等信息。
Session *SessionEvent::session() const { return communicator_->session(); }
