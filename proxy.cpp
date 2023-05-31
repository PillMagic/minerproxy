#include "proxy.h"

#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <list>

#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#define SIZE_JUST_BUFFER 2048

std::string userAgent;
struct sockaddr_in addr_pool;
LOG logger;
MAP_MINERS all_miners;
#define DOCHECKPASS false
#define YOURPASSWORD "YourPassword"

void serveMiner(MINER *min);
void servePort(int port);
void serveLOG();
void expandFD(int size);

void usage() {
  printf("proxy [-o/--o/--bind] port [--url/--pool] url_pool "
         "[--user/-u/--wallet] wallet_address\n");
  exit(0);
}
int main(int argc, char **argv) {
  std::string wallet_address, pool_url;
  std::vector<int> bind_ports;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--o") == 0 ||
        strcmp(argv[i], "--bind") == 0)
      bind_ports.push_back(std::stoi(std::string(argv[++i])));
    else if (strcmp(argv[i], "--url") == 0 || strcmp(argv[i], "--pool") == 0)
      pool_url = std::string(argv[++i]);
    else if (strcmp(argv[i], "--user") == 0 || strcmp(argv[i], "-u") == 0 ||
             strcmp(argv[i], "--wallet") == 0)
      wallet_address = std::string(argv[++i]);
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      usage();
  }
  if (bind_ports.size() <= 0 || wallet_address.size() <= 0 ||
      pool_url.size() <= 0)
    usage();

  userAgent = "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{"
              "\"login\":\"" +
              wallet_address +
              "\",\"pass\":\"x\",\"agent\":\"xmrig-proxy/6.18.0 (Linux x86_64) "
              "libuv/1.20.3 gcc/9.4.0\"}}\n";
  struct hostent *he = gethostbyname(pool_url.c_str());
  char *ip = inet_ntoa(*(struct in_addr *)he->h_addr_list[0]);
  addr_pool.sin_addr.s_addr = inet_addr(ip);
  addr_pool.sin_port = htons(3333);
  addr_pool.sin_family = AF_INET;
  bzero(addr_pool.sin_zero, 8);

  signal(SIGHUP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  expandFD(32768);
  logger.printf("\x1b[33mBICYCLE\t\t\x1b[0m\n");
  for (int i = 0; i < bind_ports.size(); i++) {
    logger.printf("\x1b[33mBOUND\t\t\x1b[32m TO 0.0.0.0:\x1b[34m%d\x1b[0m\n",
                  bind_ports[i]);
    std::thread *lal = new std::thread(servePort, bind_ports[i]);
    lal->detach();
  }
  logger.printf("\x1b[33mLOGGED\t\t\x1b[32m%s\x1b[0m\n", pool_url.c_str());
  logger.printf("\x1b[33mCommands: \t\t\x1b[32m%s\x1b[0m\n",
                "w - show miner; m - 1minute update; l - 10minutes update; s - "
                "save show miners to FLOG");
  serveLOG();
}

void servePort(int port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    printf("ERROR CREATE SOCKET %s\n", strerror(errno));
  struct sockaddr_in addr, cliaddr;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  bzero(addr.sin_zero, 8);
  const int enable = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  if (bind(sockfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0) {
    logger.printf("PORT %d IS BEING USED\n", port);
    exit(0);
  }
  listen(sockfd, 1024);

  while (1) {
    socklen_t lenaddr = sizeof(struct sockaddr_in);
    int new_sockfd = accept(sockfd, (struct sockaddr *)&cliaddr, &lenaddr);
    if (new_sockfd < 0) {
      logger.printf("\x1b[31mERROR ACCEPT %s\x1b[0m\n", strerror(errno));
      continue;
    }
    MINER_INFO *mi;
    all_miners.lock();
    if (all_miners.find(cliaddr.sin_addr.s_addr) == all_miners.end()) {
      mi = new MINER_INFO(cliaddr.sin_addr.s_addr);
      mi->started = time(NULL);
      all_miners[cliaddr.sin_addr.s_addr] = mi;
      all_miners.unlock();
    } else {
      mi = all_miners[cliaddr.sin_addr.s_addr];
      all_miners.unlock();
      mi->m.lock();
      int alive = mi->alive;
      if (mi->count_exited >= 500)
        alive = 1;
      if (alive == 0) {
        mi->alive = 1;
        mi->started = time(NULL);
      }
      mi->m.unlock();
      if (alive == 1) {
        close(new_sockfd);
        continue;
      }
    }
    std::thread thd(serveMiner, new MINER(cliaddr, new_sockfd, mi));
    thd.detach();
  }
}
bool login(MINER &min) {
  if (connect(min.poolfd, (struct sockaddr *)&(min.pooladdr),
              sizeof(struct sockaddr_in)) < 0)
    return false;
  if (send(min.poolfd, userAgent.c_str(), userAgent.size(), MSG_NOSIGNAL) < 0)
    return false;
  return true;
}
void pollfd_init(struct pollfd *pfd, int fd, int flags = POLLIN) {
  pfd->fd = fd;
  pfd->events = flags;
  pfd->revents = 0;
}
char *getTOKEN(char *ptr) {
  for (; (*ptr) != ':' && (*ptr) != '\n' && (*ptr) != '\0'; ptr++)
    ;
  if ((*ptr) == '\n' || (*ptr) == '\0')
    return NULL;
  ptr++;
  for (; (*ptr) == ' ' && (*ptr) != '\n' && (*ptr) != '\0'; ptr++)
    ;
  if ((*ptr) == '\n' || (*ptr) == '\0')
    return NULL;
  char *start = ptr;
  int MEEM = 0;
  for (; (*ptr) != '\n' && (*ptr) != '\0'; ptr++) {
    if ((*ptr) == '{')
      MEEM++;
    else if (((*ptr) == '}' && (--MEEM) < 0) || ((*ptr) == ',' && MEEM <= 0))
      break;
  }
  char c = (*ptr);
  (*ptr) = '\0';
  char *newed = new char[strlen(start) + 1];
  strcpy(newed, start);
  (*ptr) = c;
  return newed;
}
int checkPoolMSG(char *justBuffer, int size) {
  if (size <= 0)
    return 0;
  char *ptr = strstr(justBuffer, "\"error\"");
  if (ptr == NULL)
    return 0;
  char *error = getTOKEN(ptr);
  if (error == NULL)
    return 0;
  if (strstr(error, "null") == NULL) {
    logger.increaseERRORS();
    delete[] error;
    return -1;
  }
  delete[] error;
  ptr = strstr(justBuffer, "\"result\"");
  if (ptr == NULL)
    return 0;
  char *result = getTOKEN(ptr);

  if (result == NULL)
    return 0;
  int ret = 0;
  if (strstr(result, "id") == NULL && strstr(result, "status") != NULL &&
      strstr(result, "OK") != NULL) {
    logger.increaseACCEPTS();
    ret = 1;
  }
  delete[] result;
  return ret;
}
bool pollfuck(struct pollfd *pfd, char *justBuffer, MINER &mmm, int sit) {
  std::queue<mebuffer *> &push = (sit == 0 ? mmm.to_pool : mmm.for_miner);
  std::queue<mebuffer *> &pop = (sit == 0 ? mmm.for_miner : mmm.to_pool);
  int *pushflag = (sit == 0 ? (&mmm.poolflag) : (&mmm.minerflag));
  int *popflag = (sit == 0 ? (&mmm.minerflag) : (&mmm.poolflag));

  if (pfd->revents & POLLHUP || pfd->revents & POLLERR)
    pfd->revents |= POLLIN;
  if (pfd->revents & POLLIN) {
    int ret = read(pfd->fd, justBuffer, SIZE_JUST_BUFFER);
    if (ret <= 0) {
      close(pfd->fd);
      if (sit == 0)
        mmm.minerfd = -1;
      else
        mmm.poolfd = -1;
      return false;
    }
    justBuffer[ret] = '\0';
    push.push(new mebuffer(justBuffer));
    (*pushflag) |= POLLOUT;
    if (sit == 1) {
      int dodo = checkPoolMSG(justBuffer, ret);
      if (dodo == -1 || dodo == 1) {
        mmm.info->m.lock();
        if (dodo == -1)
          mmm.info->ERRORS++;
        else
          mmm.info->ACCEPTS++;
        mmm.info->m.unlock();
      }
    }
  }
  if ((*popflag) & POLLOUT) {
    if (!pop.empty()) {
      mebuffer *meb = pop.front();
      if (send(pfd->fd, meb->str.c_str(), meb->str.size(), MSG_NOSIGNAL) > 0) {
        pop.pop();
        delete meb;
      }
    }
    if (pop.empty())
      (*popflag) = POLLIN;
  }
  return true;
}
bool check_auth_message(const std::string &msg) {
  if (!DOCHECKPASS)
    return true;
  if (msg.find(YOURPASSWORD) == std::string::npos)
    return false;
  return true;
}
void serveMiner(MINER *min) {
  MINER &mmm = *min;
  fcntl(mmm.minerfd, F_SETFL, O_NONBLOCK | fcntl(mmm.minerfd, F_GETFL, 0));

  struct pollfd tmpfd;
  pollfd_init(&tmpfd, mmm.minerfd);
  if (poll(&tmpfd, 1, 30000) < 0)
    return;

  if ((!pollfuck(&tmpfd, mmm.justBuffer, mmm, 0)) || mmm.to_pool.empty() ||
      !check_auth_message(mmm.to_pool.front()->str)) {
    min->info->m.lock();
    min->info->alive = 0;
    min->info->count_exited++;
    logger.reportExited(min->info);
    min->info->ended = time(NULL);
    min->info->m.unlock();
    delete min;
    return;
  }
  mmm.to_pool.pop();

  if ((mmm.poolfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    printf("ERROR CREATE SOCKET %s\n", strerror(errno));

  while (1) {
    if (!login(mmm)) {
      close(mmm.poolfd);
      if ((mmm.poolfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        printf("ERROR CREATE SOCKET %s\n", strerror(errno));
      sleep(5);
      continue;
    }
    fcntl(mmm.poolfd, F_SETFL, O_NONBLOCK | fcntl(mmm.poolfd, F_GETFL, 0));
    struct pollfd pfd[2];
    while (1) {
      pollfd_init(pfd, mmm.minerfd, mmm.minerflag);
      pollfd_init(pfd + 1, mmm.poolfd, mmm.poolflag);
      if (poll(pfd, 2, -1) <= 0) {
        sleep(1);
        continue;
      }
      if (!pollfuck(pfd, mmm.justBuffer, mmm, 0)) {
        min->info->m.lock();
        min->info->alive = 0;
        min->info->count_exited++;
        logger.reportExited(min->info);
        min->info->ended = time(NULL);
        min->info->m.unlock();

        delete min;
        return;
      }
      if (!pollfuck(pfd + 1, mmm.justBuffer, mmm, 1)) {
        logger.printf("\x1b[44mFUCKED OFF SERVER.\x1b[0m "
                      "\x1b[31mRECONNECTING...\x1b[0m\n");
        break;
      }
    }
    if ((mmm.poolfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
      printf("ERROR CREATE SOCKET %s\n", strerror(errno));
  }
}
MINER::MINER(struct sockaddr_in a, int fd, MINER_INFO *mi)
    : mineraddr(a), minerfd(fd), minerflag(POLLIN), pooladdr(addr_pool),
      poolfd(-1), poolflag(POLLIN), info(mi) {
  this->justBuffer = new char[SIZE_JUST_BUFFER];
}
MINER::~MINER() {
  delete[] justBuffer;
  if (poolfd != -1)
    close(poolfd);
  if (minerfd != -1)
    close(minerfd);
}
void LOG::printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}
void LOG::verbose(const char *fmt, ...) {
  if (this->verb) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
  }
}
void LOG::increaseERRORS() {
  m.lock();
  ERRORS++;
  m.unlock();
}
void LOG::increaseACCEPTS() {
  m.lock();
  ACCEPTS++;
  m.unlock();
}

char getch(int timeout) {
  struct pollfd sin;
  sin.fd = 1;
  sin.events = POLLIN;
  sin.revents = 0;
  if (poll(&sin, 1, timeout) <= 0)
    return -1;
  else
    return getchar();
}
void serveLOG() {
  char gotcha[1024];
  int LAST_ACCEPTS = 0, LAST_SIZE = 0, MAX_MINERS = 0,
      update_time = 600000; // 10 minutes
  while (1) {
    char ret = getch(update_time);
    if (ret == -1 || ret == 'g') {
      all_miners.lock();
      int size_miners = 0;
      for (const auto &p : all_miners) {
        if (p.second->alive == 1)
          size_miners++;
      }
      all_miners.unlock();
      if (MAX_MINERS < size_miners)
        MAX_MINERS = size_miners;
      double hashrate_like = (logger.ACCEPTS - LAST_ACCEPTS) / 2.0;
      logger.m.lock();
      logger.printf(
          "\x1b[45mOUT: %fkH/s? \x1b[0m  shares: \x1b[32m%d/\x1b[31m%d "
          "\x1b[33m+%d\x1b[0m, miners: %d(\x1b[32m+%d\x1b[0m), max: %d \n",
          hashrate_like, logger.ACCEPTS, logger.ERRORS,
          logger.ACCEPTS - LAST_ACCEPTS, size_miners, size_miners - LAST_SIZE,
          MAX_MINERS);
      LAST_ACCEPTS = logger.ACCEPTS;
      LAST_SIZE = size_miners;
      logger.m.unlock();
    } else if (ret == 'm') {
      update_time = 60000;
    } else if (ret == 'l') {
      update_time = 600000;
    } else if (ret == 'w') {
      all_miners.lock();
      struct sockaddr_in addr;
      char buffer_ip[18];
      buffer_ip[17] = 0;
      logger.printf("IP\t|\tALIVE\t|\tACCEPTS\t|\tREJ\t|\tCOUNT_EXITED\n");
      for (const auto &p : all_miners) {
        addr.sin_addr.s_addr = p.first;
        memset(buffer_ip, ' ', 17);
        char *gip = inet_ntoa(addr.sin_addr);
        for (int alah = 0; alah < strlen(gip); alah++)
          buffer_ip[alah] = gip[alah];

        logger.printf("\x1b[33m%s\x1b[0m:\t\x1b[32m%d\x1b[0m\t|\t%d\t|\t%d\t|"
                      "\t\x1b[31m%d\x1b[0m (+%d)\n",
                      buffer_ip, p.second->alive, p.second->ACCEPTS,
                      p.second->ERRORS, p.second->count_exited,
                      p.second->count_exited - p.second->last_count_exited);
        p.second->last_count_exited = p.second->count_exited;
      }
      all_miners.unlock();
    } else if (ret == 's') {
      std::string buff;
      FILE *flog = fopen("flog", "w");
      if (flog == NULL)
        continue;
      ;
      all_miners.lock();
      struct sockaddr_in addr;
      for (const auto &p : all_miners) {
        addr.sin_addr.s_addr = p.first;
        sprintf(gotcha, "%s\t%d\t|\t%d\t|\t%d\t|\t%d (+%d)\n",
                inet_ntoa(addr.sin_addr), p.second->alive, p.second->ACCEPTS,
                p.second->ERRORS, p.second->count_exited,
                p.second->count_exited - p.second->last_count_exited);
        p.second->last_count_exited = p.second->count_exited;
        buff += std::string(gotcha);
      }
      all_miners.unlock();
      fwrite(buff.c_str(), 1, buff.size(), flog);
      fclose(flog);
      logger.printf("I SAVED TO ./FLOG\n");
    } else if (ret == 'q') {
      logger.mla.lock();
      struct sockaddr_in addr;
      char buffer_ip[18];
      buffer_ip[17] = 0;
      for (int j = 0; j < logger.last_exited.size(); j++) {
        auto *me = logger.last_exited[j];
        if (me->alive == 1)
          continue;

        time_t wrk = me->ended - me->started;
        time_t k = me->ended;
        addr.sin_addr.s_addr = me->ip;
        memset(buffer_ip, ' ', 17);
        char *gip = inet_ntoa(addr.sin_addr);
        for (int alah = 0; alah < strlen(gip); alah++)
          buffer_ip[alah] = gip[alah];

        logger.printf("\x1b[33m%s\x1b[0m:  %ds  |  \x1b[32m%s\x1b[0m  |  %d  | "
                      " %d  |  %d\n",
                      buffer_ip, wrk, ctime(&k), me->ACCEPTS, me->ERRORS,
                      me->count_exited);
      }
      logger.last_exited.clear();
      logger.mla.unlock();
    }
  }
}
void expandFD(int size) {
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl); //
  rlim_t myLimit = size;
  if (rl.rlim_cur >= myLimit) {
    printf("DONT NEED EXPAND LIMIT\n");
    return;
  }
  rl.rlim_cur = myLimit;
  if (rl.rlim_max <= myLimit) {
    printf("NEED TO EXPAND RLIM_MAX\n");
    rlim_t maxed = rl.rlim_max - 1;
    if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
      printf("COULDN'T TO EXPAND RLIM_MAX\n");
      getrlimit(RLIMIT_NOFILE, &rl);
      rl.rlim_cur = rl.rlim_max - 1;
      setrlimit(RLIMIT_NOFILE, &rl);
    }
  } else if (setrlimit(RLIMIT_NOFILE, &rl) < 0) {
    printf("UNKWOWN ERROR EXPAND %s\n", strerror(errno));
  }
}

void LOG::reportExited(MINER_INFO *mi) {
  mla.lock();
  for (int i = 0; i < last_exited.size(); i++) {
    if (last_exited[i] == mi) {
      mi = NULL;
      break;
    }
  }
  if (mi != NULL)
    last_exited.push_back(mi);
  mla.unlock();
}