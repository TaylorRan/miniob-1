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
// Created by Wangyunlai on 2022/11/17.
//

#include "net/communicator.h"
#include "net/buffered_writer.h"
#include "net/cli_communicator.h"
#include "net/mysql_communicator.h"
#include "net/plain_communicator.h"
#include "session/session.h"

#include "common/lang/mutex.h"

RC Communicator::init(int fd, unique_ptr<Session> session, const string &addr)
{
  // 保存连接 fd、对端地址和会话对象，并创建一个带缓冲的写出器，
  // 之后发送结果都先写进缓冲区，再批量 flush 到 socket。
  fd_      = fd;
  session_ = std::move(session);
  addr_    = addr;
  // writer_ 是输出通道；read_event/write_result 等接口会在子类中实现具体协议收发。
  writer_  = new BufferedWriter(fd_);
  return RC::SUCCESS;
}

Communicator::~Communicator()
{
  // 先关闭 socket，避免后续继续使用已经无效的连接。
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }

  // 再释放缓冲写出器，注意关闭时它内部还会把剩余缓冲 flush 一次。
  if (writer_ != nullptr) {
    delete writer_;
    writer_ = nullptr;
  }
}

/////////////////////////////////////////////////////////////////////////////////

Communicator *CommunicatorFactory::create(CommunicateProtocol protocol)
{
  // 根据服务端启动时指定的协议，创建对应的收发实现。
  // 默认是 PLAIN 文本协议，也支持 CLI（直接读写终端）和 MYSQL 协议。
  switch (protocol) {
    case CommunicateProtocol::PLAIN: {
      return new PlainCommunicator;
    } break;
    case CommunicateProtocol::CLI: {
      return new CliCommunicator;
    } break;
    case CommunicateProtocol::MYSQL: {
      return new MysqlCommunicator;
    } break;
    default: {
      return nullptr;
    }
  }
}
