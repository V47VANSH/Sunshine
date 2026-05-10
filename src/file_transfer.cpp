/**
 * @file src/file_transfer.cpp
 * @brief Implementation of file transfer API endpoints for Moonlight clients.
 *
 * Provides endpoints for browsing PC disks/folders, downloading files,
 * and uploading files. Served on the same HTTPS port (47984) with the
 * same TLS client-certificate authentication as the GameStream API.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "file_transfer.h"
#include "logging.h"

#ifdef _WIN32
  #include <windows.h>
#elif defined(__APPLE__)
  #include <sys/mount.h>
  #include <sys/param.h>
#else
  #include <sys/statvfs.h>
#endif

using namespace std::literals;
namespace fs = std::filesystem;

namespace file_transfer {

  /**
   * @brief Send a JSON response with the given status code.
   */
  void
  send_json(const resp_https_t &response, const nlohmann::json &json,
            SimpleWeb::StatusCode status = SimpleWeb::StatusCode::success_ok) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(status, json.dump(), headers);
    response->close_connection_after_response = true;
  }

  /**
   * @brief Send a JSON error response.
   */
  void
  send_error(const resp_https_t &response, const std::string &message,
             SimpleWeb::StatusCode status = SimpleWeb::StatusCode::client_error_bad_request) {
    nlohmann::json err;
    err["error"] = message;
    send_json(response, err, status);
  }

  /**
   * @brief Validate that a path is absolute and contains no ".." traversal.
   */
  bool
  validate_path(const std::string &path_str) {
    if (path_str.empty()) return false;
    fs::path p(path_str);
    if (!p.is_absolute()) return false;
    for (const auto &component : p) {
      if (component == "..") return false;
    }
    return true;
  }

  /**
   * @brief Get a query parameter value, or empty string if absent.
   */
  std::string
  get_param(const SimpleWeb::CaseInsensitiveMultimap &args, const std::string &name) {
    auto it = args.find(name);
    if (it != args.end()) return it->second;
    return "";
  }

  /**
   * @brief Convert a filesystem time_point to Unix epoch milliseconds.
   */
  int64_t
  to_epoch_millis(const fs::file_time_type &ftime) {
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L && !defined(__APPLE__)
    auto sys_time = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             sys_time.time_since_epoch())
      .count();
#else
    // Portable fallback: compute the offset between file_clock and system_clock
    auto file_now = fs::file_time_type::clock::now();
    auto sys_now = std::chrono::system_clock::now();
    auto sys_time = sys_now + (ftime - file_now);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             sys_time.time_since_epoch())
      .count();
#endif
  }

  void
  disks(resp_https_t response, req_https_t request) {
    BOOST_LOG(debug) << "FileTransfer: disks request"sv;

    nlohmann::json result;
    nlohmann::json disks_array = nlohmann::json::array();

#ifdef _WIN32
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
      if (!(drives & (1 << i))) continue;

      char drive_letter = 'A' + i;
      std::string root_path = std::string(1, drive_letter) + ":\\";

      UINT drive_type = GetDriveTypeA(root_path.c_str());
      if (drive_type == DRIVE_NO_ROOT_DIR) continue;

      nlohmann::json disk;
      disk["name"] = std::string(1, drive_letter) + ":";

      char label[MAX_PATH + 1] = {};
      if (GetVolumeInformationA(root_path.c_str(), label, sizeof(label),
                                nullptr, nullptr, nullptr, nullptr, 0)) {
        disk["label"] = std::string(label);
      } else {
        disk["label"] = "";
      }

      ULARGE_INTEGER free_bytes, total_bytes;
      if (GetDiskFreeSpaceExA(root_path.c_str(), nullptr, &total_bytes, &free_bytes)) {
        disk["totalSpace"] = static_cast<int64_t>(total_bytes.QuadPart);
        disk["freeSpace"] = static_cast<int64_t>(free_bytes.QuadPart);
      } else {
        disk["totalSpace"] = 0;
        disk["freeSpace"] = 0;
      }

      disks_array.push_back(disk);
    }
#elif defined(__APPLE__)
    // macOS: use getmntinfo() to enumerate mounted filesystems
    struct statfs *mnt_buf = nullptr;
    int mnt_count = getmntinfo(&mnt_buf, MNT_NOWAIT);

    static const std::unordered_set<std::string> skip_types = {
      "devfs", "autofs", "nullfs", "vmhgfs", "synthfs"
    };

    for (int i = 0; i < mnt_count; ++i) {
      std::string fstype(mnt_buf[i].f_fstypename);
      std::string mount_point(mnt_buf[i].f_mntonname);

      if (skip_types.count(fstype)) continue;
      if (mount_point.starts_with("/System/") || mount_point.starts_with("/dev")) continue;

      nlohmann::json disk;
      disk["name"] = mount_point;
      disk["label"] = (mount_point == "/") ? "Macintosh HD" : fs::path(mount_point).filename().string();
      disk["totalSpace"] = static_cast<int64_t>(mnt_buf[i].f_blocks) * static_cast<int64_t>(mnt_buf[i].f_bsize);
      disk["freeSpace"] = static_cast<int64_t>(mnt_buf[i].f_bfree) * static_cast<int64_t>(mnt_buf[i].f_bsize);
      disks_array.push_back(disk);
    }
#else
    // Linux: parse /proc/mounts for real filesystems
    std::vector<std::string> mount_points;

    std::ifstream mounts("/proc/mounts");
    if (mounts.is_open()) {
      std::string line;
      while (std::getline(mounts, line)) {
        std::istringstream iss(line);
        std::string device, mount_point, fstype;
        iss >> device >> mount_point >> fstype;

        // Skip virtual/pseudo filesystems
        static const std::unordered_set<std::string> skip_types = {
          "proc", "sysfs", "devtmpfs", "tmpfs", "cgroup", "cgroup2",
          "devpts", "securityfs", "debugfs", "hugetlbfs", "mqueue",
          "pstore", "configfs", "fusectl", "binfmt_misc", "autofs",
          "efivarfs", "tracefs", "bpf", "overlay"
        };
        if (skip_types.count(fstype)) continue;

        // Skip system mount paths
        if (mount_point.starts_with("/snap/") || mount_point.starts_with("/sys/") ||
            mount_point.starts_with("/proc/") || mount_point.starts_with("/dev/") ||
            mount_point.starts_with("/run/")) {
          continue;
        }

        if (!mount_point.empty()) {
          mount_points.push_back(mount_point);
        }
      }
    }

    // Ensure root is always present
    if (std::find(mount_points.begin(), mount_points.end(), "/") == mount_points.end()) {
      mount_points.insert(mount_points.begin(), "/");
    }

    for (const auto &mp : mount_points) {
      struct statvfs stat;
      if (statvfs(mp.c_str(), &stat) == 0) {
        nlohmann::json disk;
        disk["name"] = mp;
        disk["label"] = (mp == "/") ? "Root" : fs::path(mp).filename().string();
        disk["totalSpace"] = static_cast<int64_t>(stat.f_blocks) * static_cast<int64_t>(stat.f_frsize);
        disk["freeSpace"] = static_cast<int64_t>(stat.f_bfree) * static_cast<int64_t>(stat.f_frsize);
        disks_array.push_back(disk);
      }
    }
#endif

    result["disks"] = disks_array;
    send_json(response, result);
  }

  void
  list(resp_https_t response, req_https_t request) {
    BOOST_LOG(debug) << "FileTransfer: list request"sv;

    auto args = request->parse_query_string();
    auto path_str = get_param(args, "path");

    if (!validate_path(path_str)) {
      send_error(response, "Invalid or non-absolute path");
      return;
    }

    fs::path dir_path(path_str);
    std::error_code ec;

    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
      send_error(response, "Directory not found",
                 SimpleWeb::StatusCode::client_error_not_found);
      return;
    }

    nlohmann::json result;
    result["path"] = dir_path.string();
    nlohmann::json items = nlohmann::json::array();

    for (auto it = fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator();
         it.increment(ec)) {
      try {
        auto status = it->status();
        bool is_dir = fs::is_directory(status);
        bool is_file = fs::is_regular_file(status);
        if (!is_dir && !is_file) continue;

        nlohmann::json item;
        item["name"] = it->path().filename().string();
        item["isDirectory"] = is_dir;

        if (is_dir) {
          item["size"] = 0;
        } else {
          std::error_code size_ec;
          item["size"] = static_cast<int64_t>(fs::file_size(it->path(), size_ec));
          if (size_ec) item["size"] = 0;
        }

        std::error_code time_ec;
        auto ftime = fs::last_write_time(it->path(), time_ec);
        item["lastModified"] = time_ec ? 0 : to_epoch_millis(ftime);

        items.push_back(item);
      } catch (const fs::filesystem_error &) {
        // Skip entries we can't access
      }
    }

    // Sort: directories first, then files, both alphabetical (case-insensitive)
    std::sort(items.begin(), items.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
      bool a_dir = a["isDirectory"].get<bool>();
      bool b_dir = b["isDirectory"].get<bool>();
      if (a_dir != b_dir) return a_dir;

      auto a_name = a["name"].get<std::string>();
      auto b_name = b["name"].get<std::string>();
      std::transform(a_name.begin(), a_name.end(), a_name.begin(), ::tolower);
      std::transform(b_name.begin(), b_name.end(), b_name.begin(), ::tolower);
      return a_name < b_name;
    });

    result["items"] = items;
    send_json(response, result);
  }

  void
  download(resp_https_t response, req_https_t request) {
    BOOST_LOG(debug) << "FileTransfer: download request"sv;

    auto args = request->parse_query_string();
    auto path_str = get_param(args, "path");

    if (!validate_path(path_str)) {
      send_error(response, "Invalid or non-absolute path");
      return;
    }

    fs::path file_path(path_str);
    std::error_code ec;

    if (!fs::exists(file_path, ec) || !fs::is_regular_file(file_path, ec)) {
      send_error(response, "File not found",
                 SimpleWeb::StatusCode::client_error_not_found);
      return;
    }

    auto file_size = fs::file_size(file_path, ec);
    if (ec) {
      send_error(response, "Cannot determine file size",
                 SimpleWeb::StatusCode::server_error_internal_server_error);
      return;
    }

    std::ifstream in(file_path, std::ios::binary);
    if (!in) {
      send_error(response, "Cannot open file",
                 SimpleWeb::StatusCode::server_error_internal_server_error);
      return;
    }

    auto filename = file_path.filename().string();

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/octet-stream");
    headers.emplace("Content-Length", std::to_string(file_size));
    headers.emplace("Content-Disposition", "attachment; filename=\"" + filename + "\"");

    // Stream the file — SimpleWeb handles chunked reading from the ifstream
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  /**
   * @brief Generate a unique file path by appending (1), (2), etc. if the file already exists.
   */
  fs::path
  unique_file_path(const fs::path &dest_dir, const std::string &filename) {
    fs::path target = dest_dir / filename;
    if (!fs::exists(target)) return target;

    auto stem = fs::path(filename).stem().string();
    auto ext = fs::path(filename).extension().string();

    for (int i = 1;; ++i) {
      target = dest_dir / (stem + " (" + std::to_string(i) + ")" + ext);
      if (!fs::exists(target)) return target;
    }
  }

  void
  upload(resp_https_t response, req_https_t request) {
    BOOST_LOG(debug) << "FileTransfer: upload request"sv;

    auto args = request->parse_query_string();
    auto dest_path_str = get_param(args, "destPath");

    if (!validate_path(dest_path_str)) {
      send_error(response, "Invalid or non-absolute destination path");
      return;
    }

    fs::path dest_dir(dest_path_str);
    std::error_code ec;

    if (!fs::exists(dest_dir, ec) || !fs::is_directory(dest_dir, ec)) {
      send_error(response, "Destination directory not found",
                 SimpleWeb::StatusCode::client_error_not_found);
      return;
    }

    // Extract multipart boundary from Content-Type header
    auto content_type_it = request->header.find("Content-Type");
    if (content_type_it == request->header.end()) {
      send_error(response, "Missing Content-Type header");
      return;
    }

    std::string content_type = content_type_it->second;
    auto boundary_pos = content_type.find("boundary=");
    if (boundary_pos == std::string::npos) {
      send_error(response, "Missing multipart boundary");
      return;
    }

    std::string boundary = content_type.substr(boundary_pos + 9);
    if (!boundary.empty() && boundary.front() == '"' && boundary.back() == '"') {
      boundary = boundary.substr(1, boundary.size() - 2);
    }

    // Read request body
    std::string body;
    {
      std::ostringstream ss;
      ss << request->content.rdbuf();
      body = ss.str();
    }

    // Parse multipart: find the first part between boundaries
    std::string delimiter = "--" + boundary;

    auto part_start = body.find(delimiter);
    if (part_start == std::string::npos) {
      send_error(response, "Invalid multipart body");
      return;
    }
    part_start += delimiter.size();
    // Skip \r\n after boundary line
    if (part_start < body.size() && body[part_start] == '\r') ++part_start;
    if (part_start < body.size() && body[part_start] == '\n') ++part_start;

    auto part_end = body.find(delimiter, part_start);
    if (part_end == std::string::npos) {
      send_error(response, "Invalid multipart body");
      return;
    }
    // Remove trailing \r\n before next boundary
    if (part_end >= 2 && body[part_end - 2] == '\r' && body[part_end - 1] == '\n') {
      part_end -= 2;
    }

    std::string_view part(body.data() + part_start, part_end - part_start);

    // Split part into headers and data at the \r\n\r\n boundary
    auto header_end = part.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
      send_error(response, "Invalid multipart part");
      return;
    }

    std::string part_headers(part.substr(0, header_end));
    std::string_view file_data = part.substr(header_end + 4);

    // Extract filename from Content-Disposition header
    std::string filename;
    auto cd_pos = part_headers.find("filename=\"");
    if (cd_pos != std::string::npos) {
      cd_pos += 10;
      auto cd_end = part_headers.find('"', cd_pos);
      if (cd_end != std::string::npos) {
        filename = part_headers.substr(cd_pos, cd_end - cd_pos);
      }
    }

    if (filename.empty()) {
      send_error(response, "Missing filename in upload");
      return;
    }

    // Sanitize: use only the filename component, no directory traversal
    filename = fs::path(filename).filename().string();
    if (filename.empty() || filename == "." || filename == "..") {
      send_error(response, "Invalid filename");
      return;
    }

    fs::path final_path = unique_file_path(dest_dir, filename);

    std::ofstream out(final_path, std::ios::binary);
    if (!out) {
      send_error(response, "Cannot create file",
                 SimpleWeb::StatusCode::server_error_internal_server_error);
      return;
    }
    out.write(file_data.data(), static_cast<std::streamsize>(file_data.size()));
    out.close();

    BOOST_LOG(info) << "FileTransfer: uploaded " << final_path;

    nlohmann::json result;
    result["success"] = true;
    result["path"] = final_path.string();
    send_json(response, result);
  }

}  // namespace file_transfer
