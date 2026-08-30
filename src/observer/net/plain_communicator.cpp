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
// Created by Wangyunlai on 2023/06/25.
//

#include "net/plain_communicator.h"
#include "common/io/io.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "net/buffered_writer.h"
#include "session/session.h"
#include "sql/expr/tuple.h"

PlainCommunicator::PlainCommunicator()
{
  // 默认文本协议的“消息结束符”是 '\0'。
  // 客户端发送 SQL 时会把字符串末尾的 '\0' 一起发来，服务端据此判断一条消息是否完整。
  send_message_delimiter_.assign(1, '\0');
  // 调试信息每行以 "# " 开头，方便客户端区分正常结果和调试输出。
  debug_message_prefix_.resize(2);
  debug_message_prefix_[0] = '#';
  debug_message_prefix_[1] = ' ';
}

RC PlainCommunicator::read_event(SessionEvent *&event)
{
  RC rc = RC::SUCCESS;

  event = nullptr;

  int data_len = 0;
  int read_len = 0;

  // 单条消息最长 8192 字节；超过会返回 IOERR_TOO_LONG。
  const int    max_packet_size = 8192;
  vector<char> buf(max_packet_size);

  // 持续接收消息，直到遇到'\0'。将'\0'遇到的后续数据直接丢弃没有处理，因为目前仅支持一收一发的模式
  // 也就是说：一次只处理一条 SQL，读到结束符 '\0' 就停止。
  while (true) {
    // 从缓冲区的 data_len 偏移处继续读，把新数据接到旧数据后面。
    read_len = ::read(fd_, buf.data() + data_len, max_packet_size - data_len);
    if (read_len < 0) {
      // EAGAIN 表示暂无数据，稍后再试；其他错误直接结束读取。
      if (errno == EAGAIN) {
        continue;
      }
      break;
    }
    // read 返回 0 表示对端已关闭连接。
    if (read_len == 0) {
      break;
    }

    // 如果读完后已经超过限制，记录长度并退出，后面统一返回“消息过长”。
    if (read_len + data_len > max_packet_size) {
      data_len += read_len;
      break;
    }

    // 在本次读到的字节里查找 '\0'，找到就说明完整消息到此结束。
    bool msg_end = false;
    for (int i = 0; i < read_len; i++) {
      if (buf[data_len + i] == 0) {
        // 把 '\0' 也计入 data_len，后面直接按这个长度使用数据。
        data_len += i + 1;
        msg_end = true;
        break;
      }
    }

    if (msg_end) {
      break;
    }

    data_len += read_len;
  }

  if (data_len > max_packet_size) {
    LOG_WARN("The length of sql exceeds the limitation %d", max_packet_size);
    return RC::IOERR_TOO_LONG;
  }
  if (read_len == 0) {
    LOG_INFO("The peer has been closed %s", addr());
    return RC::IOERR_CLOSE;
  } else if (read_len < 0) {
    LOG_ERROR("Failed to read socket of %s, %s", addr(), strerror(errno));
    return RC::IOERR_READ;
  }

  LOG_INFO("receive command(size=%d): %s", data_len, buf.data());
  // 把刚收到的 SQL 文本放进 SessionEvent，后面 SqlTaskHandler 会从这个事件里取出 SQL。
  event = new SessionEvent(this);
  event->set_query(string(buf.data()));
  return rc;
}

RC PlainCommunicator::write_state(SessionEvent *event, bool &need_disconnect)
{
  SqlResult    *sql_result   = event->sql_result();
  const int     buf_size     = 2048;
  char         *buf          = new char[buf_size];
  const string &state_string = sql_result->state_string();
  // 没有错误说明时，只回 SUCCESS/FAILURE；有说明则带上返回码和说明。
  if (state_string.empty()) {
    const char *result = RC::SUCCESS == sql_result->return_code() ? "SUCCESS" : "FAILURE";
    snprintf(buf, buf_size, "%s\n", result);
  } else {
    snprintf(buf, buf_size, "%s > %s\n", strrc(sql_result->return_code()), state_string.c_str());
  }

  RC rc = writer_->writen(buf, strlen(buf));
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to send data to client. err=%s", strerror(errno));
    need_disconnect = true;
    delete[] buf;
    return RC::IOERR_WRITE;
  }

  need_disconnect = false;
  delete[] buf;

  return RC::SUCCESS;
}

RC PlainCommunicator::write_debug(SessionEvent *request, bool &need_disconnect)
{
  // 只有会话打开了 SQL 调试开关，才输出调试信息。
  if (!session_->sql_debug_on()) {
    return RC::SUCCESS;
  }

  SqlDebug &sql_debug = request->sql_debug();

  const list<string> &debug_infos = sql_debug.get_debug_infos();
  // 每条调试信息前加 "# " 前缀，并换行。
  for (auto &debug_info : debug_infos) {
    RC rc = writer_->writen(debug_message_prefix_.data(), debug_message_prefix_.size());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to send data to client. err=%s", strerror(errno));
      need_disconnect = true;
      return RC::IOERR_WRITE;
    }

    rc = writer_->writen(debug_info.data(), debug_info.size());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to send data to client. err=%s", strerror(errno));
      need_disconnect = true;
      return RC::IOERR_WRITE;
    }

    char newline = '\n';

    rc = writer_->writen(&newline, 1);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to send new line to client. err=%s", strerror(errno));
      need_disconnect = true;
      return RC::IOERR_WRITE;
    }
  }

  need_disconnect = false;
  return RC::SUCCESS;
}

RC PlainCommunicator::write_result(SessionEvent *event, bool &need_disconnect)
{
  // 返回结果的顺序：
  //   1. write_result_internal：写状态、表头和数据行；
  //   2. write_debug：如果打开了 SQL 调试开关，再附加调试信息；
  //   3. 最后写一个 '\0' 表示响应结束，并 flush 缓冲。
  RC rc = write_result_internal(event, need_disconnect);
  if (!need_disconnect) {
    RC rc1 = write_debug(event, need_disconnect);
    if (OB_FAIL(rc1)) {
      LOG_WARN("failed to send debug info to client. rc=%s, err=%s", strrc(rc), strerror(errno));
    }
  }
  if (!need_disconnect) {
    // 最后补上 '\0' 结束符，让客户端知道一条完整响应结束。
    rc = writer_->writen(send_message_delimiter_.data(), send_message_delimiter_.size());
    if (OB_FAIL(rc)) {
      LOG_ERROR("Failed to send data back to client. ret=%s, error=%s", strrc(rc), strerror(errno));
      need_disconnect = true;
      return rc;
    }
  }
  // 把所有缓冲数据真正刷到 socket。
  writer_->flush();  // TODO handle error
  return rc;
}

RC PlainCommunicator::write_result_internal(SessionEvent *event, bool &need_disconnect)
{
  RC rc = RC::SUCCESS;

  need_disconnect = true;

  SqlResult *sql_result = event->sql_result();

  // 没有执行计划（例如 DDL），或者前面阶段已经失败，就只回一个状态字符串。
  if (RC::SUCCESS != sql_result->return_code() || !sql_result->has_operator()) {
    return write_state(event, need_disconnect);
  }

  // 打开执行计划：这里会启动事务，并递归调用执行计划顶层算子的 open()。
  rc = sql_result->open();
  if (OB_FAIL(rc)) {
    // open 失败时也要关闭结果并返回错误状态。
    sql_result->close();
    sql_result->set_return_code(rc);
    return write_state(event, need_disconnect);
  }

  const TupleSchema &schema   = sql_result->tuple_schema();
  const int          cell_num = schema.cell_num();

  // 先写表头：每列的名字/别名用 " | " 分隔，最后换行。
  for (int i = 0; i < cell_num; i++) {
    const TupleCellSpec &spec  = schema.cell_at(i);
    const char          *alias = spec.alias();
    // alias 非空才写；cell_num 为 0 的语句（如 DDL）没有表头。
    if (nullptr != alias || alias[0] != 0) {
      if (0 != i) {
        const char *delim = " | ";

        rc = writer_->writen(delim, strlen(delim));
        if (OB_FAIL(rc)) {
          LOG_WARN("failed to send data to client. err=%s", strerror(errno));
          return rc;
        }
      }

      int len = strlen(alias);

      rc = writer_->writen(alias, len);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to send data to client. err=%s", strerror(errno));
        sql_result->close();
        return rc;
      }
    }
  }

  if (cell_num > 0) {
    char newline = '\n';

    rc = writer_->writen(&newline, 1);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to send data to client. err=%s", strerror(errno));
      sql_result->close();
      return rc;
    }
  }

  rc = RC::SUCCESS;
  // 再写数据：根据执行模式选择“一行一行”还是“一 chunk 一 chunk”读取。
  if (event->session()->get_execution_mode() == ExecutionMode::CHUNK_ITERATOR
      && event->session()->used_chunk_mode()) {
    rc = write_chunk_result(sql_result);
  } else {
    rc = write_tuple_result(sql_result);
  }

  if (OB_FAIL(rc)) {
    return rc;
  }

  if (cell_num == 0) {
    // 除了select之外，其它的消息通常不会通过operator来返回结果，表头和行数据都是空的
    // 这里针对这种情况做特殊处理，当表头和行数据都是空的时候，就返回处理的结果
    // 可能是insert/delete等操作，不直接返回给客户端数据，这里把处理结果返回给客户端
    RC rc_close = sql_result->close();
    if (rc == RC::SUCCESS) {
      rc = rc_close;
    }
    sql_result->set_return_code(rc);
    return write_state(event, need_disconnect);
  } else {
    // 正常返回数据后不需要断开连接，客户端可以继续发下一条 SQL。
    need_disconnect = false;
  }

  // 数据写完后关闭执行计划，触发算子 close 和事务提交。
  RC rc_close = sql_result->close();
  if (OB_SUCC(rc)) {
    rc = rc_close;
  }

  return rc;
}

RC PlainCommunicator::write_tuple_result(SqlResult *sql_result)
{
  RC rc = RC::SUCCESS;
  Tuple *tuple = nullptr;
  // 不断调用 next_tuple 取下一行，直到返回 RECORD_EOF 表示没有更多数据。
  while (RC::SUCCESS == (rc = sql_result->next_tuple(tuple))) {
    assert(tuple != nullptr);

    int cell_num = tuple->cell_num();
    // 每列之间用 " | " 分隔，行尾换行，形成易读的表格文本。
    for (int i = 0; i < cell_num; i++) {
      if (i != 0) {
        const char *delim = " | ";

        rc = writer_->writen(delim, strlen(delim));
        if (OB_FAIL(rc)) {
          LOG_WARN("failed to send data to client. err=%s", strerror(errno));
          sql_result->close();
          return rc;
        }
      }

      Value value;
      // 从元组中取出一列的值，再转成字符串发送。
      rc = tuple->cell_at(i, value);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to get tuple cell value. rc=%s", strrc(rc));
        sql_result->close();
        return rc;
      }

      string cell_str = value.to_string();

      rc = writer_->writen(cell_str.data(), cell_str.size());
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to send data to client. err=%s", strerror(errno));
        sql_result->close();
        return rc;
      }
    }

    char newline = '\n';

    rc = writer_->writen(&newline, 1);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to send data to client. err=%s", strerror(errno));
      sql_result->close();
      return rc;
    }
  }

  if (rc == RC::RECORD_EOF) {
    // RECORD_EOF 是正常的“没有更多数据”标志，转成成功返回。
    rc = RC::SUCCESS;
  }
  return rc;
}

RC PlainCommunicator::write_chunk_result(SqlResult *sql_result)
{
  RC rc = RC::SUCCESS;
  Chunk chunk;
  // chunk 模式一次取一批行，减少函数调用次数，适合向量化执行。
  while (RC::SUCCESS == (rc = sql_result->next_chunk(chunk))) {
    int col_num = chunk.column_num();
    for (int row_idx = 0; row_idx < chunk.rows(); row_idx++) {
      for (int col_idx = 0; col_idx < col_num; col_idx++) {
        if (col_idx != 0) {
          const char *delim = " | ";

          rc = writer_->writen(delim, strlen(delim));
          if (OB_FAIL(rc)) {
            LOG_WARN("failed to send data to client. err=%s", strerror(errno));
            sql_result->close();
            return rc;
          }
        }

        Value value = chunk.get_value(col_idx, row_idx);

        string cell_str = value.to_string();

        rc = writer_->writen(cell_str.data(), cell_str.size());
        if (OB_FAIL(rc)) {
          LOG_WARN("failed to send data to client. err=%s", strerror(errno));
          sql_result->close();
          return rc;
        }
      }
      char newline = '\n';

      rc = writer_->writen(&newline, 1);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to send data to client. err=%s", strerror(errno));
        sql_result->close();
        return rc;
      }
    }
    // 处理完当前 chunk 后清空，准备接收下一批数据。
    chunk.reset();
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  return rc;
}
