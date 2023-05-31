#include <arpa/inet.h>
#include <map>
#include <mutex>
#include <queue>
#include <vector>
class MINER_INFO;
class LOG {
  bool verb = 0;
  std::mutex m;
  int ERRORS = 0;
  int ACCEPTS = 0;

  std::mutex mla;
  std::vector<MINER_INFO *> last_exited;

public:
  void increaseERRORS();
  void increaseACCEPTS();
  void reportExited(MINER_INFO *mi);
  friend void serveLOG();

  void verbose(const char *fmt, ...);
  void printf(const char *fmt, ...);
};
class mebuffer {
public:
  std::string str;
  mebuffer(const char *buff) : str(buff){};
};

class MINER_INFO {
public:
  std::mutex m;
  uint32_t ip;
  int alive;
  int ACCEPTS;
  int ERRORS;
  int count_exited;
  int last_count_exited;
  time_t started, ended;
  MINER_INFO(uint32_t i)
      : ip(i), alive(1), ACCEPTS(0), ERRORS(0), count_exited(0),
        last_count_exited(0) {}
};

class MINER {
public:
  MINER(struct sockaddr_in a, int fd, MINER_INFO *mi);
  ~MINER();
  std::queue<mebuffer *> for_miner;
  std::queue<mebuffer *> to_pool;
  MINER_INFO *info;
  char *justBuffer;

  struct sockaddr_in mineraddr, pooladdr;
  int minerfd, poolfd;
  int minerflag, poolflag;
};

class MAP_MINERS : public std::map<uint32_t, MINER_INFO *> {
  std::mutex m;

public:
  MAP_MINERS(){};
  void lock() { m.lock(); }
  void unlock() { m.unlock(); }
};