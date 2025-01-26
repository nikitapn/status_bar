#include <stdlib.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libudev.h>
#include <X11/Xlib.h>
#include <pthread.h>
#include <errno.h>

#define MAX_BLOCK_SIZE 128
#define MAX_STATUS_LEN 1024
#define LEFT_BORDER "^c#dddddd^["
#define RIGHT_BORDER "^c#dddddd^]"


enum Block : int {
  Battery, Memory, Date, MAX_BLOCKS
};

static Display *dpy;
static int screen;
static Window root;
static pthread_mutex_t mutex;

struct BlockData {
  int len;
  char data[MAX_BLOCK_SIZE];
};

static void update(enum Block bix, const char* str) {
  static char status [MAX_STATUS_LEN] = "";
  static struct BlockData blocks[MAX_BLOCKS] = { 0 };

  pthread_mutex_lock(&mutex);

  const int blen = strlen(str);
  blocks[bix].len = blen;
  memcpy(blocks[bix].data, str, blen);

  char *ptr = status;
  memcpy(ptr, LEFT_BORDER, sizeof(LEFT_BORDER) - 1);
  ptr += sizeof(LEFT_BORDER) - 1;
  for (int i = 0; i < MAX_BLOCKS; ++i) {
    const int len = blocks[i].len;
    if (!len) continue;
    memcpy(ptr, blocks[i].data, len);
    ptr += len;
  }

  memcpy(ptr, RIGHT_BORDER, sizeof(RIGHT_BORDER));

  XStoreName(dpy, root, status);

  pthread_mutex_unlock(&mutex);
  XFlush(dpy);
}

static char months[][4] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
}; 

static char days[][4] = {
  "Sun", "Mon", "Tue", "Wen", "Thu", "Fri", "Sat"
}; 

static void* time_thread(void* /* params */) {
  for (;;) {
    static char str[MAX_BLOCK_SIZE];
    time_t tm = time(NULL);
    struct tm *date = localtime(&tm);
    sprintf(str, " ^c#c7d7e8^\uf017  ^c#bbbbbb^%s %s %d %s %.02d:%.02d ",
      days[date->tm_wday], 
      months[date->tm_mon],
      date->tm_mday,
      (date->tm_hour >= 8 && date->tm_hour < 21 ? "^c#edd238^\uf185" : "^c#ecede8^\uf186"),
      date->tm_hour, 
      date->tm_min
    );

    update(Date, str);
    
    sleep(60 - date->tm_sec + 1);
  }

  return NULL;
}

static const char battery_icon [][16] = { 
  "^c#ff0000^ \uf244 ",
  "^c#eb9634^ \uf243 ",
  "^c#ebd334^ \uf242 ",
  "^c#c6eb34^ \uf241 ",
  "^c#00ff00^ \uf240 ",
};

static const char* get_battery_icon(const char* str) {
  int value = atoi(str);
  if (value < 10) return battery_icon[0];
  if (value >= 10 && value < 25) return battery_icon[1];
  if (value >= 25 && value < 50) return battery_icon[2];
  if (value >= 50 && value < 75) return battery_icon[3];
  return battery_icon[4];
}

static void* memory_status(void* /* params */) {
  for (;;) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
      perror("fopen");
    }

    char line[256];
    unsigned long memTotal = 0, memFree = 0, buffers = 0, cached = 0, memReclaimable = 0;

    for (int i = 0; fgets(line, sizeof(line), file);) {
      if (i == 5) break;
      if (
          (sscanf(line, "MemTotal: %lu kB", &memTotal) == 1) ||
          (sscanf(line, "MemFree: %lu kB", &memFree) == 1)   ||
          (sscanf(line, "Buffers: %lu kB", &buffers) == 1)   ||
          (sscanf(line, "Cached: %lu kB", &cached) == 1)     ||
          (sscanf(line, "SReclaimable: %lu kB", &memReclaimable) == 1)
           ) ++i;
    }

    fclose(file);

    unsigned long memCachedAll = cached + memReclaimable;

    char result[MAX_BLOCK_SIZE];
    sprintf(result, "^c#186da5^ \uf2db %.1fGB", (memTotal - memFree - memCachedAll) / 1000.0f / 1000.f);
    update(Memory, result);

    sleep(5);
  }
  return NULL;
}


static void populate_with_initial_values() {
  { // init battery status
    static char str[MAX_BLOCK_SIZE];
    char capacity [5] = "---";
    FILE* fp = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (fp) {
      fgets(capacity, 5, fp);
      int len = (int)ftell(fp);
      if (len > 1) {
        capacity[len - 1] = '\0'; 
        memcpy(str, get_battery_icon(capacity), sizeof(battery_icon[0]) - 1);
      }
      fclose(fp);
    }
    sprintf(str + sizeof(battery_icon[0]) - 1, " %s%%", capacity);
    update(Battery, str);
  }
}

static int monitor_battery() {
  struct udev *udev;
  struct udev_device *dev;
  struct udev_monitor *mon;

  udev = udev_new();
  if (!udev) {
    fprintf(stderr, "Can't create udev\n");
    return EXIT_FAILURE;
  }

  mon = udev_monitor_new_from_netlink(udev, "udev");
  if (udev_monitor_filter_add_match_subsystem_devtype(mon, "power_supply", NULL) != 0) {
    fprintf(stderr, "udev_monitor_filter_add_match_subsystem_devtype() failed.");
    return EXIT_FAILURE;
  }
  udev_monitor_enable_receiving(mon);

  struct pollfd items[1];
  items[0].fd = udev_monitor_get_fd(mon);
  items[0].events = POLLIN;
  items[0].revents = 0;

  while (poll(items, 1, -1) > 0) {
    dev = udev_monitor_receive_device(mon);
    if (dev) {
      /*for (struct udev_list_entry *list = udev_device_get_properties_list_entry(dev);
        list != NULL;
        list = udev_list_entry_get_next(list)) {
        printf("%s: %s\n", udev_list_entry_get_name(list), udev_list_entry_get_value(list));
      }*/
      const char *capacity = udev_device_get_property_value(dev, "POWER_SUPPLY_CAPACITY");
      if (capacity) {
        static char str[MAX_BLOCK_SIZE];
        memcpy(str, get_battery_icon(capacity), sizeof(battery_icon[0]) - 1);
        sprintf(str + sizeof(battery_icon[0]) - 1, " %s%%", capacity);
        update(Battery, str);
      }
      udev_device_unref(dev);
    }
  }

  udev_unref(udev);

  return EXIT_SUCCESS;
}

int main() {
  if (0 != (errno = pthread_mutex_init(&mutex, NULL))) {
    perror("pthread_mutex_init() failed");
    return EXIT_FAILURE;
  }

  dpy = XOpenDisplay(NULL);
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);

  populate_with_initial_values();

  pthread_t thread_time, thread_memory;
  pthread_create(&thread_time, NULL, &time_thread, NULL);
  pthread_create(&thread_memory, NULL, &memory_status, NULL);

  return monitor_battery();
}
