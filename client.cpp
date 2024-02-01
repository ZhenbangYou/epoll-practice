#include <fcntl.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

int openClientSocketAsync(const char *server, int port) {
  addrinfo hints{};
  addrinfo *matchingAddrs{};
  sockaddr_in dest{};

  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  int status =
      getaddrinfo(server, std::to_string(port).c_str(), &hints, &matchingAddrs);
  if (status != 0) {
    perror("status!=0");
  }
  std::memcpy(&dest, matchingAddrs->ai_addr, matchingAddrs->ai_addrlen);
  freeaddrinfo(matchingAddrs);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("client socket creation failed");
  }

  if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
    perror("setting socket non-blocking failed");
  }

  connect(fd, (sockaddr *)&dest, sizeof(dest));
  return fd;
}

const char TARGER_ADDR[] = "142.251.46.228";
const int PORT = 80;
const int MAX_EVENTS = 100;
const int BUF_SIZE = 1024 - 1;
const uint32_t EPOLL_OUT_EVENTS = EPOLLOUT | EPOLLET;
const uint32_t EPOLL_IN_EVENTS = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;
std::string OUTPUT_DIR("./out/");

void fetchAndSavePages(std::string path, int num_pages,
                       std::vector<std::string> &cumulative_messages) {
  int epollfd = epoll_create1(0);
  if (epollfd == -1) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }

  std::unordered_map<int, int> fd_to_index_of_out_files;
  for (int i = 0; i < num_pages; i++) {
    int fd = openClientSocketAsync(TARGER_ADDR, PORT);
    if (fd < 0) {
      perror("open socket failed");
    }
    fd_to_index_of_out_files[fd] = i;

    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = EPOLL_OUT_EVENTS;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
      perror("epoll_ctl: listen_sock");
      exit(EXIT_FAILURE);
    }
  }

  int remainingTasks = num_pages;
  while (remainingTasks > 0) {
    epoll_event events[MAX_EVENTS]{};
    int nfds = epoll_wait(epollfd, events, MAX_EVENTS, 10000);
    if (nfds == 0) {
      break; // timeout
    }
    if (nfds == -1) {
      perror("epoll_wait");
      exit(EXIT_FAILURE);
    }

    for (int i = 0; i < nfds; i++) {
      if (events[i].events & EPOLLERR) {
        perror("client gets error");

        remainingTasks--;

        int fd = events[i].data.fd;
        close(fd);
      } else if (events[i].events & EPOLLOUT) {
        int fd = events[i].data.fd;
        std::string request("GET / HTTP/1.0\r\n\r\n");
        send(fd, request.c_str(), request.length(), 0);

        epoll_event ev{};
        ev.data.fd = fd;
        ev.events = EPOLL_IN_EVENTS;
        if (epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
          perror("epoll_ctl: listen_sock");
        }
      } else if (events[i].events & EPOLLIN) {
        int fd = events[i].data.fd;
        while (true) {
          char buf[BUF_SIZE + 1]{}; // W/o `+1`, some data will be garbled
          int count = recv(fd, buf, BUF_SIZE, 0);
          if (count > 0) {
            cumulative_messages[fd_to_index_of_out_files[fd]] += buf;
          }
          // a short count or -1 means read space is exhausted
          // see https://man7.org/linux/man-pages/man7/epoll.7.html
          if (count < BUF_SIZE) {
            break;
          }
        }

        epoll_event ev{};
        ev.data.fd = fd;
        ev.events = EPOLL_IN_EVENTS;
        if (epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
          perror("epoll_ctl: listen_sock");
        }
      }
      if (events[i].events & EPOLLRDHUP) {
        int fd = events[i].data.fd;
        remainingTasks--;

        close(fd);
        // No need to remove the fd from epoll set
        // https://stackoverflow.com/questions/8707601/is-it-necessary-to-deregister-a-socket-from-epoll-before-closing-it
      }
    }
  }
  close(epollfd);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "please specify the number of web pages in the first "
                 "command line argument"
              << std::endl;
  }

  std::system((std::string("rm -rf ") + OUTPUT_DIR).c_str());
  std::system((std::string("mkdir ") + OUTPUT_DIR).c_str());

  const int num_pages = atoi(argv[1]);

  std::vector<std::string> cumulative_messages(num_pages);

  const auto start_time = std::chrono::system_clock::now();

  fetchAndSavePages("tmp", num_pages, cumulative_messages);

  const auto end_time = std::chrono::system_clock::now();
  const auto time_elapsed_millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time -
                                                            start_time);
  std::cout << "Time elapsed: " << time_elapsed_millis.count() << "ms"
            << std::endl;

  for (int i = 0; i < num_pages; i++) {
    std::ofstream fout(OUTPUT_DIR + std::to_string(i) + ".html",
                       std::ios::binary | std::ios::trunc);
    if (!cumulative_messages.empty()) {
      fout << cumulative_messages[i];
    }
  }
}
