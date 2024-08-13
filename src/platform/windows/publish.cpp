/**
 * @file src/platform/windows/publish.cpp
 * @brief Definitions for Windows mDNS service registration.
 */
#include <winsock2.h>

#include <windows.h>

#include <windns.h>
#include <winerror.h>

#include <boost/asio/ip/host_name.hpp>

#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/server/network.h"
#include "src/server/nvhttp.h"
#include "src/thread_safe.h"

#define _FN(x, ret, args)    \
  typedef ret(*x##_fn) args; \
  static x##_fn x

using namespace std::literals;

#define __SV(quote) L##quote##sv
#define SV(quote) __SV(quote)

extern "C" {
#ifndef __MINGW32__
constexpr auto DNS_REQUEST_PENDING = 9506L;
constexpr auto DNS_QUERY_REQUEST_VERSION1 = 0x1;
constexpr auto DNS_QUERY_RESULTS_VERSION1 = 0x1;
#endif

#define SERVICE_DOMAIN "local"

constexpr auto SERVICE_INSTANCE_NAME = SV(SERVICE_NAME "." SERVICE_TYPE "." SERVICE_DOMAIN);
constexpr auto SERVICE_TYPE_DOMAIN = SV(SERVICE_TYPE "." SERVICE_DOMAIN);

#ifndef __MINGW32__
typedef struct _DNS_SERVICE_INSTANCE {
  LPWSTR pszInstanceName;
  LPWSTR pszHostName;

  IP4_ADDRESS *ip4Address;
  IP6_ADDRESS *ip6Address;

  WORD wPort;
  WORD wPriority;
  WORD wWeight;

  // Property list
  DWORD dwPropertyCount;

  PWSTR *keys;
  PWSTR *values;

  DWORD dwInterfaceIndex;
} DNS_SERVICE_INSTANCE, *PDNS_SERVICE_INSTANCE;
#endif

typedef VOID WINAPI
DNS_SERVICE_REGISTER_COMPLETE(
  _In_ DWORD Status,
  _In_ PVOID pQueryContext,
  _In_ PDNS_SERVICE_INSTANCE pInstance);

typedef DNS_SERVICE_REGISTER_COMPLETE *PDNS_SERVICE_REGISTER_COMPLETE;

#ifndef __MINGW32__
typedef struct _DNS_SERVICE_CANCEL {
  PVOID reserved;
} DNS_SERVICE_CANCEL, *PDNS_SERVICE_CANCEL;

typedef struct _DNS_SERVICE_REGISTER_REQUEST {
  ULONG Version;
  ULONG InterfaceIndex;
  PDNS_SERVICE_INSTANCE pServiceInstance;
  PDNS_SERVICE_REGISTER_COMPLETE pRegisterCompletionCallback;
  PVOID pQueryContext;
  HANDLE hCredentials;
  BOOL unicastEnabled;
} DNS_SERVICE_REGISTER_REQUEST, *PDNS_SERVICE_REGISTER_REQUEST;
#endif

_FN(_DnsServiceFreeInstance, VOID, (_In_ PDNS_SERVICE_INSTANCE pInstance));
_FN(_DnsServiceDeRegister, DWORD, (_In_ PDNS_SERVICE_REGISTER_REQUEST pRequest, _Inout_opt_ PDNS_SERVICE_CANCEL pCancel));
_FN(_DnsServiceRegister, DWORD, (_In_ PDNS_SERVICE_REGISTER_REQUEST pRequest, _Inout_opt_ PDNS_SERVICE_CANCEL pCancel));
} /* extern "C" */

namespace platf::publish {
  VOID WINAPI
  register_cb(DWORD status, PVOID pQueryContext, PDNS_SERVICE_INSTANCE pInstance) {
    auto alarm = (safe::alarm_t<PDNS_SERVICE_INSTANCE>::element_type *) pQueryContext;

    if (status) {
      print_status("register_cb()"sv, status);
    }

    alarm->ring(pInstance);
  }

  static int
  service(bool enable, PDNS_SERVICE_INSTANCE &existing_instance) {
    auto alarm = safe::make_alarm<PDNS_SERVICE_INSTANCE>();

    std::wstring name { SERVICE_INSTANCE_NAME.data(), SERVICE_INSTANCE_NAME.size() };
    std::wstring domain { SERVICE_TYPE_DOMAIN.data(), SERVICE_TYPE_DOMAIN.size() };

    auto host = from_utf8(boost::asio::ip::host_name() + ".local");

    DNS_SERVICE_INSTANCE instance {};
    instance.pszInstanceName = name.data();
    instance.wPort = net::map_port(nvhttp::PORT_HTTP);
    instance.pszHostName = host.data();

    // Setting these values ensures Windows mDNS answers comply with RFC 1035.
    // If these are unset, Windows will send a TXT record that has zero strings,
    // which is illegal. Setting them to a single empty value causes Windows to
    // send a single empty string for the TXT record, which is the correct thing
    // to do when advertising a service without any TXT strings.
    //
    // Most clients aren't strictly checking TXT record compliance with RFC 1035,
    // but Apple's mDNS resolver does and rejects the entire answer if an invalid
    // TXT record is present.
    PWCHAR keys[] = { nullptr };
    PWCHAR values[] = { nullptr };
    instance.dwPropertyCount = 1;
    instance.keys = keys;
    instance.values = values;

    DNS_SERVICE_REGISTER_REQUEST req {};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.pQueryContext = alarm.get();
    req.pServiceInstance = enable ? &instance : existing_instance;
    req.pRegisterCompletionCallback = register_cb;

    DNS_STATUS status {};

    if (enable) {
      status = _DnsServiceRegister(&req, nullptr);
      if (status != DNS_REQUEST_PENDING) {
        print_status("DnsServiceRegister()"sv, status);
        return -1;
      }
    }
    else {
      status = _DnsServiceDeRegister(&req, nullptr);
      if (status != DNS_REQUEST_PENDING) {
        print_status("DnsServiceDeRegister()"sv, status);
        return -1;
      }
    }

    alarm->wait();

    auto registered_instance = alarm->status();
    if (enable) {
      // Store this instance for later deregistration
      existing_instance = registered_instance;
    }
    else if (registered_instance) {
      // Deregistration was successful
      _DnsServiceFreeInstance(registered_instance);
      existing_instance = nullptr;
    }

    return registered_instance ? 0 : -1;
  }

  class mdns_registration_t: public ::platf::deinit_t {
  public:
    mdns_registration_t():
        existing_instance(nullptr) {
      if (service(true, existing_instance)) {
        BOOST_LOG(error) << "Unable to register Sunshine mDNS service"sv;
        return;
      }

      BOOST_LOG(info) << "Registered Sunshine mDNS service"sv;
    }

    ~mdns_registration_t() override {
      if (existing_instance) {
        if (service(false, existing_instance)) {
          BOOST_LOG(error) << "Unable to unregister Sunshine mDNS service"sv;
          return;
        }

        BOOST_LOG(info) << "Unregistered Sunshine mDNS service"sv;
      }
    }

  private:
    PDNS_SERVICE_INSTANCE existing_instance;
  };

  int
  load_funcs(HMODULE handle) {
    auto fg = util::fail_guard([handle]() {
      FreeLibrary(handle);
    });

    _DnsServiceFreeInstance = (_DnsServiceFreeInstance_fn) GetProcAddress(handle, "DnsServiceFreeInstance");
    _DnsServiceDeRegister = (_DnsServiceDeRegister_fn) GetProcAddress(handle, "DnsServiceDeRegister");
    _DnsServiceRegister = (_DnsServiceRegister_fn) GetProcAddress(handle, "DnsServiceRegister");

    if (!(_DnsServiceFreeInstance && _DnsServiceDeRegister && _DnsServiceRegister)) {
      BOOST_LOG(error) << "mDNS service not available in dnsapi.dll"sv;
      return -1;
    }

    fg.disable();
    return 0;
  }

  std::unique_ptr<::platf::deinit_t>
  start() {
    HMODULE handle = LoadLibrary("dnsapi.dll");

    if (!handle || load_funcs(handle)) {
      BOOST_LOG(error) << "Couldn't load dnsapi.dll, You'll need to add PC manually from Moonlight"sv;
      return nullptr;
    }

    return std::make_unique<mdns_registration_t>();
  }
}  // namespace platf::publish
