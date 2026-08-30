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

#include <poll.h>

#include "net/one_thread_per_connection_thread_handler.h"
#include "common/log/log.h"
#include "common/lang/thread.h"
#include "common/lang/mutex.h"
#include "common/lang/chrono.h"
#include "common/thread/thread_util.h"
#include "net/communicator.h"
#include "net/sql_task_handler.h"

using namespace common;

class Worker
{
public:
  Worker(ThreadHandler &host, Communicator *communicator) 
    : host_(host), communicator_(communicator)
  {}
  ~Worker()
  {
    // 线程还在运行就先停止并等待结束，确保对象析构前线程已退出。
    if (thread_ != nullptr) {
      stop();
      join();
    }
  }

  RC start()
  {
    // ref(*this) 把当前 Worker 对象包装成可调用对象，交给新线程执行 operator()。
    thread_ = new thread(ref(*this));
    return RC::SUCCESS;
  }

  RC stop()
  {
    // 把运行标志置为 false，线程下一轮 poll 超时后会退出循环。
    running_ = false;
    return RC::SUCCESS;
  }

  RC join()
  {
    if (thread_) {
      if (thread_->get_id() == this_thread::get_id()) {
        thread_->detach(); // 如果当前线程join当前线程，就会卡死
      } else {
        // 等待工作线程真正结束，再安全删除线程对象。
        thread_->join();
      }
      delete thread_;
      thread_ = nullptr;
    }
    return RC::SUCCESS;
  }

  void operator()()
  {
    // 每个连接都有自己的工作线程，这个函数就是该线程的主循环。
    LOG_INFO("worker thread start. communicator = %p", communicator_);
    int ret = thread_set_name("SQLWorker");
    if (ret != 0) {
      LOG_WARN("failed to set thread name. ret = %d", ret);
    }

    struct pollfd poll_fd;
    // 只监听当前连接的可读事件；有数据到达时 poll 会返回。
    poll_fd.fd = communicator_->fd();
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    while (running_) {
      // 等待这个连接上有数据可读。poll 超时时间 500ms，超时后继续循环，
      // 这样也能及时感知到 stop() 发出的退出信号。
      int ret = poll(&poll_fd, 1, 500);
      if (ret < 0) {
        LOG_WARN("poll error. fd = %d, ret = %d, error=%s", poll_fd.fd, ret, strerror(errno));
        break;
      } else if (0 == ret) {
        // LOG_TRACE("poll timeout. fd = %d", poll_fd.fd);
        // 超时不是错误，继续循环并检查 running_。
        continue;
      }

      if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        LOG_WARN("poll error. fd = %d, revents = %d", poll_fd.fd, poll_fd.revents);
        break;
      }

      // 有请求到达，交给 SqlTaskHandler：读取 SQL -> 解析 -> 优化 -> 执行 -> 写回结果。
      RC rc = task_handler_.handle_event(communicator_);
      if (OB_FAIL(rc)) {
        LOG_ERROR("handle error. rc = %s", strrc(rc));
        break;
      }
    }

    LOG_INFO("worker thread stop. communicator = %p", communicator_);
    // 工作线程退出前通知宿主关闭并删除这个连接；之后当前 Worker 对象会被销毁。
    host_.close_connection(communicator_); /// 连接关闭后，当前对象会被删除
  }

private:
  ThreadHandler &host_;
  SqlTaskHandler task_handler_;
  Communicator *communicator_ = nullptr;
  thread *thread_ = nullptr;
  volatile bool running_ = true;
};

OneThreadPerConnectionThreadHandler::~OneThreadPerConnectionThreadHandler()
{
  stop();
  await_stop();
}

RC OneThreadPerConnectionThreadHandler::new_connection(Communicator *communicator)
{
  // 加锁保护 thread_map_，避免并发 accept 时同时修改。
  lock_guard guard(lock_);

  // 每个 Communicator 对应一个 Worker 和一个独立线程。
  auto iter = thread_map_.find(communicator);
  if (iter != thread_map_.end()) {
    LOG_WARN("connection already exists. communicator = %p", communicator);
    return RC::FILE_EXIST;
  }

  Worker *worker = new Worker(*this, communicator);
  // 记录 Communicator -> Worker 的映射，便于后续关闭连接时找到对应线程。
  thread_map_[communicator] = worker;
  // start() 内部创建并启动真正的线程，之后该连接由 Worker::operator() 处理。
  return worker->start();
}

RC OneThreadPerConnectionThreadHandler::close_connection(Communicator *communicator)
{
  lock_.lock();
  // 从映射表取出并移除该连接对应的 Worker。
  auto iter = thread_map_.find(communicator);
  if (iter == thread_map_.end()) {
    LOG_WARN("connection not exists. communicator = %p", communicator);
    lock_.unlock();
    return RC::FILE_NOT_EXIST;
  }

  Worker *worker = iter->second;
  thread_map_.erase(iter);
  lock_.unlock();

  // 停止并等待工作线程退出，然后释放 Worker 和 Communicator。
  worker->stop();
  worker->join();
  delete worker;
  delete communicator;
  LOG_INFO("close connection. communicator = %p", communicator);
  return RC::SUCCESS;
}

RC OneThreadPerConnectionThreadHandler::stop()
{
  // 遍历所有连接，逐个设置停止标志，让所有工作线程尽快退出。
  lock_guard guard(lock_);
  for (auto iter = thread_map_.begin(); iter != thread_map_.end(); ++iter) {
    Worker *worker = iter->second;
    worker->stop();
  }
  return RC::SUCCESS;
}

RC OneThreadPerConnectionThreadHandler::await_stop()
{
  // 轮询等待 thread_map_ 被工作线程清空，即所有连接都已关闭。
  LOG_INFO("begin to await stop one thread per connection thread handler");
  while (!thread_map_.empty()) {
    this_thread::sleep_for(chrono::milliseconds(100));
  }
  LOG_INFO("end to await stop one thread per connection thread handler");
  return RC::SUCCESS;
}
