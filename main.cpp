#include <X11/Xlib.h>
#include <errno.h>
#include <libudev.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <iostream>
#include <thread>
#include <fmt/format.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/signal_set.hpp>
#include <fstream>

#define MAX_BLOCK_SIZE 128
#define MAX_STATUS_LEN 1024
#define LEFT_BORDER    "^c#dddddd^["
#define RIGHT_BORDER   "^c#dddddd^]"

using json = nlohmann::json;

enum class BlockId : int {
  Weather,
  ExchangeRate,
  Battery,
  Memory,
  Date,
  MAX_BLOCKS
};
constexpr int MAX_BLOCKS = static_cast<int>(BlockId::MAX_BLOCKS);

static Display        *dpy;
static int             screen;
static Window          root;
static pthread_mutex_t mutex;
static int             is_cable_plugged;

struct BlockData {
  int  len;
  char data[MAX_BLOCK_SIZE];
};

static void update(
  BlockId bix, const char *str)
{
  static char             status[2][MAX_STATUS_LEN] = {"", ""};
  static bool             current_status            = 0;
  static struct BlockData blocks[MAX_BLOCKS]        = {};

  pthread_mutex_lock(&mutex);

  const int blen                       = strlen(str);
  blocks[static_cast<size_t>(bix)].len = blen;
  memcpy(blocks[static_cast<size_t>(bix)].data, str, blen);

  char *ptr = status[!current_status];
  memcpy(ptr, LEFT_BORDER, sizeof(LEFT_BORDER) - 1);
  ptr += sizeof(LEFT_BORDER) - 1;
  for (int i = 0; i < MAX_BLOCKS; ++i) {
    const int len = blocks[i].len;
    if (!len) continue;
    memcpy(ptr, blocks[i].data, len);
    ptr += len;
  }

  memcpy(ptr, RIGHT_BORDER, sizeof(RIGHT_BORDER));

  ptr = status[!current_status];
  if (strcmp(ptr, status[current_status]) == 0) {
    pthread_mutex_unlock(&mutex);
    return;
  }

  current_status = !current_status;

  // std::cout << ptr << std::endl;
  XStoreName(dpy, root, ptr);

  pthread_mutex_unlock(&mutex);
  XFlush(dpy);
}

template <class T>
class Waitable
{
  boost::asio::deadline_timer timer_;
  void                        timer_task() {}

 protected:
  void start_timer(
    int wait_for)
  {
    timer_.expires_from_now(boost::posix_time::seconds(wait_for));
    timer_.async_wait([this](const boost::system::error_code &ec) {
      if (ec != boost::asio::error::operation_aborted)
        static_cast<T *>(this)->timer_task();
    });
  }

 public:
  Waitable(
    boost::asio::io_context &ioc)
      : timer_ {ioc}
  {
  }

  void cancel_timer() { timer_.cancel(); }
};

class Date : public Waitable<Date>
{
  friend Waitable<Date>;

  void timer_task() { do_time(); }

  void do_time()
  {
    static char months[][4] = {"Jan",
                               "Feb",
                               "Mar",
                               "Apr",
                               "May",
                               "Jun",
                               "Jul",
                               "Aug",
                               "Sep",
                               "Oct",
                               "Nov",
                               "Dec"};

    static char days[][4] = {"Sun", "Mon", "Tue", "Wen", "Thu", "Fri", "Sat"};
    static char str[MAX_BLOCK_SIZE];
    time_t      tm   = time(NULL);
    struct tm  *date = localtime(&tm);
    sprintf(str,
            " ^c#07d7e8^\uf073 ^c#10bbbb^%s %s %d %s %.02d:%.02d ",
            days[date->tm_wday],
            months[date->tm_mon],
            date->tm_mday,
            (date->tm_hour >= 8 && date->tm_hour < 21 ? "^c#edd238^\uf185"
                                                      : "^c#ecede8^\uf186"),
            date->tm_hour,
            date->tm_min);

    update(BlockId::Date, str);

    start_timer(60 - date->tm_sec + 1);
  }

 public:
  Date(
    boost::asio::io_context &ioc)
      : Waitable {ioc}
  {
  }

  void start() { do_time(); }
};

class Memory : public Waitable<Memory>
{
  friend Waitable<Memory>;

  void timer_task() { memory_status(); }

  void memory_status()
  {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
      perror("fopen");
    }

    char          line[256];
    unsigned long memTotal = 0, memFree = 0, buffers = 0, cached = 0,
                  memReclaimable = 0;

    for (int i = 0; fgets(line, sizeof(line), file);) {
      if (i == 5) break;
      if ((sscanf(line, "MemTotal: %lu kB", &memTotal) == 1) ||
          (sscanf(line, "MemFree: %lu kB", &memFree) == 1) ||
          (sscanf(line, "Buffers: %lu kB", &buffers) == 1) ||
          (sscanf(line, "Cached: %lu kB", &cached) == 1) ||
          (sscanf(line, "SReclaimable: %lu kB", &memReclaimable) == 1))
        ++i;
    }

    fclose(file);

    unsigned long memCachedAll = cached + memReclaimable;

    char result[MAX_BLOCK_SIZE];
    sprintf(result,
            "^c#186da5^ \uf2db %.1fGB",
            (memTotal - memFree - memCachedAll) / 1000.0f / 1000.f);
    update(BlockId::Memory, result);

    start_timer(5);
  }

 public:
  Memory(
    boost::asio::io_context &ioc)
      : Waitable {ioc}
  {
  }

  void start() { memory_status(); }
};

class RestApi : public Waitable<RestApi>
{
  friend Waitable<RestApi>;

  BlockId     block_id_;
  std::string block_name_;
  std::string cache_file_;
  int         refresh_interval_;

  std::string                    url_;
  std::pair<time_t, std::string> last_sync_;

  auto get_now() { return time(NULL); }

  void store_last_sync(
    std::string &&value)
  {
    last_sync_.first  = get_now();
    last_sync_.second = std::move(value);
    std::ofstream ofs(cache_file_);
    if (ofs.is_open()) {
      ofs << last_sync_.second << "\n" << last_sync_.first;
      ofs.close();
    }
  }

  void timer_task() { perform_request(); }

  static size_t WriteCallback(
    void *contents, size_t size, size_t nmemb, std::string *output)
  {
    size_t totalSize = size * nmemb;
    output->append((char *)contents, totalSize);
    return totalSize;
  }

  void perform_request()
  {
    CURL       *curl;
    CURLcode    res;
    std::string readBuffer;

    curl = curl_easy_init();
    if (curl) {
      curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, RestApi::WriteCallback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
      res = curl_easy_perform(curl);
      curl_easy_cleanup(curl);

      if (res == CURLE_OK) {
        try {
          json data   = json::parse(readBuffer);
          auto result = build_result(data);
          update(block_id_, result.c_str());
          store_last_sync(std::move(result));
        } catch (json::parse_error &e) {
          std::cerr << "JSON parsing error: " << e.what() << std::endl;
        }
      } else {
        std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res)
                  << std::endl;
      }
    }
    start_timer(refresh_interval_);
  }

 protected:
  virtual std::string build_url()                    = 0;
  virtual std::string build_result(const json &data) = 0;

 public:
  void start()
  {
    url_ = build_url();
    if (url_.empty()) return;
    int  wait_for = 0;
    auto diff     = get_now() - last_sync_.first;
    if (last_sync_.first != 0ul && diff < refresh_interval_) {
      update(block_id_, last_sync_.second.c_str());
      wait_for = refresh_interval_ - diff;
      std::cout << block_name_ << " block will be updated in " << wait_for
                << " seconds" << std::endl;
    }
    start_timer(wait_for);
  }

  RestApi(
    boost::asio::io_context &ioc,
    BlockId                  block_id,
    std::string_view         block_name,
    std::string_view         cache_file,
    int                      refresh_interval)
      : Waitable {ioc},
        block_id_ {block_id},
        block_name_ {block_name},
        cache_file_ {cache_file},
        refresh_interval_ {refresh_interval}
  {
    std::ifstream ifs(cache_file_);
    if (ifs.is_open()) {
      getline(ifs, last_sync_.second);
      ifs >> last_sync_.first;
      std::cout << last_sync_.second << std::endl;
      std::cout << last_sync_.first << std::endl;
      auto now = get_now();
      std::cout << "Last " << block_name_ << " request: " << last_sync_.second
                << ", " << now - last_sync_.first << " seconds ago"
                << std::endl;
      ifs.close();
    } else {
      last_sync_.first = 0ul;
    }
  }
};

class Weather : public RestApi
{
  static constexpr auto block_name        = "Weather";
  static constexpr auto sync_interval_sec = 3600;
  static constexpr auto cache_file = "/home/nikita/.cache/statusbar/weather";

 protected:
  virtual std::string build_url() override
  {
    // MY_LOCATION has the format "latitude,longitude"
    // Example: "37.7749,-122.4194"
    const char *location = getenv("MY_LOCATION");
    if (!location) {
      std::cerr
        << "`MY_LOCATION` environment variable is not set. Weather block "
           "will not be used."
        << std::endl;
      return "";
    }

    std::string location_str(location);
    std::string latitude  = location_str.substr(0, location_str.find(","));
    std::string longitude = location_str.substr(location_str.find(",") + 1);

    // TODO: add humudity, wind speed, etc.
    return "https://api.open-meteo.com/v1/forecast?latitude=" + latitude +
           "&longitude=" + longitude + "&current_weather=true";
  }

  virtual std::string build_result(
    const json &data) override
  {
    double temperature = data["current_weather"]["temperature"];
    /*
      Temperature    Color        Hex Code
      ❄️ Freezing    Dark Blue    #1E90FF (Dodger Blue)
      🥶 Cold        Light Blue   #00BFFF (Deep Sky Blue)
      🌿 Cool        Light Green  #32CD32 (Lime Green)
      😊 Mild        Yellow       #FFD700 (Golden Yellow)
      🌡️ Warm        Orange       #FFA500 (Bright Orange)
      🔥 Very Hot    Red          #FF4500 (Orange-Red)
    */
    auto choose_icon   = [](double temperature) {
      if (temperature < 0) return "^c#1e90ff^ \uf2cb";   // empty
      if (temperature < 10) return "^c#00bfff^ \uf2ca";  // quarted
      if (temperature < 18) return "^c#32cd32^ \uf2c9";  // half
      if (temperature < 22) return "^c#ffd700^ \uf2c8";  // three quarters
      if (temperature < 30) return "^c#ffa500^ \uf2c7";  // full
      return "^c#ff4500^ 󰈸";
    };
    return fmt::format(
      "{} {:.{}f}°C", choose_icon(temperature), temperature, 1);
  }

 public:
  Weather(
    boost::asio::io_context &ioc)
      : RestApi(ioc, BlockId::Weather, block_name, cache_file, sync_interval_sec)
  {
  }
};

class ExchangeRate : public RestApi
{
  static constexpr auto block_name        = "ExchangeRate";
  static constexpr auto sync_interval_sec = 3600;
  static constexpr auto cache_file =
    "/home/nikita/.cache/statusbar/exchange_rate";
  static constexpr auto base_currency = "USD";
  static constexpr char rates[][4]    = {"TRY", "RUB"};

 protected:
  virtual std::string build_url() override
  {
    const char *api_key = getenv("OPENEXCHANGERATES_API_KEY");
    if (!api_key) {
      std::cerr << "`OPENEXCHANGERATES_API_KEY` environment variable is not "
                   "set. ExchangeRate block "
                   "will not be used."
                << std::endl;
      return "";
    }
    using std::string_literals::operator""s;
    return "https://openexchangerates.org/api/latest.json?app_id="s + api_key;
  }

  virtual std::string build_result(
    const json &data) override
  {
    // TODO: handle errors and possible crashes during parsing
    // if unexpected response is received
    std::string result;
    for (const auto &rate : rates) {
      double value = data["rates"][rate];
      result += fmt::format(" ^c#07d7e8^{} ^c#10bbbb^{:.2f}", rate, value);
    }
    return result;
  }

 public:
  ExchangeRate(
    boost::asio::io_context &ioc)
      : RestApi(ioc, BlockId::ExchangeRate, block_name, cache_file, sync_interval_sec)
  {
  }
};

class Battery
{
  std::atomic_bool exit_flag_ = 0;
  int              event_fd_  = -1;
  const char      *get_battery_icon(
         const char *str)
  {
    static const char battery_icon[][16] = {
      "^c#ff0000^ \uf244 ",
      "^c#eb9634^ \uf243 ",
      "^c#ebd334^ \uf242 ",
      "^c#c6eb34^ \uf241 ",
      "^c#00ff00^ \uf240 ",
    };
    int value = atoi(str);
    if (value < 10) return battery_icon[0];
    if (value >= 10 && value < 25) return battery_icon[1];
    if (value >= 25 && value < 50) return battery_icon[2];
    if (value >= 50 && value < 75) return battery_icon[3];
    return battery_icon[4];
  }

  const char *get_bolt(
    const char *str)
  {
    return strcmp(str, "Charging") == 0 ? " ^c#cccccc^\uf0e7" : "";
  }

  void init()
  {
    char  str[MAX_BLOCK_SIZE];
    char  capacity[5] = "---";
    FILE *fp          = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (fp) {
      fgets(capacity, 5, fp);
      int len = (int)ftell(fp);
      if (len > 1) {
        capacity[len - 1] = '\0';
        memcpy(str, get_battery_icon(capacity), 16 - 1);
      }
      fclose(fp);
    }
    const char *charging = "";
    {
      FILE *fp = fopen("/sys/class/power_supply/BAT0/status", "r");
      if (fp) {
        char temp[64];
        fgets(temp, 64, fp);
        int len       = (int)ftell(fp);
        temp[len - 1] = '\0';
        charging      = get_bolt(temp);
        if (strlen(charging) > 0) is_cable_plugged = 1;
        fclose(fp);
      }
    }
    sprintf(str + 16 - 1, "%s%%%s", capacity, charging);
    update(BlockId::Battery, str);
  }

  void monitor_battery()
  {
    struct udev         *udev;
    struct udev_device  *dev;
    struct udev_monitor *mon;

    udev = udev_new();
    if (!udev) {
      fprintf(stderr, "Can't create udev\n");
      return;
    }

    mon = udev_monitor_new_from_netlink(udev, "udev");
    if (udev_monitor_filter_add_match_subsystem_devtype(
          mon, "power_supply", NULL) != 0) {
      fprintf(stderr,
              "udev_monitor_filter_add_match_subsystem_devtype() failed.");
      return;
    }
    udev_monitor_enable_receiving(mon);

    struct pollfd items[2];
    items[0].fd      = udev_monitor_get_fd(mon);
    items[0].events  = POLLIN;
    items[0].revents = 0;

    event_fd_ = eventfd(0, EFD_NONBLOCK);
    if (event_fd_ == -1) {
      perror("eventfd");
      return;
    }

    items[1].fd     = event_fd_;
    items[1].events = POLLIN;

    const char *bolt      = "";
    char        level[16] = "";
    while (!exit_flag_) {
      int ret = poll(items, 2, -1);
      if (ret < 0) {
        perror("poll");
        break;
      }
      if (items[1].revents & POLLIN) {
        uint64_t val;
        read(event_fd_, &val, sizeof(val));  // Clear eventfd
        printf("Received SIGINT, exiting...\n");
        break;
      }
      if (items[0].revents & POLLIN) {
        dev = udev_monitor_receive_device(mon);
        if (dev) {
          /*for (struct udev_list_entry *list =
          udev_device_get_properties_list_entry(dev); list != NULL; list =
          udev_list_entry_get_next(list)) { printf("%s: %s\n",
          udev_list_entry_get_name(list), udev_list_entry_get_value(list));
          }*/
          const char *capacity =
            udev_device_get_property_value(dev, "POWER_SUPPLY_CAPACITY");
          const char *status =
            udev_device_get_property_value(dev, "POWER_SUPPLY_STATUS");
          const char *online =
            udev_device_get_property_value(dev, "POWER_SUPPLY_ONLINE");
          if (online) {
            is_cable_plugged = (strcmp(online, "1") == 0);
          }
          if (status) {
            bolt = !is_cable_plugged ? "" : get_bolt(status);
          }
          if (capacity) {
            strcpy(level, capacity);
          }
          if (capacity || status) {
            static char str[MAX_BLOCK_SIZE];
            memcpy(str, get_battery_icon(level), 16 - 1);
            sprintf(str + 16 - 1, "%s%%%s", level, bolt);
            update(BlockId::Battery, str);
          }
          udev_device_unref(dev);
        }
      }
    }

    close(event_fd_);
    udev_monitor_unref(mon);
    udev_unref(udev);

    exit_flag_ = 0;
    event_fd_  = -1;
  }

 public:
  void run()
  {
    if (exit_flag_) return;
    init();
    monitor_battery();
  }
  void stop()
  {
    if (!exit_flag_) return;
    exit_flag_   = 1;
    uint64_t val = 1;
    write(event_fd_, &val, sizeof(val));  // Wake up poll()
  }
};

int main()
{
  if (0 != (errno = pthread_mutex_init(&mutex, NULL))) {
    perror("pthread_mutex_init() failed");
    return EXIT_FAILURE;
  }

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "Cannot open display\n");
    return EXIT_FAILURE;
  }
  screen = DefaultScreen(dpy);
  root   = RootWindow(dpy, screen);

  boost::asio::io_context ioc;

  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work(
    boost::asio::make_work_guard(ioc));

  const int                            MAX_THREADS = 2;
  std::array<std::thread, MAX_THREADS> threads;
  for (auto &t : threads) t = std::thread([&ioc]() { ioc.run(); });

  Date         date(ioc);
  Memory       memory(ioc);
  Weather      weather(ioc);
  ExchangeRate exchange(ioc);
  Battery      battery;

  date.start();
  memory.start();
  weather.start();
  exchange.start();

  boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
  signals.async_wait([&](boost::system::error_code const &, int) {
    battery.stop();
    date.cancel_timer();
    memory.cancel_timer();
    weather.cancel_timer();
    exchange.cancel_timer();
    ioc.stop();
  });

  battery.run();  // blocking

  for (auto &t : threads) t.join();

  return 0;
}
