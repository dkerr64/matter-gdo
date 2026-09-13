/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */

// System build includes
#include <sdkconfig.h>

#ifdef CONFIG_GDO_ENABLE_SYSLOG

// C/C++ language includes
#include <algorithm>

// ESP-IDF include files...
#include <esp_netif.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <nvs_flash.h>

#include <lib/support/logging/CHIPLogging.h>

// Our project include files...
#include "app_log.h"
#include "app_utils.h"

static constexpr char *TAG = "app_log";
bool app_log_init_done = false;
vprintf_like_t old_vprintf = nullptr;

typedef struct syslogServer {
  uint16_t port;
  esp_ip4_addr_t IPv4;
} syslogServer;
static syslogServer syslogSrv = {};
char nvs_key_syslogSrv[] = "syslogSrv";
extern nvs_handle_t nvHandle;
static bool syslogDiscovered = false;

#define LOG_BUFFER_SIZE (1024 * 16)
#define LINE_BUFFER_SIZE 512

typedef struct logBuffer {
  uint32_t wrapped;
  uint32_t head;
  uint32_t tail;                                                                // points to the start of valid data in the buffer
  char buffer[LOG_BUFFER_SIZE - sizeof(wrapped) - sizeof(head) - sizeof(tail)]; // sized so whole struct is LOG_BUFFER_SIZE bytes
} logBuffer;
static logBuffer *msgBuffer = NULL; // Circular buffer to save log messages as they occur
char *lineBuffer = NULL;            // Buffer to hold a single line of log message
SemaphoreHandle_t logMutex = NULL;
TaskHandle_t syslogTask = NULL;

/****************************************************************************
 *
 * We are rolling our own mDNS query to find a syslog service on the local network.
 * This is necessary because including the ESP mDNS library causes the firmware size
 * to exceed available flash memory.
 *
 */
static constexpr uint16_t kMdnsPort = 5353;
static constexpr const char *kMdnsAddr = "224.0.0.251";
static constexpr int kQueryTimeoutMs = 3000;
static constexpr size_t kMaxRecords = 8;
static constexpr uint16_t kDnsTypePtr = 12;
static constexpr uint16_t kDnsTypeA = 1;
static constexpr uint16_t kDnsTypeSrv = 33;

struct SrvRecord {
  char owner[128];
  char target[128];
  uint16_t port;
};

struct ARecord {
  char name[128];
  uint32_t addr;
};

// Encode a dotted name (e.g. "_syslog._udp.local") into DNS wire format.
size_t encode_name(const char *name, uint8_t *out, size_t out_size)
{
  size_t pos = 0;
  const char *label = name;
  while (*label)
  {
    const char *dot = strchr(label, '.');
    size_t len = dot ? (size_t)(dot - label) : strlen(label);
    if (len > 63 || pos + len + 1 >= out_size)
      return 0;
    out[pos++] = (uint8_t)len;
    memcpy(&out[pos], label, len);
    pos += len;
    label += len;
    if (dot)
      label++;
    else
      break;
  }
  if (pos + 1 >= out_size)
    return 0;
  out[pos++] = 0;
  return pos;
}

// Decode a (possibly compressed) DNS name at offset. Returns bytes consumed
// from offset (following compression pointers transparently), or 0 on error.
size_t decode_name(const uint8_t *msg, size_t msg_len, size_t offset, char *out, size_t out_size)
{
  size_t pos = offset;
  size_t out_pos = 0;
  size_t consumed = 0;
  bool jumped = false;
  int jumps = 0;

  while (pos < msg_len)
  {
    uint8_t len = msg[pos];
    if ((len & 0xC0) == 0xC0)
    {
      if (pos + 1 >= msg_len || ++jumps > 10)
        return 0;
      size_t ptr = ((len & 0x3F) << 8) | msg[pos + 1];
      if (!jumped)
        consumed = pos + 2 - offset;
      jumped = true;
      pos = ptr;
      continue;
    }
    if (len == 0)
    {
      pos++;
      if (!jumped)
        consumed = pos - offset;
      break;
    }
    if (pos + 1 + len >= msg_len || out_pos + len + 1 >= out_size)
      return 0;
    if (out_pos)
      out[out_pos++] = '.';
    memcpy(&out[out_pos], &msg[pos + 1], len);
    out_pos += len;
    pos += len + 1;
  }
  out[out_pos] = '\0';
  return consumed;
}

// Parse one mDNS response packet, looking for a PTR answer for want_service and
// its matching SRV/A records, logging whatever is found.
void parse_mdns_response(const uint8_t *msg, size_t len, const char *want_service)
{
  if (len < 12)
    return;

  uint16_t qdcount = (msg[4] << 8) | msg[5];
  uint16_t ancount = (msg[6] << 8) | msg[7];
  uint16_t nscount = (msg[8] << 8) | msg[9];
  uint16_t arcount = (msg[10] << 8) | msg[11];

  size_t pos = 12;
  // Static: these buffers are too large for the discovery task's stack, and
  // this function only ever runs from that single task, so reuse is safe.
  static char name[256];
  static char ptr_target[256];
  static SrvRecord srv[kMaxRecords];
  static ARecord a_rec[kMaxRecords];
  ptr_target[0] = '\0';
  size_t srv_count = 0;
  size_t a_count = 0;

  // Skip the question section.
  for (int i = 0; i < qdcount && pos < len; i++)
  {
    size_t n = decode_name(msg, len, pos, name, sizeof(name));
    if (!n)
      return;
    pos += n + 4; // qtype + qclass
  }

  int total_rr = ancount + nscount + arcount;
  for (int i = 0; i < total_rr && pos < len; i++)
  {
    size_t n = decode_name(msg, len, pos, name, sizeof(name));
    if (!n)
      return;
    pos += n;
    if (pos + 10 > len)
      return;

    uint16_t type = (msg[pos] << 8) | msg[pos + 1];
    pos += 2 + 2 + 4; // skip class and ttl
    uint16_t rdlen = (msg[pos] << 8) | msg[pos + 1];
    pos += 2;
    if (pos + rdlen > len)
      return;
    size_t rdata_offset = pos;

    if (type == kDnsTypePtr && ptr_target[0] == '\0')
    {
      decode_name(msg, len, rdata_offset, ptr_target, sizeof(ptr_target));
    }
    else if (type == kDnsTypeSrv && srv_count < kMaxRecords && rdlen >= 6)
    {
      uint16_t port = (msg[rdata_offset + 4] << 8) | msg[rdata_offset + 5];
      static char target[128];
      if (decode_name(msg, len, rdata_offset + 6, target, sizeof(target)))
      {
        strncpy(srv[srv_count].owner, name, sizeof(srv[srv_count].owner) - 1);
        strncpy(srv[srv_count].target, target, sizeof(srv[srv_count].target) - 1);
        srv[srv_count].port = port;
        srv_count++;
      }
    }
    else if (type == kDnsTypeA && rdlen == 4 && a_count < kMaxRecords)
    {
      memcpy(&a_rec[a_count].addr, &msg[rdata_offset], 4);
      strncpy(a_rec[a_count].name, name, sizeof(a_rec[a_count].name) - 1);
      a_count++;
    }
    pos = rdata_offset + rdlen;
  }

  if (ptr_target[0] == '\0')
  {
    ESP_LOGI(TAG, "No PTR answer found for %s", want_service);
    return;
  }

  ESP_LOGI(TAG, "Found service instance: %s", ptr_target);
  for (size_t i = 0; i < srv_count; i++)
  {
    if (strcmp(srv[i].owner, ptr_target) != 0)
      continue;
    ESP_LOGI(TAG, "  Host: %s  Port: %u", srv[i].target, srv[i].port);
    for (size_t j = 0; j < a_count; j++)
    {
      if (strcmp(a_rec[j].name, srv[i].target) != 0)
        continue;
      esp_ip4_addr_t ip4 = {.addr = a_rec[j].addr};
      ESP_LOGI(TAG, "    IPv4: " IPSTR, IP2STR(&ip4));
      syslogSrv.IPv4 = ip4;
      syslogSrv.port = srv[i].port;
      syslogDiscovered = true;

      esp_err_t err = nvs_set_blob(nvHandle, nvs_key_syslogSrv, (void *)&syslogSrv, sizeof(syslogSrv));
      if (err != ESP_OK)
      {
        ESP_LOGE(TAG, "NVRAM set error for: syslogSrv (%s)", esp_err_to_name(err));
        return;
      }
      ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
      break;
    }
  }
}

// Sends a one-shot mDNS PTR query (e.g. service_name="_syslog", proto="_udp") over a raw
// multicast UDP socket and logs any matching instance/host/port/address found in replies.
// This avoids linking the full ESP-IDF mdns component, which is intentionally excluded from
// this build (CONFIG_USE_MINIMAL_MDNS) to keep image size within the OTA partition budget.
void find_mdns_service(const char *service_name, const char *proto)
{
  char query_name[128];
  snprintf(query_name, sizeof(query_name), "%s.%s.local", service_name, proto);
  ESP_LOGI(TAG, "Querying PTR record for: %s", query_name);

  in_addr_t wifi_if_addr = htonl(INADDR_ANY);
  esp_netif_t *wifi_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info = {};
  if (wifi_netif && esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
  {
    wifi_if_addr = ip_info.ip.addr;
  }
  else
  {
    ESP_LOGI(TAG, "Wi-Fi STA interface has no IP yet, falling back to default interface");
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0)
  {
    ESP_LOGW(TAG, "Failed to create socket (errno %d)", errno);
    return;
  }

  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &wifi_if_addr, sizeof(wifi_if_addr));

  struct sockaddr_in local_addr = {};
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons(kMdnsPort);
  local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
  {
    ESP_LOGW(TAG, "Failed to bind socket (errno %d)", errno);
    close(sock);
    return;
  }

  struct ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = inet_addr(kMdnsAddr);
  mreq.imr_interface.s_addr = wifi_if_addr;
  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
  {
    ESP_LOGW(TAG, "Failed to join multicast group (errno %d)", errno);
    close(sock);
    return;
  }

  // Build and send the PTR query packet.
  uint8_t packet[256] = {0};
  // Header: ID=0, flags=0 (standard query), QDCOUNT=1, rest=0.
  packet[5] = 1;
  size_t name_len = encode_name(query_name, &packet[12], sizeof(packet) - 12);
  if (!name_len)
  {
    ESP_LOGW(TAG, "Query name too long: %s", query_name);
    close(sock);
    return;
  }
  size_t pos = 12 + name_len;
  packet[pos++] = 0x00;
  packet[pos++] = kDnsTypePtr; // QTYPE = PTR
  packet[pos++] = 0x00;
  packet[pos++] = 0x01; // QCLASS = IN

  struct sockaddr_in dest_addr = {};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(kMdnsPort);
  dest_addr.sin_addr.s_addr = inet_addr(kMdnsAddr);
  if (sendto(sock, packet, pos, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0)
  {
    ESP_LOGW(TAG, "Failed to send query (errno %d)", errno);
    close(sock);
    return;
  }

  // Collect replies until the timeout expires.
  uint8_t rx_buf[512];
  _millis_t start = _millis();
  bool found_any = false;
  while (true)
  {
    _millis_t elapsed = _millis() - start;
    if (elapsed >= (_millis_t)kQueryTimeoutMs)
      break;

    struct timeval tv = {};
    int remaining_ms = kQueryTimeoutMs - (int)elapsed;
    tv.tv_sec = remaining_ms / 1000;
    tv.tv_usec = (remaining_ms % 1000) * 1000;

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock, &read_fds);
    int sel = select(sock + 1, &read_fds, NULL, NULL, &tv);
    if (sel <= 0)
      break;

    int n = recv(sock, rx_buf, sizeof(rx_buf), 0);
    if (n <= 0)
      continue;

    found_any = true;
    parse_mdns_response(rx_buf, (size_t)n, query_name);
  }

  if (!found_any)
  {
    ESP_LOGI(TAG, "No services found for %s", query_name);
  }

  close(sock);
}

void syslog_discovery_task(void *arg)
{
  constexpr int kSyslogDiscoveryIntervalMs = 10000;         // retry every 10 seconds until a syslog server is found
  constexpr int kSyslogDiscoveryTimeoutMs = 10 * 60 * 1000; // Abort if not found within 10 minutes
  syslogDiscovered = false;

  // Attempt to read the syslog server information from NVS.
  size_t size = sizeof(syslogSrv);
  esp_err_t err = nvs_get_blob(nvHandle, nvs_key_syslogSrv, (void *)&syslogSrv, &size);
  if (err != ESP_OK)
  {
    ESP_LOGI(TAG, "No syslog server info in NVS: %s", esp_err_to_name(err));
    syslogSrv.port = 0;
  }
  else
  {
    ESP_LOGI(TAG, "Read syslog server info from NVS successfully");
    ESP_LOGI(TAG, "    IPv4: " IPSTR, IP2STR(&syslogSrv.IPv4));
    ESP_LOGI(TAG, "    Port: %u", syslogSrv.port);
  }

  _millis_t start = _millis();
  while (!syslogDiscovered)
  {
    find_mdns_service("_syslog", "_udp");
    if (syslogDiscovered)
      break;
    if (_millis() - start >= (_millis_t)kSyslogDiscoveryTimeoutMs)
      break;
    vTaskDelay(pdMS_TO_TICKS(kSyslogDiscoveryIntervalMs));
  }
  if (syslogDiscovered)
  {
    ESP_LOGI(TAG, "Syslog discovery task exiting, server found (stack high water mark: %u bytes)", uxTaskGetStackHighWaterMark(NULL));
  }
  else
  {
    ESP_LOGW(TAG, "Syslog discovery task exiting, server not found (stack high water mark: %u bytes)", uxTaskGetStackHighWaterMark(NULL));
  }
  vTaskDelete(NULL);
}

// Runs find_mdns_service() on a background task
void start_syslog_discovery()
{
  // find_mdns_service()/parse_mdns_response() use sizeable buffers, hence the large stack.
  xTaskCreate(syslog_discovery_task, "syslog_disc", 8192, NULL, tskIDLE_PRIORITY + 1, NULL);
}

/****************************************************************************
 *
 * We hook the loging functions so we can keep a rolling buffer of log messages
 * and forward them to a syslog server.
 *
 */

#define SYSLOG_EMERGENCY 0
#define SYSLOG_ALERT 1
#define SYSLOG_CRIT 2
#define SYSLOG_ERROR 3
#define SYSLOG_WARN 4
#define SYSLOG_NOTICE 5
#define SYSLOG_INFO 6
#define SYSLOG_DEBUG 7
#define SYSLOG_NIL "-"
#define SYSLOG_BOM "\xEF\xBB\xBF"

void syslog_send(char *message, size_t msgLen)
{
  static int syslog_sock = -1;

  if (syslogSrv.port == 0)
    return;

  if (syslog_sock < 0)
  {
    syslog_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (syslog_sock < 0)
    {
      esp_rom_printf("%s: Failed to create syslog socket\n", TAG);
      return;
    }
    int flags = fcntl(syslog_sock, F_GETFL, 0);
    fcntl(syslog_sock, F_SETFL, flags | O_NONBLOCK);
  }

  // Syslog messages must not include a trailing newline.
  while (msgLen > 0 && (message[msgLen - 1] == '\n' || message[msgLen - 1] == '\r'))
    message[--msgLen] = '\0';
  if (msgLen == 0)
    return;

  // RFC 3164 PRI: facility local0 (16), severity derived from the log level letter.
  uint8_t syslogPri = 16 * 8;
  if (*message == '>' || *message == 'I')
    syslogPri += SYSLOG_INFO;
  else if (*message == '!' || *message == 'E')
    syslogPri += SYSLOG_ERROR;
  else if (*message == 'W')
    syslogPri += SYSLOG_WARN;
  else if (*message == 'D' || *message == 'V')
    syslogPri += SYSLOG_DEBUG;

  char header[64];

  uint16_t hdrLen = snprintf(header, sizeof(header), "<%u>1 - %s matter-gdo - - - ", syslogPri, deviceIPv4String);

  struct sockaddr_in dest_addr = {};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(syslogSrv.port);
  dest_addr.sin_addr.s_addr = syslogSrv.IPv4.addr;

  struct iovec iov[2] = {};
  iov[0].iov_base = (void *)header;
  iov[0].iov_len = hdrLen;
  iov[1].iov_base = (void *)message;
  iov[1].iov_len = msgLen;

  struct msghdr msg = {};
  msg.msg_name = &dest_addr;
  msg.msg_namelen = sizeof(dest_addr);
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;

  sendmsg(syslog_sock, &msg, 0);
  vTaskDelay(pdMS_TO_TICKS(20)); // Avoid flooding the network stack
}

// Task responsible for sending log messages to the syslog server. It waits for
// notifications and processes the rolling log buffer, handling wrapped messages
// and sending them via syslog_send().
void syslog_sender_task(void *arg)
{
  char buf[512] = {};
  static bool started = false;

  while (true)
  {
    if (xTaskNotifyWait(0, 0, NULL, portMAX_DELAY) != pdTRUE)
      continue;

    // No point in continuing if the syslog server port is not set
    if (!syslogSrv.port)
      continue;

    // we need to handle the case where the buffer has wrapped and we haven't started yet (because
    // no syslog port set, so we didn't remove any messages from the buffer)
    if (!started && msgBuffer->wrapped)
    {
      xSemaphoreTakeRecursive(logMutex, portMAX_DELAY);
      // When the buffer has wrapped, the oldest data begins immediately
      // after head. Start there and process to the physical end of the buffer,
      // then continue from buffer[0].
      msgBuffer->tail = msgBuffer->head + 1;
      xSemaphoreGiveRecursive(logMutex);
      while (msgBuffer->tail < sizeof(msgBuffer->buffer))
      {
        xSemaphoreTakeRecursive(logMutex, portMAX_DELAY);
        char *p = &msgBuffer->buffer[std::min(static_cast<size_t>(msgBuffer->tail), sizeof(msgBuffer->buffer) - 1)];
        size_t maxLen = sizeof(msgBuffer->buffer) - msgBuffer->tail;
        char *nl = static_cast<char *>(memchr(p, '\n', maxLen));
        // If no newline is found, we can't determine the end of the line.
        if (!nl)
        {
          xSemaphoreGiveRecursive(logMutex);
          break;
        }

        size_t l = nl - p + 1;
        // abort if the message would exceed the buffer size
        if (msgBuffer->tail + l <= sizeof(msgBuffer->buffer))
        {
          memcpy(buf, p, std::min(l, sizeof(buf) - 1));
          buf[std::min(l, sizeof(buf) - 1)] = '\0';
          msgBuffer->tail += l;
        }
        else
        {
          xSemaphoreGiveRecursive(logMutex);
          break;
        }

        xSemaphoreGiveRecursive(logMutex);
        syslog_send(buf, l);
      }
    }

    started = true;
    xSemaphoreTakeRecursive(logMutex, portMAX_DELAY);
    bool shouldContinue = (msgBuffer->tail < msgBuffer->head);
    xSemaphoreGiveRecursive(logMutex);
    while (shouldContinue)
    {
      xSemaphoreTakeRecursive(logMutex, portMAX_DELAY);
      char *p = &msgBuffer->buffer[std::min(static_cast<size_t>(msgBuffer->tail), sizeof(msgBuffer->buffer) - 1)];
      char *nl = static_cast<char *>(memchr(p, '\n', msgBuffer->head - msgBuffer->tail));
      if (!nl)
      {
        // If no newline is found, we can't determine the end of the line.
        xSemaphoreGiveRecursive(logMutex);
        break;
      }
      size_t l = std::min(static_cast<size_t>(nl - p + 1), sizeof(buf) - 1);
      memcpy(buf, p, l);
      buf[l] = '\0';
      msgBuffer->tail += l;
      shouldContinue = msgBuffer->tail < msgBuffer->head;
      xSemaphoreGiveRecursive(logMutex);
      syslog_send(buf, l);
    }

    xSemaphoreTakeRecursive(logMutex, portMAX_DELAY);
    // If the tail has surpassed the head, it means we've wrapped on the buffer size;
    if (msgBuffer->tail > msgBuffer->head)
    {
      char *p = &msgBuffer->buffer[std::min(static_cast<size_t>(msgBuffer->tail), sizeof(msgBuffer->buffer) - 1)];
      char *nl = static_cast<char *>(memchr(msgBuffer->buffer, '\n', msgBuffer->head));
      if (!nl)
      {
        // If no newline is found, we can't determine the end of the line.
        xSemaphoreGiveRecursive(logMutex);
        continue; // go back and wait on xTaskNotifyWait()
      }
      size_t firstPartLen = sizeof(msgBuffer->buffer) - msgBuffer->tail;
      size_t secondPartLen = nl - msgBuffer->buffer + 1;
      size_t totalLen = firstPartLen + secondPartLen;
      // Limit the message copied to buf.
      size_t copyLen = std::min(totalLen, sizeof(buf) - 1);
      size_t firstCopyLen = std::min(firstPartLen, copyLen);
      size_t secondCopyLen = copyLen - firstCopyLen;
      memcpy(buf, p, firstCopyLen);
      if (secondCopyLen > 0)
        memcpy(buf + firstCopyLen, msgBuffer->buffer, secondCopyLen);
      buf[copyLen] = '\0';
      // The newline tells us how much of the circular buffer
      // was actually consumed, regardless of truncation.
      msgBuffer->tail = static_cast<uint32_t>(secondPartLen);
      xSemaphoreGiveRecursive(logMutex);
      syslog_send(buf, copyLen);
    }
    else
    {
      xSemaphoreGiveRecursive(logMutex);
    }
  }
}

// Hook function for ESP logging. Captures log messages, stores them in a rolling buffer,
// and notifies the syslog task when a complete line is available.
int esp_log_hook(const char *fmt, va_list args)
{
  if (!app_log_init_done)
    return 0;


  // protect access to the rolling log buffer with a recursive mutex.
  xSemaphoreTakeRecursive(logMutex, portMAX_DELAY);
  vsnprintf(lineBuffer, LINE_BUFFER_SIZE, fmt, args);

  int written = strlen(lineBuffer);
  if (written == 0)
  {
    xSemaphoreGiveRecursive(logMutex);
    return 0;
  }
#ifdef CONFIG_GDO_LOG_TIMESTAMP
  // If timestamp is wrapped in () and not [] then message is from one of the ESP_LOGx() functions.
  // Convert the milliseconds into HH:MM:SS.mmm so it is easier to read.
  if (strchr(lineBuffer, '(') - lineBuffer == 2)
  {
    char *numptr = &lineBuffer[3];
    char *endptr;
    uint32_t num = strtoll(numptr, &endptr, 10);
    const char *ts = toHHMMSSmmm(num);
    uint32_t timestrlen = strlen(ts);
    int32_t diff = timestrlen - (int32_t)(endptr - numptr);
    int32_t max = LINE_BUFFER_SIZE - ((int32_t)(endptr - lineBuffer) + diff);
    memmove(endptr + diff, endptr, max);
    memcpy(numptr, ts, timestrlen);
    // Make sure we always end in a newline/null... which can get missed if we truncated the log message
    lineBuffer[LINE_BUFFER_SIZE - 2] = '\n';
    lineBuffer[LINE_BUFFER_SIZE - 1] = 0;    
  }
  size_t len = strlen(lineBuffer);
#else
  size_t len = written;
#endif
  write(STDOUT_FILENO, lineBuffer, len);

  constexpr size_t bufferSize = sizeof(msgBuffer->buffer);
  size_t available = bufferSize - msgBuffer->head - 1;
  memcpy(&msgBuffer->buffer[msgBuffer->head], lineBuffer, std::min(available, len));
  if (available < len)
  {
    // we wrapped on the available buffer space
    msgBuffer->wrapped = 1;
    msgBuffer->head = len - available;
    memcpy(msgBuffer->buffer, &lineBuffer[available], msgBuffer->head);
  }
  else
  {
    msgBuffer->head += len;
  }
  msgBuffer->buffer[msgBuffer->head] = 0; // null terminate

  // Only notify the syslog task if the line ends with a newline character.  Some logs come in without a newline at the end and
  // are immediately followed by another log message, so we only notify when a full line is available.
  if (lineBuffer[len - 1] == '\n')
  {
    xTaskNotify(syslogTask, 0, eSetValueWithOverwrite);
  }

  xSemaphoreGiveRecursive(logMutex);

  return written;
}

// Redirect callback for CHIP logging. Formats the log message and forwards it to the ESP logging system.
void chipLogger(const char *module, uint8_t category, const char *msg, va_list args)
{
  char tag[16];
  char formatted[512];

  snprintf(tag, sizeof(tag), "chip[%s]", module);
  vsnprintf(formatted, sizeof(formatted), msg, args);
  switch (category)
  {
  case chip::Logging::LogCategory::kLogCategory_Error:
    ESP_LOGE(tag, "%s", formatted);
    break;
  case chip::Logging::LogCategory::kLogCategory_Progress:
    ESP_LOGI(tag, "%s", formatted);
    break;
  case chip::Logging::LogCategory::kLogCategory_Detail:
    ESP_LOGD(tag, "%s", formatted);
    break;
  default:
    ESP_LOGI(tag, "%s", formatted);
    break;
  }
  return;
}

void app_log_init()
{
  if (app_log_init_done)
    return;

  logMutex = xSemaphoreCreateRecursiveMutex();
  lineBuffer = static_cast<char *>(malloc(LINE_BUFFER_SIZE));
  msgBuffer = static_cast<logBuffer *>(malloc(sizeof(logBuffer)));
  memset(msgBuffer->buffer, 0, sizeof(msgBuffer->buffer));
  msgBuffer->wrapped = 0;
  msgBuffer->head = 0;
  msgBuffer->tail = 0;

  xTaskCreate(syslog_sender_task, "syslog_send", 4096, NULL, tskIDLE_PRIORITY + 1, &syslogTask);

  esp_log_level_set("*", ESP_LOG_INFO);
  old_vprintf = esp_log_set_vprintf(esp_log_hook);
  chip::Logging::SetLogRedirectCallback(chipLogger);

  app_log_init_done = true;
}

#endif // CONFIG_GDO_ENABLE_SYSLOG
