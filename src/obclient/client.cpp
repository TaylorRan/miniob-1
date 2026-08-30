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
// Created by Longda on 2021
//

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "common/defs.h"
#include "common/lang/string.h"
#include "common/linereader/line_reader.h"
#include "common/log/log.h"

#define MAX_MEM_BUFFER_SIZE 8192    //收数据缓冲区大小，和协议最大消息长度一致。
#define PORT_DEFAULT 6789   //默认服务端口

using namespace std;
using namespace common;

const std::string LINE_HISTORY_FILE = "./.obclient.history";

// ============================================================================
// 客户端整体工作流程：
//   1. 解析命令行参数，确定是走 TCP 还是 Unix Domain Socket；
//   2. 建立到服务端的连接；
//   3. 循环：从终端读取一行 SQL -> 发送给服务端 -> 等待并打印服务端返回结果。
//
// 注意：这个文件只负责“网络收发”和“终端交互”，它不理解 SQL。
// SQL 的具体解析和执行都在服务端完成。
// ============================================================================

// 创建并连接一个 Unix Domain Socket（本机进程间通信，不经过 TCP/IP）。
int init_unix_sock(const char *unix_sock_path)
{
  // SOCK_STREAM 表示流式 socket，和 TCP 一样提供有序、可靠的字节流。
  int sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
  if (sockfd < 0) {
    fprintf(stderr, "failed to create unix socket. %s", strerror(errno));
    return -1;
  }

  // Unix socket 地址结构体：先清零，再填协议族和路径。
  struct sockaddr_un sockaddr;
  memset(&sockaddr, 0, sizeof(sockaddr));
  sockaddr.sun_family = PF_UNIX;
  snprintf(sockaddr.sun_path, sizeof(sockaddr.sun_path), "%s", unix_sock_path);

  // connect 成功表示和服务端建立好了连接，之后可以像读写文件一样读写 sockfd。
  if (connect(sockfd, (struct sockaddr *)&sockaddr, sizeof(sockaddr)) < 0) {
    fprintf(stderr, "failed to connect to server. unix socket path '%s'. error %s", sockaddr.sun_path, strerror(errno));
    close(sockfd);
    return -1;
  }
  return sockfd;
}

// 创建并连接一个 TCP Socket，是默认的连接方式。
int init_tcp_sock(const char *server_host, int server_port)
{
  struct hostent    *host;
  struct sockaddr_in serv_addr;

  // gethostbyname 把主机名或 IP 字符串解析成地址信息，用于填充连接参数。
  if ((host = gethostbyname(server_host)) == NULL) {
    fprintf(stderr, "gethostbyname failed. errmsg=%d:%s\n", errno, strerror(errno));
    return -1;
  }

  int sockfd;
  // 创建 IPv4 + TCP 流式 socket。
  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    fprintf(stderr, "create socket error. errmsg=%d:%s\n", errno, strerror(errno));
    return -1;
  }

  // 填充目标地址：协议族、端口、IP。
  // htons 把主机字节序转成网络字节序（大端），网络协议要求端口使用网络字节序。
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port   = htons(server_port);
  serv_addr.sin_addr   = *((struct in_addr *)host->h_addr);
  bzero(&(serv_addr.sin_zero), 8);

  if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr)) == -1) {
    fprintf(stderr, "Failed to connect. errmsg=%d:%s\n", errno, strerror(errno));
    close(sockfd);
    return -1;
  }
  return sockfd;
}

const char *startup_tips = R"(
Welcome to the OceanBase database implementation course.

Copyright (c) 2021 OceanBase and/or its affiliates.

Learn more about OceanBase at https://github.com/oceanbase/oceanbase
Learn more about MiniOB at https://github.com/oceanbase/miniob

)";

int main(int argc, char *argv[])
{
  printf("%s", startup_tips);

  // 默认连接本机 6789 端口；也可用 -s 指定 unix socket、-h 指定 host、-p 指定端口。
  const char  *unix_socket_path = nullptr;
  const char  *server_host      = "127.0.0.1";
  int          server_port      = PORT_DEFAULT;
  int          opt;
  extern char *optarg;
  // getopt 解析命令行选项；optarg 是当前选项的参数值。
  while ((opt = getopt(argc, argv, "s:h:p:")) > 0) {
    switch (opt) {
      case 's': unix_socket_path = optarg; break;
      case 'p': server_port = atoi(optarg); break;
      case 'h': server_host = optarg; break;
    }
  }

  const char *prompt_str = "miniob > ";

  int sockfd, send_bytes;

  // 根据参数选择连接方式，成功后会得到已经建立连接的 socket 文件描述符 sockfd。
  if (unix_socket_path != nullptr) {
    sockfd = init_unix_sock(unix_socket_path);
  } else {
    sockfd = init_tcp_sock(server_host, server_port);
  }
  if (sockfd < 0) {
    return 1;
  }

  char send_buf[MAX_MEM_BUFFER_SIZE];

  std::string input_command = "";
  // 初始化行读取器，加载命令历史，方便上下方向键翻历史。
  MiniobLineReader::instance().init(LINE_HISTORY_FILE);

  // 客户端主循环：读一行 SQL，发给服务端，再读回结果。
  while (true) {
    input_command = MiniobLineReader::instance().my_readline(prompt_str);

    // 空行或纯空格不处理，直接重新等待输入。
    if (input_command.empty() || common::is_blank(input_command.c_str())) {
      continue;
    }

    // 本地退出命令，不需要发给服务端。
    if (MiniobLineReader::instance().is_exit_command(input_command)) {
      break;
    }

    // 关键点：发送长度是 length() + 1，多出的 1 个字节是字符串末尾的 '\0'。
    // MiniOB 的默认文本协议就是用 '\0' 表示“一条 SQL 结束”，
    // 所以这里要把结尾的 '\0' 一起发出去，服务端才能正确切分消息。
    if ((send_bytes = write(sockfd, input_command.c_str(), input_command.length() + 1)) == -1) {  // TODO writen
      fprintf(stderr, "send error: %d:%s \n", errno, strerror(errno));
      exit(1);
    }

    memset(send_buf, 0, sizeof(send_buf));

    // 发送完成后阻塞等待服务端响应，同样以 '\0' 作为响应结束标记。
    int len = 0;
    while ((len = recv(sockfd, send_buf, MAX_MEM_BUFFER_SIZE, 0)) > 0) {
      bool msg_end = false;
      // 逐字节检查响应中是否出现 '\0'，出现就说明一条完整响应已收完。
      for (int i = 0; i < len; i++) {
        if (0 == send_buf[i]) {
          msg_end = true;
          break;
        }
        printf("%c", send_buf[i]);
      }
      if (msg_end) {
        break; // 收到一条完整响应，结束本次接收。
      }
      // 还没收完整，清空缓冲区继续 recv 下一段。
      memset(send_buf, 0, MAX_MEM_BUFFER_SIZE);
    }

    if (len < 0) {
      fprintf(stderr, "Connection was broken: %s\n", strerror(errno));
      break;
    }
    if (0 == len) {
      printf("Connection has been closed\n");
      break;
    }
  }

  close(sockfd);

  return 0;
}


