/**
 * @file src/platform/linux/publish.cpp
 * @brief Definitions for publishing services on Linux.
 * @note Adapted from https://www.avahi.org/doxygen/html/client-publish-service_8c-example.html
 */
#include <thread>

#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/server/network.h"
#include "src/server/nvhttp.h"
#include "src/utility.h"

using namespace std::literals;

namespace avahi {

  /**
   * @brief Error codes used by avahi.
   */
  enum err_e {
    OK = 0,  ///< OK
    ERR_FAILURE = -1,  ///< Generic error code
    ERR_BAD_STATE = -2,  ///< Object was in a bad state
    ERR_INVALID_HOST_NAME = -3,  ///< Invalid host name
    ERR_INVALID_DOMAIN_NAME = -4,  ///< Invalid domain name
    ERR_NO_NETWORK = -5,  ///< No suitable network protocol available
    ERR_INVALID_TTL = -6,  ///< Invalid DNS TTL
    ERR_IS_PATTERN = -7,  ///< RR key is pattern
    ERR_COLLISION = -8,  ///< Name collision
    ERR_INVALID_RECORD = -9,  ///< Invalid RR

    ERR_INVALID_SERVICE_NAME = -10,  ///< Invalid service name
    ERR_INVALID_SERVICE_TYPE = -11,  ///< Invalid service type
    ERR_INVALID_PORT = -12,  ///< Invalid port number
    ERR_INVALID_KEY = -13,  ///< Invalid key
    ERR_INVALID_ADDRESS = -14,  ///< Invalid address
    ERR_TIMEOUT = -15,  ///< Timeout reached
    ERR_TOO_MANY_CLIENTS = -16,  ///< Too many clients
    ERR_TOO_MANY_OBJECTS = -17,  ///< Too many objects
    ERR_TOO_MANY_ENTRIES = -18,  ///< Too many entries
    ERR_OS = -19,  ///< OS error

    ERR_ACCESS_DENIED = -20,  ///< Access denied
    ERR_INVALID_OPERATION = -21,  ///< Invalid operation
    ERR_DBUS_ERROR = -22,  ///< An unexpected D-Bus error occurred
    ERR_DISCONNECTED = -23,  ///< Daemon connection failed
    ERR_NO_MEMORY = -24,  ///< Memory exhausted
    ERR_INVALID_OBJECT = -25,  ///< The object passed to this function was invalid
    ERR_NO_DAEMON = -26,  ///< Daemon not running
    ERR_INVALID_INTERFACE = -27,  ///< Invalid interface
    ERR_INVALID_PROTOCOL = -28,  ///< Invalid protocol
    ERR_INVALID_FLAGS = -29,  ///< Invalid flags

    ERR_NOT_FOUND = -30,  ///< Not found
    ERR_INVALID_CONFIG = -31,  ///< Configuration error
    ERR_VERSION_MISMATCH = -32,  ///< Version mismatch
    ERR_INVALID_SERVICE_SUBTYPE = -33,  ///< Invalid service subtype
    ERR_INVALID_PACKET = -34,  ///< Invalid packet
    ERR_INVALID_DNS_ERROR = -35,  ///< Invalid DNS return code
    ERR_DNS_FORMERR = -36,  ///< DNS Error: Form error
    ERR_DNS_SERVFAIL = -37,  ///< DNS Error: Server Failure
    ERR_DNS_NXDOMAIN = -38,  ///< DNS Error: No such domain
    ERR_DNS_NOTIMP = -39,  ///< DNS Error: Not implemented

    ERR_DNS_REFUSED = -40,  ///< DNS Error: Operation refused
    ERR_DNS_YXDOMAIN = -41,  ///< TODO
    ERR_DNS_YXRRSET = -42,  ///< TODO
    ERR_DNS_NXRRSET = -43,  ///< TODO
    ERR_DNS_NOTAUTH = -44,  ///< DNS Error: Not authorized
    ERR_DNS_NOTZONE = -45,  ///< TODO
    ERR_INVALID_RDATA = -46,  ///< Invalid RDATA
    ERR_INVALID_DNS_CLASS = -47,  ///< Invalid DNS class
    ERR_INVALID_DNS_TYPE = -48,  ///< Invalid DNS type
    ERR_NOT_SUPPORTED = -49,  ///< Not supported

    ERR_NOT_PERMITTED = -50,  ///< Operation not permitted
    ERR_INVALID_ARGUMENT = -51,  ///< Invalid argument
    ERR_IS_EMPTY = -52,  ///< Is empty
    ERR_NO_CHANGE = -53,  ///< The requested operation is invalid because it is redundant

    ERR_MAX = -54  ///< TODO
  };

  constexpr auto IF_UNSPEC = -1;
  enum proto {
    PROTO_INET = 0,  ///< IPv4
    PROTO_INET6 = 1,  ///< IPv6
    PROTO_UNSPEC = -1  ///< Unspecified/all protocol(s)
  };

  enum ServerState {
    SERVER_INVALID,  ///< Invalid state (initial)
    SERVER_REGISTERING,  ///< Host RRs are being registered
    SERVER_RUNNING,  ///< All host RRs have been established
    SERVER_COLLISION,  ///< There is a collision with a host RR. All host RRs have been withdrawn, the user should set a new host name via avahi_server_set_host_name()
    SERVER_FAILURE  ///< Some fatal failure happened, the server is unable to proceed
  };

  enum ClientState {
    CLIENT_S_REGISTERING = SERVER_REGISTERING,  ///< Server state: REGISTERING
    CLIENT_S_RUNNING = SERVER_RUNNING,  ///< Server state: RUNNING
    CLIENT_S_COLLISION = SERVER_COLLISION,  ///< Server state: COLLISION
    CLIENT_FAILURE = 100,  ///< Some kind of error happened on the client side
    CLIENT_CONNECTING = 101  ///< We're still connecting. This state is only entered when AVAHI_CLIENT_NO_FAIL has been passed to avahi_client_new() and the daemon is not yet available.
  };

  enum EntryGroupState {
    ENTRY_GROUP_UNCOMMITED,  ///< The group has not yet been committed, the user must still call avahi_entry_group_commit()
    ENTRY_GROUP_REGISTERING,  ///< The entries of the group are currently being registered
    ENTRY_GROUP_ESTABLISHED,  ///< The entries have successfully been established
    ENTRY_GROUP_COLLISION,  ///< A name collision for one of the entries in the group has been detected, the entries have been withdrawn
    ENTRY_GROUP_FAILURE  ///< Some kind of failure happened, the entries have been withdrawn
  };

  enum ClientFlags {
    CLIENT_IGNORE_USER_CONFIG = 1,  ///< Don't read user configuration
    CLIENT_NO_FAIL = 2  ///< Don't fail if the daemon is not available when avahi_client_new() is called, instead enter CLIENT_CONNECTING state and wait for the daemon to appear
  };

  /**
   * @brief Flags for publishing functions.
   */
  enum PublishFlags {
    PUBLISH_UNIQUE = 1,  ///< For raw records: The RRset is intended to be unique
    PUBLISH_NO_PROBE = 2,  ///< For raw records: Though the RRset is intended to be unique no probes shall be sent
    PUBLISH_NO_ANNOUNCE = 4,  ///< For raw records: Do not announce this RR to other hosts
    PUBLISH_ALLOW_MULTIPLE = 8,  ///< For raw records: Allow multiple local records of this type, even if they are intended to be unique
    PUBLISH_NO_REVERSE = 16,  ///< For address records: don't create a reverse (PTR) entry
    PUBLISH_NO_COOKIE = 32,  ///< For service records: do not implicitly add the local service cookie to TXT data
    PUBLISH_UPDATE = 64,  ///< Update existing records instead of adding new ones
    PUBLISH_USE_WIDE_AREA = 128,  ///< Register the record using wide area DNS (i.e. unicast DNS update)
    PUBLISH_USE_MULTICAST = 256  ///< Register the record using multicast DNS
  };

  using IfIndex = int;
  using Protocol = int;

  struct EntryGroup;
  struct Poll;
  struct SimplePoll;
  struct Client;

  typedef void (*ClientCallback)(Client *, ClientState, void *userdata);
  typedef void (*EntryGroupCallback)(EntryGroup *g, EntryGroupState state, void *userdata);

  typedef void (*free_fn)(void *);

  typedef Client *(*client_new_fn)(const Poll *poll_api, ClientFlags flags, ClientCallback callback, void *userdata, int *error);
  typedef void (*client_free_fn)(Client *);
  typedef char *(*alternative_service_name_fn)(char *);

  typedef Client *(*entry_group_get_client_fn)(EntryGroup *);

  typedef EntryGroup *(*entry_group_new_fn)(Client *, EntryGroupCallback, void *userdata);
  typedef int (*entry_group_add_service_fn)(
    EntryGroup *group,
    IfIndex interface,
    Protocol protocol,
    PublishFlags flags,
    const char *name,
    const char *type,
    const char *domain,
    const char *host,
    uint16_t port,
    ...);

  typedef int (*entry_group_is_empty_fn)(EntryGroup *);
  typedef int (*entry_group_reset_fn)(EntryGroup *);
  typedef int (*entry_group_commit_fn)(EntryGroup *);

  typedef char *(*strdup_fn)(const char *);
  typedef char *(*strerror_fn)(int);
  typedef int (*client_errno_fn)(Client *);

  typedef Poll *(*simple_poll_get_fn)(SimplePoll *);
  typedef int (*simple_poll_loop_fn)(SimplePoll *);
  typedef void (*simple_poll_quit_fn)(SimplePoll *);
  typedef SimplePoll *(*simple_poll_new_fn)();
  typedef void (*simple_poll_free_fn)(SimplePoll *);

  free_fn free;
  client_new_fn client_new;
  client_free_fn client_free;
  alternative_service_name_fn alternative_service_name;
  entry_group_get_client_fn entry_group_get_client;
  entry_group_new_fn entry_group_new;
  entry_group_add_service_fn entry_group_add_service;
  entry_group_is_empty_fn entry_group_is_empty;
  entry_group_reset_fn entry_group_reset;
  entry_group_commit_fn entry_group_commit;
  strdup_fn strdup;
  strerror_fn strerror;
  client_errno_fn client_errno;
  simple_poll_get_fn simple_poll_get;
  simple_poll_loop_fn simple_poll_loop;
  simple_poll_quit_fn simple_poll_quit;
  simple_poll_new_fn simple_poll_new;
  simple_poll_free_fn simple_poll_free;

  int
  init_common() {
    static void *handle { nullptr };
    static bool funcs_loaded = false;

    if (funcs_loaded) return 0;

    if (!handle) {
      handle = dyn::handle({ "libavahi-common.so.3", "libavahi-common.so" });
      if (!handle) {
        return -1;
      }
    }

    std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
      { (dyn::apiproc *) &alternative_service_name, "avahi_alternative_service_name" },
      { (dyn::apiproc *) &free, "avahi_free" },
      { (dyn::apiproc *) &strdup, "avahi_strdup" },
      { (dyn::apiproc *) &strerror, "avahi_strerror" },
      { (dyn::apiproc *) &simple_poll_get, "avahi_simple_poll_get" },
      { (dyn::apiproc *) &simple_poll_loop, "avahi_simple_poll_loop" },
      { (dyn::apiproc *) &simple_poll_quit, "avahi_simple_poll_quit" },
      { (dyn::apiproc *) &simple_poll_new, "avahi_simple_poll_new" },
      { (dyn::apiproc *) &simple_poll_free, "avahi_simple_poll_free" },
    };

    if (dyn::load(handle, funcs)) {
      return -1;
    }

    funcs_loaded = true;
    return 0;
  }

  int
  init_client() {
    if (init_common()) {
      return -1;
    }

    static void *handle { nullptr };
    static bool funcs_loaded = false;

    if (funcs_loaded) return 0;

    if (!handle) {
      handle = dyn::handle({ "libavahi-client.so.3", "libavahi-client.so" });
      if (!handle) {
        return -1;
      }
    }

    std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
      { (dyn::apiproc *) &client_new, "avahi_client_new" },
      { (dyn::apiproc *) &client_free, "avahi_client_free" },
      { (dyn::apiproc *) &entry_group_get_client, "avahi_entry_group_get_client" },
      { (dyn::apiproc *) &entry_group_new, "avahi_entry_group_new" },
      { (dyn::apiproc *) &entry_group_add_service, "avahi_entry_group_add_service" },
      { (dyn::apiproc *) &entry_group_is_empty, "avahi_entry_group_is_empty" },
      { (dyn::apiproc *) &entry_group_reset, "avahi_entry_group_reset" },
      { (dyn::apiproc *) &entry_group_commit, "avahi_entry_group_commit" },
      { (dyn::apiproc *) &client_errno, "avahi_client_errno" },
    };

    if (dyn::load(handle, funcs)) {
      return -1;
    }

    funcs_loaded = true;
    return 0;
  }
}  // namespace avahi

namespace platf::publish {

  template <class T>
  void
  free(T *p) {
    avahi::free(p);
  }

  template <class T>
  using ptr_t = util::safe_ptr<T, free<T>>;
  using client_t = util::dyn_safe_ptr<avahi::Client, &avahi::client_free>;
  using poll_t = util::dyn_safe_ptr<avahi::SimplePoll, &avahi::simple_poll_free>;

  avahi::EntryGroup *group = nullptr;

  poll_t poll;
  client_t client;

  ptr_t<char> name;

  void
  create_services(avahi::Client *c);

  void
  entry_group_callback(avahi::EntryGroup *g, avahi::EntryGroupState state, void *) {
    group = g;

    switch (state) {
      case avahi::ENTRY_GROUP_ESTABLISHED:
        BOOST_LOG(info) << "Avahi service " << name.get() << " successfully established.";
        break;
      case avahi::ENTRY_GROUP_COLLISION:
        name.reset(avahi::alternative_service_name(name.get()));

        BOOST_LOG(info) << "Avahi service name collision, renaming service to " << name.get();

        create_services(avahi::entry_group_get_client(g));
        break;
      case avahi::ENTRY_GROUP_FAILURE:
        BOOST_LOG(error) << "Avahi entry group failure: " << avahi::strerror(avahi::client_errno(avahi::entry_group_get_client(g)));
        avahi::simple_poll_quit(poll.get());
        break;
      case avahi::ENTRY_GROUP_UNCOMMITED:
      case avahi::ENTRY_GROUP_REGISTERING:;
    }
  }

  void
  create_services(avahi::Client *c) {
    int ret;

    auto fg = util::fail_guard([]() {
      avahi::simple_poll_quit(poll.get());
    });

    if (!group) {
      if (!(group = avahi::entry_group_new(c, entry_group_callback, nullptr))) {
        BOOST_LOG(error) << "avahi::entry_group_new() failed: "sv << avahi::strerror(avahi::client_errno(c));
        return;
      }
    }

    if (avahi::entry_group_is_empty(group)) {
      BOOST_LOG(info) << "Adding avahi service "sv << name.get();

      ret = avahi::entry_group_add_service(
        group,
        avahi::IF_UNSPEC, avahi::PROTO_UNSPEC,
        avahi::PublishFlags(0),
        name.get(),
        SERVICE_TYPE,
        nullptr, nullptr,
        net::map_port(nvhttp::PORT_HTTP),
        nullptr);

      if (ret < 0) {
        if (ret == avahi::ERR_COLLISION) {
          // A service name collision with a local service happened. Let's pick a new name
          name.reset(avahi::alternative_service_name(name.get()));
          BOOST_LOG(info) << "Service name collision, renaming service to "sv << name.get();

          avahi::entry_group_reset(group);

          create_services(c);

          fg.disable();
          return;
        }

        BOOST_LOG(error) << "Failed to add "sv << SERVICE_TYPE << " service: "sv << avahi::strerror(ret);
        return;
      }

      ret = avahi::entry_group_commit(group);
      if (ret < 0) {
        BOOST_LOG(error) << "Failed to commit entry group: "sv << avahi::strerror(ret);
        return;
      }
    }

    fg.disable();
  }

  void
  client_callback(avahi::Client *c, avahi::ClientState state, void *) {
    switch (state) {
      case avahi::CLIENT_S_RUNNING:
        create_services(c);
        break;
      case avahi::CLIENT_FAILURE:
        BOOST_LOG(error) << "Client failure: "sv << avahi::strerror(avahi::client_errno(c));
        avahi::simple_poll_quit(poll.get());
        break;
      case avahi::CLIENT_S_COLLISION:
      case avahi::CLIENT_S_REGISTERING:
        if (group)
          avahi::entry_group_reset(group);
        break;
      case avahi::CLIENT_CONNECTING:;
    }
  }

  class deinit_t: public ::platf::deinit_t {
  public:
    std::thread poll_thread;

    deinit_t(std::thread poll_thread):
        poll_thread { std::move(poll_thread) } {}

    ~deinit_t() override {
      if (avahi::simple_poll_quit && poll) {
        avahi::simple_poll_quit(poll.get());
      }

      if (poll_thread.joinable()) {
        poll_thread.join();
      }
    }
  };

  [[nodiscard]] std::unique_ptr<::platf::deinit_t>
  start() {
    if (avahi::init_client()) {
      return nullptr;
    }

    int avhi_error;

    poll.reset(avahi::simple_poll_new());
    if (!poll) {
      BOOST_LOG(error) << "Failed to create simple poll object."sv;
      return nullptr;
    }

    name.reset(avahi::strdup(SERVICE_NAME));

    client.reset(
      avahi::client_new(avahi::simple_poll_get(poll.get()), avahi::ClientFlags(0), client_callback, nullptr, &avhi_error));

    if (!client) {
      BOOST_LOG(error) << "Failed to create client: "sv << avahi::strerror(avhi_error);
      return nullptr;
    }

    return std::make_unique<deinit_t>(std::thread { avahi::simple_poll_loop, poll.get() });
  }
}  // namespace platf::publish
