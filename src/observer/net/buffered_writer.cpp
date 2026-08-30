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
// Created by Wangyunlai on 2023/06/16.
//

#ifdef __MUSL__
#include <errno.h>
#else
#include <sys/errno.h>
#endif
#include <unistd.h>

#include "net/buffered_writer.h"

BufferedWriter::BufferedWriter(int fd) : fd_(fd), buffer_() {} // 使用默认大小的 ring buffer

BufferedWriter::BufferedWriter(int fd, int32_t size) : fd_(fd), buffer_(size) {} // 指定缓冲区大小

BufferedWriter::~BufferedWriter() { close(); }

RC BufferedWriter::close()
{
  // 已经关闭过，直接返回成功，保证 close 可以重复调用。
  if (fd_ < 0) {
    return RC::SUCCESS;
  }

  // 关闭前必须把缓冲区里剩余数据刷出去，避免丢数据。
  RC rc = flush();
  if (OB_FAIL(rc)) {
    return rc;
  }

  // fd 置为无效值，防止之后误用。
  fd_ = -1;

  return RC::SUCCESS;
}

RC BufferedWriter::write(const char *data, int32_t size, int32_t &write_size)
{
  if (fd_ < 0) {
    return RC::INVALID_ARGUMENT;
  }

  // 缓冲区满了就先刷一部分出去，腾出空间再写入。
  if (buffer_.remain() == 0) {
    RC rc = flush_internal(size);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  // 真正把数据写入 ring buffer；write_size 返回本次实际写入的字节数。
  return buffer_.write(data, size, write_size);
}

RC BufferedWriter::writen(const char *data, int32_t size)
{
  if (fd_ < 0) {
    return RC::INVALID_ARGUMENT;
  }

  // 与 write 的区别：writen 会循环写，直到 size 个字节全部写完才算成功。
  int32_t write_size = 0;
  while (write_size < size) {
    int32_t tmp_write_size = 0;

    // data + write_size 表示从上次没写完的位置继续写。
    RC rc = write(data + write_size, size - write_size, tmp_write_size);
    if (OB_FAIL(rc)) {
      return rc;
    }

    write_size += tmp_write_size; // 累计已写入字节数，驱动循环结束。
  }

  return RC::SUCCESS;
}

RC BufferedWriter::flush()
{
  if (fd_ < 0) {
    return RC::INVALID_ARGUMENT;
  }

  RC rc = RC::SUCCESS;
  // 只要缓冲区里还有数据，就一直刷到 socket，直到写完。
  while (OB_SUCC(rc) && buffer_.size() > 0) {
    rc = flush_internal(buffer_.size());
  }
  return rc;
}

RC BufferedWriter::flush_internal(int32_t size)
{
  if (fd_ < 0) {
    return RC::INVALID_ARGUMENT;
  }

  RC rc = RC::SUCCESS;

  int32_t write_size = 0;
  // 从 ring buffer 取出连续数据，调用底层 write 发送；网络写不完整时会循环。
  while (OB_SUCC(rc) && buffer_.size() > 0 && size > write_size) {
    const char *buf       = nullptr;
    int32_t     read_size = 0;
    // buffer_.buffer 取出当前可发送的连续内存段和长度。
    rc                    = buffer_.buffer(buf, read_size);
    if (OB_FAIL(rc)) {
      return rc;
    }

    ssize_t tmp_write_size = 0;
    while (tmp_write_size == 0) {
      tmp_write_size = ::write(fd_, buf, read_size);
      if (tmp_write_size < 0) {
        // EAGAIN 表示缓冲区暂时写不下，EINTR 表示被信号打断，都重试即可。
        if (errno == EAGAIN || errno == EINTR) {
          tmp_write_size = 0;
          continue;
        } else {
          return RC::IOERR_WRITE;
        }
      }
    }

    write_size += tmp_write_size;
    // forward 表示把已经发送成功的字节从 ring buffer 中消费掉。
    buffer_.forward(tmp_write_size);
  }

  return rc;
}
