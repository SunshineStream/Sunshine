/**
 * @file src/server/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <utility>

// lib includes
#include <Simple-Web-Server/server_http.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <string>

// local includes
#include "src/config.h"
#include "src/file_handler.h"
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/server/crypto.h"
#include "src/server/httpcommon.h"
#include "src/server/network.h"
#include "src/server/nvhttp.h"
#include "src/server/rtsp.h"
#include "src/system_tray.h"
#include "src/utility.h"
#include "src/uuid.h"
#include "src/video.h"

using namespace std::literals;
namespace nvhttp {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  crypto::cert_chain_t cert_chain;

  class SunshineHttpsServer: public SimpleWeb::Server<SimpleWeb::HTTPS> {
  public:
    SunshineHttpsServer(const std::string &certification_file, const std::string &private_key_file):
        SimpleWeb::Server<SimpleWeb::HTTPS>::Server(certification_file, private_key_file) {}

    std::function<int(SSL *)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    void
    after_bind() override {
      SimpleWeb::Server<SimpleWeb::HTTPS>::after_bind();

      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void
    accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock)
          return;

        if (ec != SimpleWeb::error::operation_aborted)
          this->accept();

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock)
              return;
            if (!ec) {
              if (verify && !verify(session->connection->socket->native_handle()))
                this->write(session, on_verify_failed);
              else
                this->read(session);
            }
            else if (this->on_error)
              this->on_error(session->request, ec);
          });
        }
        else if (this->on_error)
          this->on_error(session->request, ec);
      });
    }
  };

  using https_server_t = SunshineHttpsServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  struct named_cert_t {
    std::string name;
    std::string uuid;
    std::string cert;
  };

  struct client_t {
    std::vector<std::string> certs;
    std::vector<named_cert_t> named_devices;
  };

  struct pair_session_t {
    struct {
      std::string uniqueID;
      std::string cert;
    } client;

    std::unique_ptr<crypto::aes_t> cipher_key;
    std::vector<uint8_t> clienthash;

    std::string serversecret;
    std::string serverchallenge;

    struct {
      util::Either<
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>,
        std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>>
        response;
      std::string salt;
    } async_insert_pin;
  };

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string
  get_arg(const args_t &args, const char *name, const char *default_value = nullptr) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != NULL) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  void
  save_state() {
    pt::ptree root;

    if (fs::exists(config::nvhttp.file_state)) {
      try {
        pt::read_json(config::nvhttp.file_state, root);
      }
      catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
        return;
      }
    }

    root.erase("root"s);

    root.put("root.uniqueid", http::unique_id);
    client_t &client = client_root;
    pt::ptree node;

    pt::ptree named_cert_nodes;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("cert"s, named_cert.cert);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }
    root.add_child("root.named_devices"s, named_cert_nodes);

    try {
      pt::write_json(config::nvhttp.file_state, root);
    }
    catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void
  load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    pt::ptree tree;
    try {
      pt::read_json(config::nvhttp.file_state, tree);
    }
    catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();

      return;
    }

    auto unique_id_p = tree.get_optional<std::string>("root.uniqueid");
    if (!unique_id_p) {
      // This file doesn't contain moonlight credentials
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }
    http::unique_id = std::move(*unique_id_p);

    auto root = tree.get_child("root");
    client_t client;

    // Import from old format
    if (root.get_child_optional("devices")) {
      auto device_nodes = root.get_child("devices");
      for (auto &[_, device_node] : device_nodes) {
        auto uniqID = device_node.get<std::string>("uniqueid");

        if (device_node.count("certs")) {
          for (auto &[_, el] : device_node.get_child("certs")) {
            named_cert_t named_cert;
            named_cert.name = ""s;
            named_cert.cert = el.get_value<std::string>();
            named_cert.uuid = uuid_util::uuid_t::generate().string();
            client.named_devices.emplace_back(named_cert);
            client.certs.emplace_back(named_cert.cert);
          }
        }
      }
    }

    if (root.count("named_devices")) {
      for (auto &[_, el] : root.get_child("named_devices")) {
        named_cert_t named_cert;
        named_cert.name = el.get_child("name").get_value<std::string>();
        named_cert.cert = el.get_child("cert").get_value<std::string>();
        named_cert.uuid = el.get_child("uuid").get_value<std::string>();
        client.named_devices.emplace_back(named_cert);
        client.certs.emplace_back(named_cert.cert);
      }
    }

    // Empty certificate chain and import certs from file
    cert_chain.clear();
    for (auto &cert : client.certs) {
      cert_chain.add(crypto::x509(cert));
    }
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(crypto::x509(named_cert.cert));
    }

    client_root = client;
  }

  void
  update_id_client(const std::string &uniqueID, std::string &&cert, op_e op) {
    switch (op) {
      case op_e::ADD: {
        client_t &client = client_root;
        client.certs.emplace_back(std::move(cert));
      } break;
      case op_e::REMOVE:
        client_t client;
        client_root = client;
        break;
    }

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t>
  make_launch_session(bool host_audio, const args_t &args) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
    std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

    launch_session->host_audio = host_audio;
    std::stringstream mode = std::stringstream(get_arg(args, "mode", "0x0x0"));
    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) launch_session->width = atoi(segment.c_str());
      if (x == 1) launch_session->height = atoi(segment.c_str());
      if (x == 2) launch_session->fps = atoi(segment.c_str());
      x++;
    }
    launch_session->unique_id = (get_arg(args, "uniqueid", "unknown"));
    launch_session->appid = util::from_view(get_arg(args, "appid", "unknown"));
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));

    // Encrypted RTSP is enabled with client reported corever >= 1
    auto corever = util::from_view(get_arg(args, "corever", "0"));
    if (corever >= 1) {
      launch_session->rtsp_cipher = crypto::cipher::gcm_t {
        launch_session->gcm_key, false
      };
      launch_session->rtsp_iv_counter = 0;
    }
    launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

    // Generate the unique identifiers for this connection that we will send later during RTSP handshake
    unsigned char raw_payload[8];
    RAND_bytes(raw_payload, sizeof(raw_payload));
    launch_session->av_ping_payload = util::hex_vec(raw_payload);
    RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

    launch_session->iv.resize(16);
    uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
    auto prepend_iv_p = (uint8_t *) &prepend_iv;
    std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    return launch_session;
  }

  void
  getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.async_insert_pin.salt.size() < 32) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Salt too short");
      return;
    }

    std::string_view salt_view { sess.async_insert_pin.salt.data(), 32 };

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void
  serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const args_t &args) {
    auto encrypted_response = util::from_hex_vec(get_arg(args, "serverchallengeresp"), true);

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void
  clientchallenge(pair_session_t &sess, pt::ptree &tree, const args_t &args) {
    auto challenge = util::from_hex_vec(get_arg(args, "clientchallenge"), true);

    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({ (char *) decrypted.data(), decrypted.size() });
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void
  clientpairingsecret(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, pair_session_t &sess, pt::ptree &tree, const args_t &args) {
    auto &client = sess.client;

    auto pairingsecret = util::from_hex_vec(get_arg(args, "clientpairingsecret"), true);
    if (pairingsecret.size() <= 16) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Clientpairingsecret too short");
      return;
    }

    std::string_view secret { pairingsecret.data(), 16 };
    std::string_view sign { pairingsecret.data() + secret.size(), pairingsecret.size() - secret.size() };

    auto x509 = crypto::x509(client.cert);
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    if (!std::memcmp(hash.data(), sess.clienthash.data(), hash.size()) && crypto::verify256(crypto::x509(client.cert), secret, sign)) {
      tree.put("root.paired", 1);
      add_cert->raise(crypto::x509(client.cert));

      auto it = map_id_sess.find(client.uniqueID);

      update_id_client(client.uniqueID, std::move(client.cert), op_e::ADD);
      map_id_sess.erase(it);
    }
    else {
      map_id_sess.erase(client.uniqueID);
      tree.put("root.paired", 0);
    }

    tree.put("root.<xmlattr>.status_code", 200);
  }

  template <class T>
  struct tunnel;

  template <>
  struct tunnel<SimpleWeb::HTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template <>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  template <class T>
  void
  print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template <class T>
  void
  not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());

    *response
      << "HTTP/1.1 404 NOT FOUND\r\n"
      << data.str();

    response->close_connection_after_response = true;
  }

  template <class T>
  void
  pair(std::shared_ptr<safe::queue_t<crypto::x509_t>> &add_cert, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID { get_arg(args, "uniqueid") };
    auto sess_it = map_id_sess.find(uniqID);

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        sess.client.uniqueID = std::move(uniqID);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

        BOOST_LOG(debug) << sess.client.cert;
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));
        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
        }
        else {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      }
      else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
      }
    }
    else if (it = args.find("clientchallenge"); it != std::end(args)) {
      clientchallenge(sess_it->second, tree, args);
    }
    else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      serverchallengeresp(sess_it->second, tree, args);
    }
    else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      clientpairingsecret(add_cert, sess_it->second, tree, args);
    }
    else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool
  pin(std::string pin, std::string name) {
    pt::ptree tree;
    if (map_id_sess.empty()) {
      return false;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message", "Pin must be 4 digits, " + std::to_string(pin.size()) + " provided");
      return false;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    auto &sess = std::begin(map_id_sess)->second;
    getservercert(sess, tree, pin);

    // set up named cert
    client_t &client = client_root;
    named_cert_t named_cert;
    named_cert.name = name;
    named_cert.cert = sess.client.cert;
    named_cert.uuid = uuid_util::uuid_t::generate().string();
    client.named_devices.emplace_back(named_cert);

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &async_response = sess.async_insert_pin.response;
    if (async_response.has_left() && async_response.left()) {
      async_response.left()->write(data.str());
    }
    else if (async_response.has_right() && async_response.right()) {
      async_response.right()->write(data.str());
    }
    else {
      return false;
    }

    // reset async_response
    async_response = std::decay_t<decltype(async_response.left())>();
    // response to the current request
    return true;
  }

  template <class T>
  void
  serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<SimpleWeb::HTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", video::active_hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<SimpleWeb::HTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
    }
    else {
      tree.put("root.mac", "00:00:00:00:00:00");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    }
    else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (video::active_hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
    }
    if (video::active_hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
    }
    if (video::active_av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
    }
    if (video::active_av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);

    auto current_appid = proc::proc.running();
    tree.put("root.PairStatus", pair_status);
    tree.put("root.currentgame", current_appid);
    tree.put("root.state", current_appid > 0 ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  pt::ptree
  get_all_clients() {
    pt::ptree named_cert_nodes;
    client_t &client = client_root;
    for (auto &named_cert : client.named_devices) {
      pt::ptree named_cert_node;
      named_cert_node.put("name"s, named_cert.name);
      named_cert_node.put("uuid"s, named_cert.uuid);
      named_cert_nodes.push_back(std::make_pair(""s, named_cert_node));
    }

    return named_cert_nodes;
  }

  void
  applist(resp_https_t response, req_https_t request) {
    print_req<SimpleWeb::HTTPS>(request);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    for (auto &proc : proc::proc.get_apps()) {
      pt::ptree app;

      app.put("IsHdrSupported"s, video::active_hevc_mode == 3 ? 1 : 0);
      app.put("AppTitle"s, proc.name);
      app.put("ID", proc.id);

      apps.push_back(std::make_pair("App", std::move(app)));
    }
  }

  void
  launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SimpleWeb::HTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    if (rtsp_stream::session_count() == config::stream.channels) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "The host's concurrent stream limit has been reached. Stop an existing stream or increase the 'Channels' value in the Sunshine Web UI.");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      args.find("appid"s) == std::end(args)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    auto appid = util::from_view(get_arg(args, "appid"));

    auto current_appid = proc::proc.running();
    if (current_appid > 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

      return;
    }

    // Probe encoders again before streaming to ensure our chosen
    // encoder matches the active GPU (which could have changed
    // due to hotplugging, driver crash, primary monitor change,
    // or any number of other factors).
    if (rtsp_stream::session_count() == 0) {
      if (video::probe_encoders()) {
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, args);

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    if (appid > 0) {
      auto err = proc::proc.execute(appid, launch_session);
      if (err) {
        tree.put("root.<xmlattr>.status_code", err);
        tree.put("root.<xmlattr>.status_message", "Failed to start the specified application");
        tree.put("root.gamesession", 0);

        return;
      }
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.sessionUrl0", launch_session->rtsp_url_scheme +
                                   net::addr_to_url_escaped_string(request->local_endpoint().address()) + ':' +
                                   std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT)));
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);
  }

  void
  resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<SimpleWeb::HTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    // It is possible that due a race condition that this if-statement gives a false negative,
    // that is automatically resolved in rtsp_server_t
    if (rtsp_stream::session_count() == config::stream.channels) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "The host's concurrent stream limit has been reached. Stop an existing stream or increase the 'Channels' value in the Sunshine Web UI.");

      return;
    }

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    if (rtsp_stream::session_count() == 0) {
      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }

      // Newer Moonlight clients send localAudioPlayMode on /resume too,
      // so we should use it if it's present in the args and there are
      // no active sessions we could be interfering with.
      if (args.find("localAudioPlayMode"s) != std::end(args)) {
        host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
      }
    }

    auto launch_session = make_launch_session(host_audio, args);

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.sessionUrl0", launch_session->rtsp_url_scheme +
                                   net::addr_to_url_escaped_string(request->local_endpoint().address()) + ':' +
                                   std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT)));
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);
  }

  void
  cancel(resp_https_t response, req_https_t request) {
    print_req<SimpleWeb::HTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    // It is possible that due a race condition that this if-statement gives a false positive,
    // the client should try again
    if (rtsp_stream::session_count() != 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "All sessions must be disconnected before quitting");

      return;
    }

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    }
  }

  void
  appasset(resp_https_t response, req_https_t request) {
    print_req<SimpleWeb::HTTPS>(request);

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void
  start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    conf_intern.pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    conf_intern.servercert = file_handler::read_file(config::nvhttp.cert.c_str());

    auto add_cert = std::make_shared<safe::queue_t<crypto::x509_t>>(30);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server { config::nvhttp.cert, config::nvhttp.pkey };
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [add_cert](SSL *ssl) {
      crypto::x509_t x509 { SSL_get_peer_certificate(ssl) };
      if (!x509) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return 0;
      }

      int verified = 0;

      auto fg = util::fail_guard([&]() {
        char subject_name[256];

        X509_NAME_oneline(X509_get_subject_name(x509.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << subject_name << " -- "sv << (verified ? "verified"sv : "denied"sv);
      });

      while (add_cert->peek()) {
        char subject_name[256];

        auto cert = add_cert->pop();
        X509_NAME_oneline(X509_get_subject_name(cert.get()), subject_name, sizeof(subject_name));

        BOOST_LOG(debug) << "Added cert ["sv << subject_name << ']';
        cert_chain.add(std::move(cert));
      }

      auto err_str = cert_chain.verify(x509.get());
      if (err_str) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;

        return verified;
      }

      verified = 1;

      return verified;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<SimpleWeb::HTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTPS>;
    https_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) { pair<SimpleWeb::HTTPS>(add_cert, resp, req); };
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) { launch(host_audio, resp, req); };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) { resume(host_audio, resp, req); };
    https_server.resource["^/cancel$"]["GET"] = cancel;

    https_server.config.reuse_address = true;
    https_server.config.address = net::af_to_any_address_string(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = [&add_cert](auto resp, auto req) { pair<SimpleWeb::HTTP>(add_cert, resp, req); };

    http_server.config.reuse_address = true;
    http_server.config.address = net::af_to_any_address_string(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        http_server->start();
      }
      catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl { accept_and_run, &https_server };
    std::thread tcp { accept_and_run, &http_server };

    // Wait for any event
    shutdown_event->view();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  void
  erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
  }

  int
  unpair_client(std::string uuid) {
    int removed = 0;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it).uuid == uuid) {
        // Find matching cert and remove it
        for (auto cert = client.certs.begin(); cert != client.certs.end();) {
          if ((*cert) == (*it).cert) {
            cert = client.certs.erase(cert);
            removed++;
          }
          else {
            ++cert;
          }
        }

        // And then remove the named cert
        it = client.named_devices.erase(it);
        removed++;
      }
      else {
        ++it;
      }
    }

    save_state();
    load_state();
    return removed;
  }
}  // namespace nvhttp
