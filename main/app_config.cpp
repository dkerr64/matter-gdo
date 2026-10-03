/****************************************************************************
 *
 * Copyright (c) 2023-26 David A Kerr... https://github.com/dkerr64/
 * All Rights Reserved.
 * Licensed under terms of the GPL-3.0 License.
 *
 */

// C/C++ language includes
#include <string>

// ESP-IDF include files...
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

// Our project include files...
#include "app_config.h"

static constexpr char *TAG = "app_config";

// Construct the singleton classes for user config
nvRamClass *nvRamClass::instancePtr = new nvRamClass();
nvRamClass *nvRam = nvRamClass::getInstance();

/****************************************************************************
 * NVRAM class
 */
nvRamClass::nvRamClass()
{
  ESP_LOGI(TAG, "Constructor for NVRAM class");
  // Initialize non volatile ram
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    // NVS partition was truncated and needs to be erased
    // Retry nvs_flash_init
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  err = nvs_open("matter-gdo", NVS_READWRITE, &nvHandle);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    nvHandle = 0;
  }
}

void nvRamClass::checkStats()
{
  nvs_stats_t nvs_stats;
  if (esp_err_t err = nvs_get_stats(NULL, &nvs_stats) == ESP_OK)
  {
    ESP_LOGI(TAG, "NVRAM Stats... UsedEntries = (%lu), FreeEntries = (%lu), TotalEntries = (%lu), Count = (%lu)\n", nvs_stats.used_entries, nvs_stats.free_entries, nvs_stats.total_entries,
             nvs_stats.namespace_count);
  }
  else
  {
    ESP_LOGE(TAG, "Error return from nvs_get_stats: %d", err);
  }
}

int32_t nvRamClass::read(const std::string &constKey, const int32_t dflt)
{
  std::string key = constKey;
  if (key.length() >= NVS_KEY_NAME_MAX_SIZE)
    key.resize(NVS_KEY_NAME_MAX_SIZE - 1); // allow for null terminator

  int32_t value = dflt;
  esp_err_t err = nvs_get_i32(nvHandle, key.c_str(), &value);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
  {
    ESP_LOGE(TAG, "NVRAM get error for: %s (%s)", key.c_str(), esp_err_to_name(err));
  }
  return value;
}

std::string nvRamClass::read(const std::string &constKey, const char *dflt)
{
  std::string key = constKey;
  if (key.length() >= NVS_KEY_NAME_MAX_SIZE)
    key.resize(NVS_KEY_NAME_MAX_SIZE - 1); // allow for null terminator

  std::string value(dflt);
  size_t len;
  esp_err_t err = nvs_get_str(nvHandle, key.c_str(), NULL, &len);
  if (err == ESP_OK)
  {
    char *buf = static_cast<char *>(malloc(len));
    if (nvs_get_str(nvHandle, key.c_str(), buf, &len) == ESP_OK)
    {
      value = buf;
    }
    free(buf);
  }
  else if (err != ESP_ERR_NVS_NOT_FOUND)
  {
    ESP_LOGE(TAG, "NVRAM get error for: %s (%s)", key.c_str(), esp_err_to_name(err));
  }
  return value;
}

bool nvRamClass::write(const std::string &constKey, const int32_t value, bool commit)
{
  std::string key = constKey;
  if (key.length() >= NVS_KEY_NAME_MAX_SIZE)
    key.resize(NVS_KEY_NAME_MAX_SIZE - 1); // allow for null terminator

  esp_err_t err = nvs_set_i32(nvHandle, key.c_str(), value);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVRAM set error for: %s (%s)", key.c_str(), esp_err_to_name(err));
    return false;
  }
  if (commit)
  {
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
  }
  return true;
}

bool nvRamClass::readBlob(const std::string &constKey, void *value, size_t size)
{
  std::string key = constKey;
  if (key.length() >= NVS_KEY_NAME_MAX_SIZE)
    key.resize(NVS_KEY_NAME_MAX_SIZE - 1); // allow for null terminator

  esp_err_t err = nvs_get_blob(nvHandle, key.c_str(), value, &size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVRAM get error for: %s (%s)", key.c_str(), esp_err_to_name(err));
    return false;
  }
  return true;
}

bool nvRamClass::writeBlob(const std::string &constKey, const void *value, size_t size, bool commit)
{
  std::string key = constKey;
  if (key.length() >= NVS_KEY_NAME_MAX_SIZE)
    key.resize(NVS_KEY_NAME_MAX_SIZE - 1); // allow for null terminator

  esp_err_t err = nvs_set_blob(nvHandle, key.c_str(), value, size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVRAM set error for: %s (%s)", key.c_str(), esp_err_to_name(err));
    return false;
  }
  if (commit)
  {
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
  }
  return true;
}

bool nvRamClass::write(const std::string &constKey, const char *value, bool commit)
{
  std::string key = constKey;
  if (key.length() >= NVS_KEY_NAME_MAX_SIZE)
    key.resize(NVS_KEY_NAME_MAX_SIZE - 1); // allow for null terminator

  esp_err_t err = nvs_set_str(nvHandle, key.c_str(), value);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVRAM set error for: %s (%s)", key.c_str(), esp_err_to_name(err));
    return false;
  }
  if (commit)
  {
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
  }
  return true;
}

bool nvRamClass::erase(const std::string &constKey)
{
  std::string key = constKey;
  if (key.length() >= NVS_KEY_NAME_MAX_SIZE)
    key.resize(NVS_KEY_NAME_MAX_SIZE - 1); // allow for null terminator

  esp_err_t err = nvs_erase_key(nvHandle, key.c_str());
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVRAM erase error for: %s (%s)", key.c_str(), esp_err_to_name(err));
    return false;
  }
  ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
  return true;
}

void nvRamClass::erase()
{
  esp_err_t err = nvs_erase_all(nvHandle);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVRAM erase_all error: %s", esp_err_to_name(err));
    return;
  }
  ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_commit(nvHandle));
  return;
}