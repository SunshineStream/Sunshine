/**
 * @file src/platform/macos/publish.cpp
 * @brief Definitions for publishing services on macOS.
 */
#include <dns_sd.h>
#include <thread>

#include "src/logging.h"
#include "src/platform/common.h"
#include "src/server/network.h"
#include "src/server/nvhttp.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::publish {
  namespace {
    /** @brief Custom deleter intended to be used for `std::unique_ptr<DNSServiceRef>`. */
    struct ServiceRefDeleter {
      typedef DNSServiceRef pointer;  ///< Type of object to be deleted.
      void
      operator()(pointer serviceRef) {
        DNSServiceRefDeallocate(serviceRef);
        BOOST_LOG(info) << "Deregistered DNS service."sv;
      }
    };
    /** @brief This class encapsulates the polling and deinitialization of our connection with
     *         the mDNS service. Implements the `::platf::deinit_t` interface.
     */
    class deinit_t: public ::platf::deinit_t, std::unique_ptr<DNSServiceRef, ServiceRefDeleter> {
    public:
      /** @brief Construct deinit_t object.
       *
       * Create a thread that will use `select(2)` to wait for a response from the mDNS service.
       * The thread will give up if an error is received or if `_stopRequested` becomes true.
       *
       * @param serviceRef An initialized reference to the mDNS service.
       */
      deinit_t(DNSServiceRef serviceRef):
          unique_ptr(serviceRef) {
        _thread = std::thread { [serviceRef, &_stopRequested = std::as_const(_stopRequested)]() {
          const auto socket = DNSServiceRefSockFD(serviceRef);
          while (!_stopRequested) {
            auto fdset = fd_set {};
            FD_ZERO(&fdset);
            FD_SET(socket, &fdset);
            auto timeout = timeval { .tv_sec = 3, .tv_usec = 0 };  // 3 second timeout
            const auto ready = select(socket + 1, &fdset, nullptr, nullptr, &timeout);
            if (ready == -1) {
              BOOST_LOG(error) << "Failed to obtain response from DNS service."sv;
              break;
            }
            else if (ready != 0) {
              DNSServiceProcessResult(serviceRef);
              break;
            }
          }
        } };
      }
      /** @brief Ensure that we gracefully finish polling the mDNS service before freeing our
       *         connection to it.
       */
      ~deinit_t() override {
        _stopRequested = true;
        _thread.join();
      }
      deinit_t(const deinit_t &) = delete;
      deinit_t &
      operator=(const deinit_t &) = delete;

    private:
      std::thread _thread;  ///< Thread for polling the mDNS service for a response.
      std::atomic<bool> _stopRequested = false;  ///< Whether to stop polling the mDNS service.
    };

    /** @brief Callback that will be invoked when the mDNS service finishes registering our service.
     *  @param errorCode Describes whether the registration was successful.
     */
    void
    registrationCallback(DNSServiceRef /*serviceRef*/, DNSServiceFlags /*flags*/,
      DNSServiceErrorType errorCode, const char * /*name*/,
      const char * /*regtype*/, const char * /*domain*/, void * /*context*/) {
      if (errorCode != kDNSServiceErr_NoError) {
        BOOST_LOG(error) << "Failed to register DNS service: Error "sv << errorCode;
        return;
      }
      BOOST_LOG(info) << "Successfully registered DNS service."sv;
    }
  }  // anonymous namespace

  /**
   * @brief Main entry point for publication of our service on macOS.
   *
   * This function initiates a connection to the macOS mDNS service and requests to register
   * our Sunshine service. Registration will occur asynchronously (unless it fails immediately,
   * which is probably only possible if the host machine is misconfigured).
   *
   * @return Either `nullptr` (if the registration fails immediately) or a `uniqur_ptr<deinit_t>`,
   *         which will manage polling for a response from the mDNS service, and then, when
   *         deconstructed, will deregister the service.
   */
  [[nodiscard]] std::unique_ptr<::platf::deinit_t>
  start() {
    auto serviceRef = DNSServiceRef {};
    const auto status = DNSServiceRegister(
      &serviceRef,
      0,  // flags
      0,  // interfaceIndex
      SERVICE_NAME, SERVICE_TYPE,
      nullptr,  // domain
      nullptr,  // host
      htons(net::map_port(nvhttp::PORT_HTTP)),
      0,  // txtLen
      nullptr,  // txtRecord
      registrationCallback,
      nullptr  // context
    );
    if (status != kDNSServiceErr_NoError) {
      BOOST_LOG(error) << "Failed immediately to register DNS service: Error "sv << status;
      return nullptr;
    }
    return std::make_unique<deinit_t>(serviceRef);
  }
}  // namespace platf::publish
