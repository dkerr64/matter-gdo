/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */
#pragma once

// C/C++ language includes
#include <string>

// ESP system includes
#include <nvs_flash.h>

constexpr const char cfg_logLevel[] = "logLevel";

constexpr const char nvram_id_code[] = "id_code";
constexpr const char nvram_rolling[] = "rolling";
constexpr const char nvram_open_history[] = "open_history";
constexpr const char nvram_close_history[] = "close_history";

class nvRamClass {
private:
  nvs_handle_t nvHandle;
  static nvRamClass *instancePtr;
  nvRamClass();

public:
  nvRamClass(const nvRamClass &obj) = delete;
  static nvRamClass *getInstance() { return instancePtr; };

  void checkStats();
  int32_t read(const std::string &constKey, const int32_t dflt);
  int32_t read(const std::string &constKey) { return read(constKey, (int32_t)0); };
  std::string read(const std::string &constKey, const char *dflt);
  bool write(const std::string &constKey, const int32_t value, bool commit);
  bool write(const std::string &constKey, const int32_t value) { return write(constKey, value, true); };
  bool write(const std::string &constKey, const char *value, bool commit);
  bool write(const std::string &constKey, const char *value) { return write(constKey, value, true); };
  bool writeBlob(const std::string &constKey, const void *value, size_t size, bool commit);
  bool writeBlob(const std::string &constKey, const void *value, size_t size) { return writeBlob(constKey, value, size, true); };
  bool readBlob(const std::string &constKey, void *value, size_t size);
  bool erase(const std::string &constKey);
  void erase();
};
extern nvRamClass *nvRam;
#define read_nvs_int nvRam->read
#define write_nvs_int nvRam->write
#define read_nvs_str nvRam->read
#define write_nvs_str nvRam->write
#define read_nvs_data nvRam->readBlob
#define write_nvs_data nvRam->writeBlob
#define erase_nvs_data nvRam->erase
