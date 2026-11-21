/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTP server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <lizardbyte/common/env.h>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

#if defined(_WIN32) || defined(__APPLE__)
  #include "platform/virtualhid_input.h"
#endif

#ifdef _WIN32
  #include "platform/windows/misc.h"
  #include "platform/windows/utf_utils.h"

  #include <Windows.h>
#elif defined(__APPLE__)
  #include "platform/macos/misc.h"

  #include <CoreFoundation/CoreFoundation.h>
#endif

// local includes
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "input.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "platform/permissions.h"
#include "process.h"
#include "rtsp.h"
#include "system_tray.h"
#include "utility.h"
#include "uuid.h"

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  /**
   * @brief HTTPS server type used for Sunshine's configuration UI.
   */
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;

  /**
   * @brief Case-insensitive map used for HTTP headers and query parameters.
   */
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  /**
   * @brief Shared HTTPS response object passed to configuration handlers.
   */
  using resp_https_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  /**
   * @brief Shared HTTPS request object received by configuration handlers.
   */
  using req_https_t = std::shared_ptr<SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  /**
   * @brief Handler signature for configuration UI HTTPS routes.
   */
  using https_handler_t = std::function<void(resp_https_t, req_https_t)>;

  namespace {
    using license_status_provider_t = std::function<lvh::LicenseResult()>;  ///< Provider for the current libvirtualhid license status.
#ifdef SUNSHINE_TESTS
    std::optional<nlohmann::json> permission_status_override;  ///< Deterministic permission statuses for HTTP tests.
#endif
#if defined(linux) || defined(__FreeBSD__) || defined(SUNSHINE_TESTS)
    using portal_token_path_provider_t = std::function<fs::path()>;  ///< Provider for the XDG Portal token path.
#endif

    /**
     * @brief Return the current libvirtualhid license status provider.
     *
     * Unit-test builds expose a mutable provider so the HTTP fixture can avoid
     * contacting an installed Windows broker. Production builds keep the
     * provider const and always call libvirtualhid directly.
     *
     * @return License status provider for the current build.
     */
    auto &virtual_input_license_status_provider() {
#ifdef SUNSHINE_TESTS
      static license_status_provider_t status_provider = lvh::get_license_status;
#else
      static const license_status_provider_t status_provider = lvh::get_license_status;
#endif
      return status_provider;
    }

#if defined(linux) || defined(__FreeBSD__) || defined(SUNSHINE_TESTS)
    /**
     * @brief Return the path provider for the saved XDG Portal restore token.
     *
     * @return Path provider for the current build.
     */
    auto &portal_token_path_provider() {
  #ifdef SUNSHINE_TESTS
      static portal_token_path_provider_t path_provider = []() {
        return platf::get_xdg_restore_token_path();
      };
  #else
      static const portal_token_path_provider_t path_provider = []() {
        return platf::get_xdg_restore_token_path();
      };
  #endif
      return path_provider;
    }
#endif
  }  // namespace

  /**
   * @brief Client certificate operations accepted by the configuration API.
   */
  enum class op_e {
    ADD,  ///< Add client
    REMOVE  ///< Remove client
  };

  /**
   * @brief Overwrite a request-local sensitive string when leaving scope.
   */
  class scoped_sensitive_string_clear_t {
  public:
    /**
     * @brief Register a sensitive string for best-effort clearing.
     *
     * @param value Mutable sensitive string.
     */
    explicit scoped_sensitive_string_clear_t(std::string &value):
        value_ {value} {}

    scoped_sensitive_string_clear_t(const scoped_sensitive_string_clear_t &) = delete;
    scoped_sensitive_string_clear_t &operator=(const scoped_sensitive_string_clear_t &) = delete;

    /**
     * @brief Overwrite and clear the registered string.
     */
    ~scoped_sensitive_string_clear_t() {
      std::fill(value_.begin(), value_.end(), '\0');
      value_.clear();
    }

  private:
    std::string &value_;  ///< Sensitive request-local string.
  };

#ifdef SUNSHINE_TESTS
  void set_virtual_input_license_status_provider_for_testing(virtual_input_license_status_provider_t status_provider) {
    virtual_input_license_status_provider() = std::move(status_provider);
  }

  void reset_virtual_input_license_status_provider_for_testing() {
    virtual_input_license_status_provider() = lvh::get_license_status;
  }

  void set_portal_token_path_provider_for_testing(confighttp::portal_token_path_provider_t path_provider) {
    portal_token_path_provider() = std::move(path_provider);
  }

  void reset_portal_token_path_provider_for_testing() {
    portal_token_path_provider() = []() {
      return platf::get_xdg_restore_token_path();
    };
  }

  void clear_sensitive_string_for_testing(std::string &value) {
    const scoped_sensitive_string_clear_t clear_value {value};
  }
#endif

  // CSRF token management
  /**
   * @brief CSRF token value and its expiration deadline.
   */
  struct csrf_token_t {
    std::string token;  ///< Random token value that must be echoed by the client.
    std::chrono::steady_clock::time_point expiration;  ///< Monotonic deadline after which the token is rejected.
  };

  std::map<std::string, csrf_token_t, std::less<>> csrf_tokens;  ///< CSRF tokens by client identifier. NOSONAR(cpp:S5421) - intentionally mutable global
  std::mutex csrf_tokens_mutex;  ///< Mutex protecting CSRF token storage. NOSONAR(cpp:S5421) - intentionally mutable global

  // CSRF token configuration
  /**
   * @brief Number of random bytes used when generating a CSRF token.
   */
  constexpr auto CSRF_TOKEN_SIZE = 32;  // 32 bytes = 256 bits
  /**
   * @brief Amount of time a generated CSRF token remains valid.
   */
  constexpr auto CSRF_TOKEN_LIFETIME = std::chrono::hours(1);  // Tokens valid for 1 hour

#ifndef __APPLE__
  constexpr std::string_view libvirtualhid_minimum_version = LIBVIRTUALHID_MINIMUM_VERSION;  ///< Minimum supported Windows broker version.
#endif
#ifdef __APPLE__
  constexpr std::string_view libvirtualhid_macos_minimum_version = LIBVIRTUALHID_MACOS_MINIMUM_VERSION;  ///< Minimum supported macOS broker bundle version.
#endif
  constexpr auto VIGEMBUS_MINIMUM_VERSION = "1.17.0.0"sv;  ///< Minimum supported ViGEmBus fallback driver version.  // NOSONAR(cpp:S1313): not an IP address

  /**
   * @brief Parse one dotted driver-version component.
   *
   * @param part Version component text.
   * @return Parsed component value, or empty when invalid.
   */
  std::optional<unsigned int> parse_driver_version_part(std::string_view part) {
    if (part.empty()) {
      return std::nullopt;
    }

    unsigned int value = 0;
    const auto *begin = part.data();
    const auto *end = part.data() + part.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc {} || ptr != end) {
      return std::nullopt;
    }

    return value;
  }

  /**
   * @brief Parse a dotted driver version into numeric components.
   *
   * @param version Driver version text.
   * @return Parsed version parts, or empty when invalid.
   */
  std::optional<std::vector<unsigned int>> parse_driver_version(std::string_view version) {
    if (version.empty()) {
      return std::nullopt;
    }

    std::vector<unsigned int> parts;
    std::size_t start = 0;
    while (start <= version.size()) {
      const auto dot = version.find('.', start);
      const auto length = dot == std::string_view::npos ? std::string_view::npos : dot - start;
      const auto part = parse_driver_version_part(version.substr(start, length));
      if (!part.has_value()) {
        return std::nullopt;
      }

      parts.push_back(*part);
      if (dot == std::string_view::npos) {
        break;
      }
      start = dot + 1;
    }

    return parts;
  }

  bool is_driver_version_development(std::string_view version) {
    const auto version_parts = parse_driver_version(version);
    return version_parts && version_parts->size() >= 3U && (*version_parts)[0] == 0U && (*version_parts)[1] == 0U;
  }

  bool is_driver_version_supported(std::string_view version, std::string_view minimum_version) {
    if (minimum_version.empty() || is_driver_version_development(version)) {
      return true;
    }

    const auto version_parts = parse_driver_version(version);
    const auto minimum_parts = parse_driver_version(minimum_version);
    if (!version_parts || !minimum_parts) {
      return false;
    }

    const auto part_count = std::max(version_parts->size(), minimum_parts->size());
    for (std::size_t i = 0; i < part_count; ++i) {
      const auto version_part = i < version_parts->size() ? (*version_parts)[i] : 0U;
      const auto minimum_part = i < minimum_parts->size() ? (*minimum_parts)[i] : 0U;
      if (version_part != minimum_part) {
        return version_part > minimum_part;
      }
    }

    return true;
  }

  nlohmann::json build_driver_status(bool installed, const std::string &version, std::string_view minimum_version) {
    const auto minimum_version_text = std::string {minimum_version};

    nlohmann::json output_tree;
    output_tree["installed"] = installed;
    output_tree["version"] = version;
    output_tree["minimum_version"] = minimum_version_text;
    output_tree["supported_versions"] = minimum_version.empty() ? "Any" : std::format(">= {}", minimum_version_text);
    output_tree["development_version"] = installed && is_driver_version_development(version);
    output_tree["version_compatible"] = installed && is_driver_version_supported(version, minimum_version);

    return output_tree;
  }

  /**
   * @brief Return a stable Web UI name for a libvirtualhid license state.
   *
   * @param state License state.
   * @return Lowercase state name.
   */
  std::string_view virtualhid_license_state_name(lvh::LicenseState state) {
    using enum lvh::LicenseState;

    switch (state) {
      case unlicensed:
        return "unlicensed";
      case licensed:
        return "licensed";
      case expired:
        return "expired";
      case disabled:
        return "disabled";
      case invalid:
        return "invalid";
      case unavailable:
      default:
        return "unavailable";
    }
  }

  nlohmann::json build_virtualhid_license_status(const lvh::LicenseResult &result) {
    const auto &license = result.license;
    nlohmann::json output_tree;
    output_tree["operation_ok"] = result.status.ok();
    output_tree["service_available"] = license.service_available;
    output_tree["state"] = virtualhid_license_state_name(license.state);
    output_tree["licensed"] = license.licensed();
    output_tree["active_devices"] = license.active_devices;
    output_tree["activation_limit"] = license.activation_limit;
    output_tree["activation_usage"] = license.activation_usage;
    output_tree["plan_name"] = license.plan_name;
    output_tree["customer_email"] = license.customer_email;
    output_tree["message"] = license.message;
    output_tree["purchase_url"] = license.purchase_url;
    output_tree["manage_account_url"] = license.manage_account_url;
    output_tree["error"] = result.status.ok() ? "" : result.status.message();
    return output_tree;
  }

  namespace {
    /**
     * @brief Handle a virtual-input license request using the configured status provider.
     *
     * @param response HTTP response object.
     * @param request Authenticated HTTP request.
     */
    void get_virtual_input_license(const resp_https_t &response, const req_https_t &request) {
      if (!authenticate(response, request)) {
        return;
      }

      print_req(request);
      send_response(response, build_virtualhid_license_status(virtual_input_license_status_provider()()));
    }
  }  // namespace

#ifdef _WIN32
  /**
   * @brief RAII wrapper for a Windows registry key handle.
   */
  class registry_key_t {
  public:
    /**
     * @brief Construct an empty registry key wrapper.
     */
    registry_key_t() = default;

    /**
     * @brief Copy construction is disabled because the wrapper owns a handle.
     */
    registry_key_t(const registry_key_t &) = delete;

    /**
     * @brief Copy assignment is disabled because the wrapper owns a handle.
     *
     * @return This registry key wrapper.
     */
    registry_key_t &operator=(const registry_key_t &) = delete;

    /**
     * @brief Close the owned registry key handle.
     */
    ~registry_key_t() {
      close();
    }

    /**
     * @brief Get the owned registry key handle.
     *
     * @return Registry key handle.
     */
    HKEY get() const {
      return handle;
    }

    /**
     * @brief Prepare the wrapper to receive a registry key handle.
     *
     * @return Address of the wrapped handle.
     */
    HKEY *put() {
      close();
      return &handle;
    }

  private:
    /**
     * @brief Close the owned registry key handle if one is open.
     */
    void close() {
      if (handle) {
        RegCloseKey(handle);
        handle = nullptr;
      }
    }

    HKEY handle = nullptr;  ///< Owned Windows registry key handle.
  };

  /**
   * @brief Read a string value from a Windows registry key.
   *
   * @param key Registry key to query.
   * @param value_name Registry value name.
   * @return Registry string value, or empty when unavailable.
   */
  std::optional<std::wstring> read_registry_string_value(HKEY key, const wchar_t *value_name) {
    DWORD value_type = 0;
    DWORD value_size = 0;
    if (RegGetValueW(key, nullptr, value_name, RRF_RT_REG_SZ, &value_type, nullptr, &value_size) != ERROR_SUCCESS || value_size == 0) {
      return std::nullopt;
    }

    std::wstring value(value_size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, value_name, RRF_RT_REG_SZ, &value_type, value.data(), &value_size) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    while (!value.empty() && value.back() == L'\0') {
      value.pop_back();
    }
    return value;
  }

  /**
   * @brief Read the installed libvirtualhid driver version from the Windows device registry.
   *
   * @return Driver version string, or empty when unavailable.
   */
  std::string read_libvirtualhid_driver_version() {
    registry_key_t root_key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\ROOT\\LIBVIRTUALHID", 0, KEY_READ, root_key.put()) != ERROR_SUCCESS) {
      return {};
    }

    for (DWORD index = 0;; ++index) {
      std::wstring subkey_name(256, L'\0');
      auto subkey_name_size = static_cast<DWORD>(subkey_name.size());
      const auto enum_status = RegEnumKeyExW(root_key.get(), index, subkey_name.data(), &subkey_name_size, nullptr, nullptr, nullptr, nullptr);
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS) {
        continue;
      }

      std::wstring device_key_path = L"SYSTEM\\CurrentControlSet\\Enum\\ROOT\\LIBVIRTUALHID\\";
      device_key_path.append(subkey_name, 0, subkey_name_size);

      registry_key_t device_key;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, device_key_path.c_str(), 0, KEY_READ, device_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      const auto driver_key_suffix = read_registry_string_value(device_key.get(), L"Driver");
      if (!driver_key_suffix) {
        continue;
      }

      std::wstring driver_key_path = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
      driver_key_path += *driver_key_suffix;

      registry_key_t driver_key;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, driver_key_path.c_str(), 0, KEY_READ, driver_key.put()) != ERROR_SUCCESS) {
        continue;
      }

      if (const auto version = read_registry_string_value(driver_key.get(), L"DriverVersion")) {
        return utf_utils::to_utf8(*version);
      }
    }

    return {};
  }

#endif

#ifdef __APPLE__
  /**
   * @brief Read the installed macOS Virtual HID Broker app version.
   *
   * @return Three-part bundle version, or empty when the app is unavailable.
   */
  std::string read_libvirtualhid_broker_version() {
    const auto bundle_url = CFURLCreateWithFileSystemPath(
      kCFAllocatorDefault,
      CFSTR("/Applications/VirtualHIDBroker.app"),
      kCFURLPOSIXPathStyle,
      true
    );
    if (!bundle_url) {
      return {};
    }

    const auto bundle = CFBundleCreate(kCFAllocatorDefault, bundle_url);
    CFRelease(bundle_url);
    if (!bundle) {
      return {};
    }

    std::string version;
    const auto value = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("CFBundleShortVersionString"));
    if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
      std::array<char, 64> buffer {};
      if (CFStringGetCString(static_cast<CFStringRef>(value), buffer.data(), buffer.size(), kCFStringEncodingUTF8)) {
        version = buffer.data();
      }
    }
    CFRelease(bundle);
    return version;
  }
#endif

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(const resp_https_t &response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(const resp_https_t &response, const req_https_t &request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;

    constexpr auto code = SimpleWeb::StatusCode::client_error_unauthorized;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";

    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"WWW-Authenticate", R"(Basic realm="Sunshine Gamestream Host", charset="UTF-8")"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(const resp_https_t &response, const req_https_t &request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Authenticate the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if the user is authenticated, false otherwise.
   */
  bool authenticate(const resp_https_t &response, const req_https_t &request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());

    if (const auto ip_type = net::from_address(address); ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }

    // If credentials are shown, redirect the user to a /welcome page
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }

    auto fg = util::fail_guard([&]() {
      send_unauthorized(response, request);
    });

    const auto auth = request->header.find("authorization");
    if (auth == request->header.end()) {
      return false;
    }

    const auto &rawAuth = auth->second;
    auto authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr("Basic "sv.length()));

    const auto index = static_cast<int>(authData.find(':'));
    if (index >= authData.size() - 1) {
      return false;
    }

    const auto username = authData.substr(0, index);
    const auto password = authData.substr(index + 1);

    if (const auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string(); !boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
      return false;
    }

    fg.disable();
    return true;
  }

  /**
   * @brief Send a 404 Not Found response.
   *
   * @param response The HTTP response object.
   * @param error_message The error message to include in the response.
   */
  void not_found(const resp_https_t &response, const std::string &error_message) {
    constexpr auto code = SimpleWeb::StatusCode::client_error_not_found;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["error"] = error_message;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   *
   * @param response The HTTP response object.
   * @param error_message The error message to include in the response.
   */
  void bad_request(const resp_https_t &response, const std::string &error_message) {
    constexpr auto code = SimpleWeb::StatusCode::client_error_bad_request;

    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = error_message;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Validate the request content type and send a bad request when mismatched.
   */
  bool check_content_type(const resp_https_t &response, const req_https_t &request, const std::string_view &contentType) {
    const auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, "Content type not provided");
      return false;
    }
    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    if (const size_t semicolonPos = actualContentType.find(';'); semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, "Content type mismatch");
      return false;
    }
    return true;
  }

  /**
   * @brief Get a unique client identifier for CSRF token management.
   * @param request The HTTP request object.
   * @return A unique identifier based on username or IP address.
   */
  std::string get_client_id(const req_https_t &request) {
    // Try to use the authenticated username as client ID
    if (const auto auth = request->header.find("authorization"); !config::sunshine.username.empty() && auth != request->header.end()) {
      if (const auto &rawAuth = auth->second; rawAuth.rfind("Basic "sv, 0) == 0) {
        auto authData = SimpleWeb::Crypto::Base64::decode(rawAuth.substr("Basic "sv.length()));
        if (const auto index = static_cast<int>(authData.find(':')); index < authData.size() - 1) {
          return authData.substr(0, index);  // Return username
        }
      }
    }

    // Fall back to IP address if no username
    return net::addr_to_normalized_string(request->remote_endpoint().address());
  }

  /**
   * @brief Generate a new CSRF token for a client.
   * @param client_id A unique identifier for the client (e.g., session ID or username).
   * @return The generated CSRF token.
   */
  std::string generate_csrf_token(const std::string &client_id) {
    // Generate a cryptographically secure random token
    std::string token = crypto::rand_alphabet(CSRF_TOKEN_SIZE);

    std::scoped_lock lock(csrf_tokens_mutex);

    // Clean up expired tokens first
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(csrf_tokens, [&now](const auto &entry) {
      return entry.second.expiration < now;
    });

    // Store the token with expiration
    csrf_tokens[client_id] = csrf_token_t {
      token,
      now + CSRF_TOKEN_LIFETIME
    };

    return token;
  }

  /**
   * @brief Validate a stored CSRF token for a client against a provided token string.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param client_id A unique identifier for the client.
   * @param provided_token The token string to validate.
   * @return True if the token is valid, false otherwise.
   */
  bool validate_stored_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string_view client_id, const std::string_view provided_token) {
    std::scoped_lock lock(csrf_tokens_mutex);
    const auto token_it = csrf_tokens.find(client_id);

    if (token_it == csrf_tokens.end()) {
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: no token found for client"sv;
      bad_request(response, "Invalid CSRF token");
      return false;
    }

    if (const auto now = std::chrono::steady_clock::now(); token_it->second.expiration < now) {
      csrf_tokens.erase(token_it);
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: token expired"sv;
      bad_request(response, "CSRF token expired");
      return false;
    }

    if (token_it->second.token != provided_token) {
      auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
      BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF token validation failed: token mismatch"sv;
      bad_request(response, "Invalid CSRF token");
      return false;
    }

    return true;
  }

  /**
   * @brief Validate CSRF token.
   */
  bool validate_csrf_token(const resp_https_t &response, const req_https_t &request, const std::string &client_id) {
    // Helper function to check if a URL starts with any allowed origin
    auto is_allowed_origin = [](const std::string_view url) {
      return std::ranges::any_of(config::sunshine.csrf_allowed_origins, [&url](const std::string &allowed_origin) {
        // Ensure exact prefix match (with ":" or "/" after to prevent malicious.com matching allowed.com)
        if (url.rfind(allowed_origin, 0) != 0) {  // rfind with pos=0 checks if the url starts with allowed_origin
          return false;
        }
        // Check that it's followed by ":" (port) or "/" (path) or is an exact match
        const size_t len = allowed_origin.length();
        return url.length() == len || url[len] == ':' || url[len] == '/';
      });
    };

    // Check if the request is from the same origin (Origin or Referer header matches configured allowed origins)
    const auto origin_it = request->header.find("Origin");
    if (origin_it != request->header.end() && is_allowed_origin(origin_it->second)) {
      // Same origin request - allow without CSRF token
      return true;
    }

    // If we have a Referer header, check if it's same-origin
    const auto referer_it = request->header.find("Referer");
    if (referer_it != request->header.end() && is_allowed_origin(referer_it->second)) {
      // Same origin request - allow without CSRF token
      return true;
    }

    // If neither Origin nor Referer is present, this cannot be a browser-initiated CSRF attack.
    // Non-browser clients (e.g. curl, scripts) never send these headers, and a malicious web page
    // cannot cause a non-browser client to make requests on a user's behalf.
    if (origin_it == request->header.end() && referer_it == request->header.end()) {
      return true;
    }

    // A browser-like request arrived with an Origin/Referer that doesn't match an allowed origin.
    // Require a CSRF token.
    const std::string_view blocked_origin = (origin_it != request->header.end()) ? origin_it->second : referer_it->second;
    // Extract token from X-CSRF-Token header
    const auto header_it = request->header.find("X-CSRF-Token");
    if (header_it == request->header.end()) {
      // Also check query parameters as fallback
      auto query_params = request->parse_query_string();
      const auto query_it = query_params.find("csrf_token");
      if (query_it == query_params.end()) {
        auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
        BOOST_LOG(error) << "Web UI: ["sv << address << "] -- CSRF protection blocked request from origin: "sv << blocked_origin;
        BOOST_LOG(error) << "Web UI: To allow this origin, add it to the 'csrf_allowed_origins' option in your Sunshine configuration"sv;
        bad_request(response, "Missing CSRF token");
        return false;
      }

      return validate_stored_csrf_token(response, request, client_id, query_it->second);
    }

    // Validate token from header
    return validate_stored_csrf_token(response, request, client_id, header_it->second);
  }

  /**
   * @brief Validates the application index and sends an error response if invalid.
   */
  bool check_app_index(const resp_https_t &response, int index) {
    std::string file = file_handler::read_file(config::stream.file_apps.c_str());
    nlohmann::json file_tree = nlohmann::json::parse(file);
    if (const auto &apps = file_tree["apps"]; index < 0 || index >= static_cast<int>(apps.size())) {
      std::string error_message;
      if (const int max_index = static_cast<int>(apps.size()) - 1; max_index < 0) {
        error_message = "No applications found";
      } else {
        error_message = std::format("'index' {} out of range, max index is {}", index, max_index);
      }
      bad_request(response, error_message);
      return false;
    }
    return true;
  }

  void getPage(const resp_https_t &response, const req_https_t &request, const bool require_auth, const bool redirect_if_username) {
    // Special handling for welcome page: redirect if the username is already set
    if (redirect_if_username && !config::sunshine.username.empty()) {
      send_redirect(response, request, "/");
      return;
    }

    if (require_auth && !authenticate(response, request)) {
      return;
    }

    print_req(request);

    const std::string content = file_handler::read_file(WEB_DIR "index.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");

    // prevent click jacking
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(content, headers);
  }

  void getFallbackPage(const resp_https_t &response, const req_https_t &request) {
    const std::string_view path = request->path;
    const auto has_server_prefix = [path](const std::string_view prefix) {
      return path == prefix || (path.starts_with(prefix) && path.length() > prefix.length() && path[prefix.length()] == '/');
    };

    if (has_server_prefix("/api") || has_server_prefix("/assets") || has_server_prefix("/images")) {
      not_found(response);
      return;
    }

    getPage(response, request);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getSunshineLogoImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getFaviconImage(const resp_https_t &response, const req_https_t &request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/sunshine.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the Sunshine logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getSunshineLogoImage(const resp_https_t &response, const req_https_t &request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-sunshine-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getAsset(const resp_https_t &response, const req_https_t &request) {
    print_req(request);
    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if the file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response);
      return;
    }
    if (!fs::exists(filePath)) {
      not_found(response);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    // check if the extension is in the map at the x position
    if (mimeType == mime_types.end()) {
      bad_request(response);
      return;
    }

    // if it is, set the content type to the mime type
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get a CSRF token for the authenticated user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/csrf-token|:| GET|:| null}
   */
  void getCSRFToken(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string client_id = get_client_id(request);
    std::string token = generate_csrf_token(client_id);

    nlohmann::json output_tree;
    output_tree["csrf_token"] = token;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps|:| GET|:| null}
   */
  void getApps(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      // Legacy versions of Sunshine used strings for boolean and integers, let's convert them
      // List of keys to convert to boolean
      const std::vector<std::string> boolean_keys = {
        "exclude-global-prep-cmd",
        "elevated",
        "auto-detach",
        "wait-all"
      };

      // List of keys to convert to integers
      std::vector<std::string> integer_keys = {
        "exit-timeout"
      };

      // Walk fileTree and convert true/false strings to boolean or integer values
      for (auto &app : file_tree["apps"]) {
        for (const auto &key : boolean_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = app[key] == "true";
          }
        }
        for (const auto &key : integer_keys) {
          if (app.contains(key) && app[key].is_string()) {
            app[key] = std::stoi(app[key].get<std::string>());
          }
        }
        if (app.contains("prep-cmd")) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated") && prep["elevated"].is_string()) {
              prep["elevated"] = prep["elevated"] == "true";
            }
          }
        }
      }

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application, the index must be `-1`. To update an existing application, you must provide the current index of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "index": -1,
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file."
   * }
   * @endcode
   *
   * @api_examples{/api/apps|:| POST|:| {"name":"Hello, World!","index":-1}}
   */
  void saveApp(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      BOOST_LOG(info) << file;
      nlohmann::json file_tree = nlohmann::json::parse(file);

      if (input_tree["prep-cmd"].empty()) {
        input_tree.erase("prep-cmd");
      }

      if (input_tree["detached"].empty()) {
        input_tree.erase("detached");
      }

      auto &apps_node = file_tree["apps"];
      int index = input_tree["index"].get<int>();  // this will intentionally cause an exception if the provided value is the wrong type

      input_tree.erase("index");

      if (index == -1) {
        apps_node.push_back(input_tree);
      } else {
        nlohmann::json newApps = nlohmann::json::array();
        for (size_t i = 0; i < apps_node.size(); ++i) {
          if (i == index) {
            newApps.push_back(input_tree);
          } else {
            newApps.push_back(apps_node[i]);
          }
        }
        file_tree["apps"] = newApps;
      }

      // Sort the apps array by name
      std::sort(apps_node.begin(), apps_node.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
      });

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close|:| POST|:| null}
   */
  void closeApp(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/9999|:| DELETE|:| null}
   */
  void deleteApp(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    try {
      nlohmann::json output_tree;
      nlohmann::json new_apps = nlohmann::json::array();
      const int index = std::stoi(request->path_match[1]);

      if (!check_app_index(response, index)) {
        return;
      }

      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps = file_tree["apps"];

      for (size_t i = 0; i < apps.size(); ++i) {
        if (i != index) {
          new_apps.push_back(apps[i]);
        }
      }
      file_tree["apps"] = new_apps;

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      output_tree["result"] = std::format("application {} deleted", index);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list|:| GET|:| null}
   */
  void getClients(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const nlohmann::json named_certs = nvhttp::get_all_clients();

    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Enable or disable a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "enabled": true
   * }
   * @endcode
   *
   * @api_examples{/api/clients/update|:| POST|:| {"uuid":"<uuid>","enabled":true}}
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }
    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      bool enabled = input_tree.value("enabled", true);
      output_tree["status"] = nvhttp::set_client_enabled(uuid, enabled);

      if (!enabled && output_tree["status"]) {
        auto cert = nvhttp::get_cert_by_uuid(uuid);
        if (!cert.empty()) {
          rtsp_stream::terminate_sessions_by_cert(cert);
        }

        if (rtsp_stream::session_count() == 0 && proc::proc.running() > 0) {
          proc::proc.terminate();
        }
      }

      send_response(response, output_tree);
    } catch (nlohmann::json::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/unpair|:| POST|:| {"uuid":"1234"}}
   */
  void unpair(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string uuid = input_tree.value("uuid", "");
      const bool removed = nvhttp::unpair_client(uuid);
      output_tree["status"] = removed;

      if (removed && nvhttp::get_all_clients().empty()) {
        proc::proc.terminate();
      }

      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all|:| POST|:| null}
   */
  void unpairAll(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();

    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/config|:| GET|:| null}
   */
  void getConfig(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;

    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));

    for (auto &[name, value] : vars) {
      output_tree[name] = std::move(value);
    }

    send_response(response, output_tree);
  }

  /**
   * @brief Get the locale setting. This endpoint does not require authentication.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale|:| GET|:| null}
   */
  void getLocale(const resp_https_t &response, const req_https_t &request) {
    // we need to return the locale whether authenticated or not

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config|:| POST|:| {"key":"value"}}
   */
  void saveConfig(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config, right now
        // we should migrate the config file to straight JSON and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Get an application's image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @note{The index in the url path is the application index.}
   *
   * @api_examples{/api/covers/9999 |:| GET|:| null}
   */
  void getCover(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      const int index = std::stoi(request->path_match[1]);
      if (!check_app_index(response, index)) {
        return;
      }

      std::string file = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(file);
      auto &apps = file_tree["apps"];

      auto &app = apps[index];

      // Get the image path from the app configuration
      std::string app_image_path;
      if (app.contains("image-path") && !app["image-path"].is_null()) {
        app_image_path = app["image-path"];
      }

      // Use validate_app_image_path to resolve and validate the path
      // This handles extension validation, PNG signature validation, and path resolution
      std::string validated_path = proc::validate_app_image_path(app_image_path);

      // Check if we got the default image path (means validation failed or no image configured)
      if (validated_path == DEFAULT_APP_IMAGE_PATH) {
        BOOST_LOG(debug) << "Application at index " << index << " does not have a valid cover image";
        not_found(response, "Cover image not found");
        return;
      }

      // Open and stream the validated file
      std::ifstream in(validated_path, std::ios::binary);
      if (!in) {
        BOOST_LOG(warning) << "Unable to read cover image file: " << validated_path;
        bad_request(response, "Unable to read cover image file");
        return;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "image/png");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetCover: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "igdb_<game_id>",
   *   "url": "https://images.igdb.com/igdb/image/upload/t_cover_big_2x/<slug>.png"
   * }
   * @endcode
   *
   * @api_examples{/api/covers/upload|:| POST|:| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);

      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");

      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);

      std::basic_string path = coverdir + http::url_escape(key) + ".png";
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path)) {
          bad_request(response, "Failed to download cover");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));

        std::ofstream imgfile(path);
        imgfile.write(data.data(), static_cast<int>(data.size()));
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs|:| GET|:| null}
   */
  void getLogs(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(config::sunshine.log_file.c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/plain");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password|:| POST|:| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!config::sunshine.username.empty() && !authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::vector<std::string> errors = {};
    std::stringstream ss;
    std::stringstream config_stream;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty()) {
        newUsername = username;
      }
      if (newUsername.empty()) {
        errors.emplace_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() || (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword) {
            errors.emplace_back("Password Mismatch");
          } else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            output_tree["status"] = true;
          }
        } else {
          errors.emplace_back("Invalid Current Credentials");
        }
      }

      if (!errors.empty()) {
        // join the errors array
        std::string error_message = std::accumulate(errors.begin(), errors.end(), std::string(), [](const std::string &a, const std::string &b) {
          return a.empty() ? b : a + ", " + b;
        });
        bad_request(response, error_message);
        return;
      }

      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief List client pairing requests that are waiting for PIN approval.
   *
   * @api_examples{/api/pin|:| GET|:| null}
   */
  void getPendingPairings(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["pairings"] = nlohmann::json::array();
    for (const auto &pairing : nvhttp::get_pending_pairings()) {
      output_tree["pairings"].push_back({
        {"id", pairing.id},
        {"name", pairing.name},
        {"address", pairing.address},
      });
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Cancel a client pairing request that is waiting for PIN approval.
   * The body for the delete request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pairing_id": "<pairing_id>"
   * }
   * @endcode
   *
   * @api_examples{/api/pin|:| DELETE|:| {"pairing_id":"0123456789abcdef0123456789abcdef"}}
   */
  void cancelPairing(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    const std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      const nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string pairing_id = input_tree.value("pairing_id", "");
      if (!nvhttp::is_valid_pairing_id(pairing_id)) {
        bad_request(response, "pairing_id must contain exactly 32 hexadecimal characters");
        return;
      }

      nlohmann::json output_tree;
      output_tree["status"] = nvhttp::cancel_pairing(pairing_id);
      send_response(response, output_tree);
    } catch (nlohmann::json::exception &e) {
      BOOST_LOG(warning) << "CancelPairing: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Submit a PIN and return whether the selected client completes pairing.
   *
   * The request remains open for up to the configured `ping_timeout` while
   * Moonlight completes the cryptographic handshake. A wrong PIN, protocol
   * failure, cancellation, or timeout returns `{"status":false}`.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pairing_id": "<pairing_id>",
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin|:| POST|:| {"pairing_id":"0123456789abcdef0123456789abcdef","pin":"1234","name":"My PC"}}
   */
  void savePin(const resp_https_t &response, const req_https_t &request) {
    if (!check_content_type(response, request, "application/json")) {
      return;
    }
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      const std::string name = input_tree.value("name", "");
      const std::string pairing_id = input_tree.value("pairing_id", "");
      const std::string pin = input_tree.value("pin", "");
      if (!nvhttp::is_valid_pairing_id(pairing_id)) {
        bad_request(response, "pairing_id must contain exactly 32 hexadecimal characters");
        return;
      }
      if (!nvhttp::is_valid_pairing_pin(pin)) {
        bad_request(response, "PIN must contain exactly 4 numeric digits");
        return;
      }
      if (!nvhttp::is_valid_pairing_name(name)) {
        bad_request(response, "Client name must contain between 1 and 128 bytes");
        return;
      }

      output_tree["status"] = nvhttp::pin(pairing_id, pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence|:| POST|:| null}
   */
  void resetDisplayDevicePersistence(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Authenticate a Web UI request and delete the saved XDG Portal restore token.
   * @details On platforms without XDG Portal capture, this operation succeeds without changing the filesystem.
   *
   * @param response HTTP response used for authentication, CSRF, and status output.
   * @param request HTTP request carrying the client identity and CSRF token.
   *
   * @api_examples{/api/reset-portal-token|:| POST|:| null}
   */
  void resetPortalToken(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    bool status = true;
#if defined(linux) || defined(__FreeBSD__)
    std::error_code ec;
    fs::remove(portal_token_path_provider()(), ec);
    if (ec) {
      BOOST_LOG(error) << "Failed to delete XDG Portal restore token: "sv << ec.message();
      status = false;
    }
#endif

    nlohmann::json output_tree;
    output_tree["status"] = status;
    send_response(response, output_tree);
  }

  /**
   * @brief Authenticate a Web UI request and restart the Sunshine process.
   *
   * @param response HTTP response used for authentication or CSRF failures.
   * @param request HTTP request carrying the client identity and CSRF token.
   *
   * @api_examples{/api/restart|:| POST|:| null}
   */
  void restart(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::string client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Return permission status for the current host platform.
   *
   * @api_examples{/api/permissions|:| GET|:| null}
   */
  void getPermissions(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
    output_tree["permissions"] = nlohmann::json::array();
#ifdef SUNSHINE_TESTS
    if (permission_status_override) {
      output_tree["permissions"] = *permission_status_override;
      send_response(response, output_tree);
      return;
    }
#endif
    for (const auto &permission : platf::get_permission_statuses()) {
      output_tree["permissions"].push_back({
        {"id", permission.id},
        {"status", permission.status},
        {"required", permission.required},
        {"verifiable", permission.verifiable},
        {"requestable", permission.requestable},
      });
    }
    send_response(response, output_tree);
  }

#ifdef SUNSHINE_TESTS
  void set_permission_statuses_for_testing(nlohmann::json permissions) {
    permission_status_override = std::move(permissions);
  }

  void reset_permission_statuses_for_testing() {
    permission_status_override.reset();
  }
#endif

  /**
   * @brief Start a native permission request or open its settings pane.
   *
   * @api_examples{/api/permissions/request|:| POST|:| {"id":"screen_recording"}}
   */
  void requestPermission(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }
    const auto client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }
    if (!check_content_type(response, request, "application/json")) {
      return;
    }

    try {
      const auto input = nlohmann::json::parse(request->content.string());
      if (!input.is_object() || !input.contains("id") || !input["id"].is_string()) {
        bad_request(response, "A permission ID is required");
        return;
      }

      const bool requested = platf::request_permission(input["id"].get<std::string>());
      if (!requested) {
        bad_request(response, "Unknown or unavailable permission");
        return;
      }
      send_response(response, {{"status", true}});
    } catch (const nlohmann::json::exception &) {
      bad_request(response, "Invalid permission request");
    }
  }

  /**
   * @brief Build Virtual HID Broker version and installation status.
   *
   * @return Virtual HID Broker status JSON.
   */
  nlohmann::json get_virtualhid_driver_status() {
#if defined(_WIN32) || defined(__APPLE__)
  #ifdef _WIN32
    const auto version_str = read_libvirtualhid_driver_version();
    const auto minimum_version = libvirtualhid_minimum_version;
  #else
    const auto version_str = read_libvirtualhid_broker_version();
    const auto minimum_version = libvirtualhid_macos_minimum_version;
  #endif
    const auto driver_detected = !version_str.empty();
    auto output_tree = build_driver_status(driver_detected, version_str, minimum_version);
    bool requires_installed_driver = true;
    std::string backend_name;
    std::string runtime_error_message;

    try {
      const auto runtime = platf::virtualhid::create_runtime();
      if (runtime) {
        const auto &capabilities = runtime->capabilities();
        backend_name = capabilities.backend_name;
        requires_installed_driver = capabilities.requires_installed_driver;
        output_tree = build_driver_status(driver_detected || capabilities.supports_gamepad, version_str, minimum_version);
      }
    } catch (const std::bad_alloc &exception) {
      runtime_error_message = exception.what();
    }

    output_tree["backend_name"] = backend_name;
    output_tree["requires_installed_driver"] = requires_installed_driver;
    if (!runtime_error_message.empty()) {
      output_tree["error"] = runtime_error_message;
    }
#else
    auto output_tree = build_driver_status(false, "", libvirtualhid_minimum_version);
    output_tree["error"] = "Virtual HID Broker status is only available on Windows and macOS";
    output_tree["backend_name"] = "";
    output_tree["requires_installed_driver"] = false;
#endif

    return output_tree;
  }

  /**
   * @brief Build ViGEmBus fallback driver version and installation status.
   *
   * @return ViGEmBus fallback driver status JSON.
   */
  nlohmann::json get_vigembus_driver_status() {
#ifdef _WIN32
    std::string version_str;

    // Check if ViGEmBus driver exists
    std::string system_root;
    if (!lizardbyte::common::get_env("SystemRoot", system_root)) {
      system_root = "C:\\Windows";
    }
    const std::filesystem::path driver_path = std::filesystem::path(system_root) / "System32" / "drivers" / "ViGEmBus.sys";
    const auto installed = std::filesystem::exists(driver_path);
    if (installed) {
      platf::getFileVersionInfo(driver_path, version_str);
    }

    auto output_tree = build_driver_status(installed, version_str, VIGEMBUS_MINIMUM_VERSION);
#else
    auto output_tree = build_driver_status(false, "", VIGEMBUS_MINIMUM_VERSION);
    output_tree["error"] = "ViGEmBus is only available on Windows";
#endif

    return output_tree;
  }

  /**
   * @brief Get virtual input driver version and installation status.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/virtual-input/status|:| GET|:| null}
   */
  void getVirtualInputStatus(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["virtualhid"] = get_virtualhid_driver_status();
    output_tree["vigembus"] = get_vigembus_driver_status();
    send_response(response, output_tree);
  }

  /**
   * @brief Get the current libvirtualhid machine license status.
   *
   * @param response HTTP response object.
   * @param request Authenticated HTTP request.
   *
   * @api_examples{/api/virtual-input/license|:| GET|:| null}
   */
  void getVirtualInputLicense(const resp_https_t &response, const req_https_t &request) {
    get_virtual_input_license(response, request);
  }

  /**
   * @brief Activate, validate, or deactivate the libvirtualhid machine license.
   *
   * Submitted license keys are used only for the synchronous broker call. They are
   * never logged or saved in Sunshine's configuration, and extracted mutable copies
   * are overwritten before the handler returns.
   *
   * @param response HTTP response object.
   * @param request Authenticated HTTP request with a JSON action.
   *
   * @api_examples{/api/virtual-input/license|:| POST|:| {"action":"validate"}}
   */
  void updateVirtualInputLicense(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    const auto client_id = get_client_id(request);
    if (!validate_csrf_token(response, request, client_id)) {
      return;
    }

    print_req(request);
    try {
      std::stringstream content;
      content << request->content.rdbuf();
      auto input_tree = nlohmann::json::parse(content);
      const auto action = input_tree.value("action", "");

      lvh::LicenseResult result;
      if (action == "activate") {
        auto license_key = input_tree.value("license_key", "");
        input_tree["license_key"] = "";
        if (license_key.empty()) {
          bad_request(response, "License key is required");
          return;
        }

        const scoped_sensitive_string_clear_t clear_license_key {license_key};
        result = lvh::activate_license(license_key);
      } else if (action == "validate") {
        result = lvh::validate_license();
      } else if (action == "deactivate") {
        result = lvh::deactivate_license();
      } else {
        bad_request(response, "Unknown license action");
        return;
      }

#ifdef _WIN32
      config::select_all_gamepad_drivers_if_licensed(result.license.licensed());
#endif
#if (defined(_WIN32) || defined(__APPLE__)) && defined(SUNSHINE_TRAY) && SUNSHINE_TRAY >= 1
      system_tray::update_tray_virtualhid_license(result.license, false);
#endif
#ifdef _WIN32
      if (result.status.ok()) {
        input::refresh_virtual_input();
      }
#endif
      send_response(response, build_virtualhid_license_status(result));
    } catch (const nlohmann::json::exception &) {
      bad_request(response, "Invalid license request");
    }
  }

  /**
   * @brief Checks whether a directory entry qualifies as an executable file.
   * @param entry The directory entry to check.
   * @param status The cached file status for the entry.
   * @return True if the file should be included in an executable-type listing.
   */
  bool is_browsable_executable([[maybe_unused]] const fs::directory_entry &entry, [[maybe_unused]] const fs::file_status &status) {
#ifdef _WIN32
    auto ext = entry.path().extension().string();
    boost::algorithm::to_lower(ext);
    return ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com" || ext == ".ps1";
#else
    const auto perms = status.permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none ||
           (perms & fs::perms::group_exec) != fs::perms::none ||
           (perms & fs::perms::others_exec) != fs::perms::none;
#endif
  }

#ifdef _WIN32
  /**
   * @brief Builds a JSON array of available Windows drive letters.
   * @return JSON array of drive-letter entries.
   */
  nlohmann::json get_windows_drives() {
    nlohmann::json entries = nlohmann::json::array();
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
      if (drives & (1 << i)) {
        const auto drive_letter = static_cast<char>('A' + i);
        const auto drive_path = std::string(1, drive_letter) + ":\\";
        nlohmann::json entry;
        entry["name"] = drive_path;
        entry["type"] = "directory";
        entry["path"] = drive_path;
        entries.push_back(entry);
      }
    }
    return entries;
  }
#endif

  /**
   * @brief Lists, filters, and sorts the entries of a directory for the browse API.
   * @param dir_path The directory to list.
   * @param type_str Filter type: "directory", "executable", "file", or "any".
   * @return Sorted JSON array of entry objects with name/type/path fields.
   */
  nlohmann::json build_browse_entries(const fs::path &dir_path, const std::string &type_str) {
    nlohmann::json entries = nlohmann::json::array();

    std::error_code iter_ec;
    for (auto it = fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, iter_ec);
         !iter_ec && it != fs::directory_iterator();
         it.increment(iter_ec)) {
      try {
        const auto status = it->status();
        const bool is_dir = fs::is_directory(status);

        if (const bool is_regular = fs::is_regular_file(status); !is_dir && !is_regular) {
          continue;
        }

        // Apply type filter (directories are always included for navigation)
        if (type_str == "directory" && !is_dir) {
          continue;
        }

        if (type_str == "executable" && !is_dir && !is_browsable_executable(*it, status)) {
          continue;
        }

        nlohmann::json file_entry;
        file_entry["name"] = it->path().filename().string();
        file_entry["path"] = it->path().string();
        file_entry["type"] = is_dir ? "directory" : "file";
        entries.push_back(file_entry);
      } catch (const fs::filesystem_error &e) {
        BOOST_LOG(debug) << "BrowseDirectory: skipping entry due to error: "sv << e.what();
      }
    }

    if (iter_ec) {
      BOOST_LOG(debug) << "BrowseDirectory: directory iteration error: "sv << iter_ec.message();
    }

    // Sort: directories first, then files; both case-insensitively alphabetical
    std::sort(entries.begin(), entries.end(), [](const nlohmann::json &a, const nlohmann::json &b) {
      const bool a_dir = (a["type"] == "directory");
      if (const bool b_dir = (b["type"] == "directory"); a_dir != b_dir) {
        return a_dir && !b_dir;
      }
      auto a_name = a["name"].get<std::string>();
      auto b_name = b["name"].get<std::string>();
      boost::algorithm::to_lower(a_name);
      boost::algorithm::to_lower(b_name);
      return a_name < b_name;
    });

    return entries;
  }

  /**
   * @brief Browse the server filesystem.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @note On Windows, an empty or root path returns the list of available drive letters.
   * @note On non-Windows, an empty path defaults to the filesystem root ("/").
   *
   * @api_examples{/api/browse?path=/home/user&type=directory|:| GET|:| null}
   */
  void browseDirectory(const resp_https_t &response, const req_https_t &request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      const auto query_params = request->parse_query_string();

      std::string path_str;
      if (const auto path_it = query_params.find("path"); path_it != query_params.end()) {
        path_str = path_it->second;
      }

      std::string type_str = "any";
      if (const auto type_it = query_params.find("type"); type_it != query_params.end() && !type_it->second.empty()) {
        type_str = type_it->second;
      }

      nlohmann::json output_tree;

#ifdef _WIN32
      // On Windows with an empty or root path, return the list of available drive letters
      if (path_str.empty() || path_str == "/" || path_str == "\\") {
        output_tree["path"] = "";
        output_tree["parent"] = "";
        output_tree["entries"] = get_windows_drives();
        send_response(response, output_tree);
        return;
      }
#else
      // On non-Windows, default an empty path to the filesystem root
      if (path_str.empty()) {
        path_str = "/";
      }
#endif

      // Normalize the path
      fs::path dir_path = fs::weakly_canonical(fs::path(path_str));

      // If the path points to a file, use its parent directory
      std::error_code ec;
      if (fs::is_regular_file(dir_path, ec)) {
        dir_path = dir_path.parent_path();
      }

      // If the path doesn't exist, try the parent
      if (!fs::exists(dir_path, ec)) {
        dir_path = dir_path.parent_path();
      }

      if (!fs::is_directory(dir_path, ec)) {
        bad_request(response, "Path is not a directory");
        return;
      }

      output_tree["path"] = dir_path.string();

      // Determine the parent path for the "Up" navigation
      const fs::path parent = dir_path.parent_path();
#ifdef _WIN32
      // At a drive root (e.g., C:\) the parent equals itself; signal the drive list with an empty string
      output_tree["parent"] = (parent == dir_path) ? "" : parent.string();
#else
      output_tree["parent"] = parent.string();
#endif

      output_tree["entries"] = build_browse_entries(dir_path, type_str);
      send_response(response, output_tree);
    } catch (const fs::filesystem_error &e) {
      BOOST_LOG(warning) << "BrowseDirectory: "sv << e.what();
      bad_request(response, e.what());
    }
  }

  /**
   * @brief Start the HTTPS configuration server.
   */
  void start() {
    platf::set_thread_name("confighttp");
    const auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    const auto port_https = net::map_port(PORT_HTTPS);
    const auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    https_server_t server {config::nvhttp.cert, config::nvhttp.pkey};

    // Helper to create SPA entry handlers without repeating the signature
    auto page_handler = [](bool require_auth = true, bool redirect_if_username = false) {
      return [require_auth, redirect_if_username](const resp_https_t &response, const req_https_t &request) {
        getPage(response, request, require_auth, redirect_if_username);
      };
    };

    // Unsupported methods use the common HTTP handler signature and do not inspect the request.
    const https_handler_t bad_request_handler = [](const resp_https_t &response, const req_https_t &) {
      bad_request(response);
    };
    // error by default
    server.default_resource["DELETE"] = bad_request_handler;
    server.default_resource["PATCH"] = bad_request_handler;
    server.default_resource["POST"] = bad_request_handler;
    server.default_resource["PUT"] = bad_request_handler;
    server.default_resource["GET"] = getFallbackPage;

    // Public SPA routes with authentication behavior that differs from the default fallback
    server.resource["^/logout/?$"]["GET"] = page_handler(false);
    server.resource["^/welcome/?$"]["GET"] = page_handler(false, true);

    // rest api
    server.resource["^/api/browse$"]["GET"] = browseDirectory;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/([0-9]+)$"]["DELETE"] = deleteApp;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/covers/([0-9]+)$"]["GET"] = getCover;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/api/csrf-token$"]["GET"] = getCSRFToken;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/pin$"]["DELETE"] = cancelPairing;
    server.resource["^/api/pin$"]["GET"] = getPendingPairings;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/reset-portal-token$"]["POST"] = resetPortalToken;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/permissions$"]["GET"] = getPermissions;
    server.resource["^/api/permissions/request$"]["POST"] = requestPermission;
    server.resource["^/api/virtual-input/license$"]["GET"] = getVirtualInputLicense;
    server.resource["^/api/virtual-input/license$"]["POST"] = updateVirtualInputLicense;
    server.resource["^/api/virtual-input/status$"]["GET"] = getVirtualInputStatus;

    // static/dynamic resources
    server.resource["^/images/sunshine.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-sunshine-45.png$"]["GET"] = getSunshineLogoImage;
    server.resource["^/assets\\/.+$"]["GET"] = getAsset;

    server.config.reuse_address = true;
    server.config.address = net::get_bind_address(address_family);
    server.config.port = port_https;

    const auto display_addr = net::get_bind_address_url_host();

    auto accept_and_run = [&](auto *https_server) {
      try {
        platf::set_thread_name("confighttp::tcp");
        https_server->start([&display_addr](const unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://"sv << display_addr << ":" << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::jthread tcp {accept_and_run, &server};

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
}  // namespace confighttp
