/**
 * @file src/file_transfer.h
 * @brief Declarations for the file transfer API endpoints.
 */
#pragma once

#include <Simple-Web-Server/server_https.hpp>

#include "nvhttp.h"

namespace file_transfer {

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>::Request>;

  /**
   * @brief GET /file-transfer/disks — Lists all mounted drives/volumes.
   */
  void disks(resp_https_t response, req_https_t request);

  /**
   * @brief GET /file-transfer/list — Lists contents of a directory.
   */
  void list(resp_https_t response, req_https_t request);

  /**
   * @brief GET /file-transfer/download — Downloads a file from the PC.
   */
  void download(resp_https_t response, req_https_t request);

  /**
   * @brief POST /file-transfer/upload — Uploads a file to the PC.
   */
  void upload(resp_https_t response, req_https_t request);

}  // namespace file_transfer
