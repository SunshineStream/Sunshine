/**
 * @file src/upnp_mapping.h
 * @brief Ownership and recovery records for UPnP port mappings.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace upnp {
  /**
   * @brief A forwarding rule Sunshine owns, including its gateway and destination.
   */
  struct owned_mapping_t {
    std::string control_url;  ///< Gateway's SOAP endpoint.
    std::string service;  ///< Gateway's WAN connection service.
    std::string target;  ///< Host or inner router receiving the forwarded traffic.
    std::string external_port;  ///< Gateway port.
    std::string internal_port;  ///< Destination port.
    std::string protocol;  ///< TCP or UDP.
    std::string description;  ///< Description used when Sunshine created the rule.
    bool permanent = false;  ///< Rule requires recovery after an unexpected process exit.
    std::size_t generation = 0;  ///< Last successful discovery pass that wanted this exact rule.

    /**
     * @brief Compare persistent rule identities, excluding refresh bookkeeping.
     * @param other Rule to compare.
     * @return Whether both records identify the same forwarding rule.
     */
    bool operator==(const owned_mapping_t &other) const {
      return std::tie(control_url, service, target, external_port, internal_port, protocol, description) ==
             std::tie(other.control_url, other.service, other.target, other.external_port, other.internal_port, other.protocol, other.description);
    }
  };

  /**
   * @brief Relationship between a gateway's existing mapping and the requested rule.
   */
  enum class mapping_state_e {
    missing,  ///< Gateway explicitly reports no such mapping.
    ours,  ///< Destination, port and description match Sunshine's record.
    existing,  ///< Someone else's rule already forwards to the requested destination.
    conflict,  ///< Rule forwards elsewhere and must be left untouched.
    unknown,  ///< Lookup failed; ownership cannot be established.
  };

  /**
   * @brief Classify a lookup without treating a destination alone as proof of ownership.
   * @param result MiniUPnPc lookup result.
   * @param client Reported destination address.
   * @param port Reported destination port.
   * @param description Reported description.
   * @param wanted Requested or previously recorded rule.
   * @return Ownership classification.
   */
  inline mapping_state_e classify_mapping(int result, std::string_view client, std::string_view port, std::string_view description, const owned_mapping_t &wanted) {
    if (result == 714) {
      return mapping_state_e::missing;
    }
    if (result != 0) {
      return mapping_state_e::unknown;
    }
    // Some gateways report the internal port of an existing rule as 0 or leave it empty; that is not another destination.
    const bool port_reported = !port.empty() && port != "0";
    if (client != wanted.target || (port_reported && port != wanted.internal_port)) {
      return mapping_state_e::conflict;
    }
    return description == wanted.description ? mapping_state_e::ours : mapping_state_e::existing;
  }

  /**
   * @brief Whether a gateway reports a rule as switched off.
   * @param enabled The rule's `NewEnabled` value; UPnP booleans may be `0`/`1`, `false`/`true` or `no`/`yes`.
   * @return Whether the rule cannot forward traffic. A missing value is not proof that it is disabled.
   */
  inline bool is_disabled_mapping(std::string_view enabled) {
    return std::ranges::any_of(std::array {std::string_view {"0"}, std::string_view {"false"}, std::string_view {"no"}}, [enabled](std::string_view value) {
      return std::ranges::equal(enabled, value, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
      });
    });
  }

  /**
   * @brief A gateway rule as reported by a lookup.
   */
  struct mapping_entry_t {
    int result = 0;  ///< MiniUPnPc lookup result.
    std::string client;  ///< Destination address.
    std::string port;  ///< Destination port.
    std::string description;  ///< Rule description.
    std::string enabled;  ///< Whether the rule is enabled.
    std::string lease;  ///< Remaining lease in seconds, `0` for a rule without expiry.
  };

  /**
   * @brief Gateway calls used to manage rules: MiniUPnPc in Sunshine, a recorder in tests.
   * @details `lookup` reports the rule on the requested gateway port, `add` adds or renews a rule with the given
   * lease in seconds (`0` for no expiry), and `remove` deletes a rule. `add` and `remove` return MiniUPnPc results.
   */
  template<class T>
  concept mapping_gateway = requires(T &calls, const owned_mapping_t &rule, const std::string &lease) {
    { calls.lookup(rule) } -> std::same_as<mapping_entry_t>;
    { calls.add(rule, lease) } -> std::convertible_to<int>;
    { calls.remove(rule) } -> std::convertible_to<int>;
  };

  /**
   * @brief Result of one mapping attempt.
   */
  enum class map_outcome_e {
    mapped,  ///< Sunshine added or renewed the rule.
    reused,  ///< An enabled rule already forwards to the destination and needs no renewal.
    disabled,  ///< Someone else's rule forwards to the destination but is disabled; left unchanged.
    conflict,  ///< The port forwards elsewhere; left unchanged.
    unknown,  ///< The lookup failed; nothing was changed.
    unrecorded,  ///< Ownership could not be recorded; nothing more was changed.
    cancelled,  ///< Shutdown was requested before the rule was added.
    failed,  ///< The gateway refused the rule.
  };

  /**
   * @brief Whether an outcome leaves the port forwarded to the destination.
   * @param outcome Mapping outcome.
   * @return Whether the port is open.
   */
  inline bool is_mapped(map_outcome_e outcome) {
    return outcome == map_outcome_e::mapped || outcome == map_outcome_e::reused;
  }

  /**
   * @brief Map one port without deleting, renewing or adopting a rule belonging to another application.
   * @param gateway Gateway holding the rule.
   * @param wanted Desired rule.
   * @param lifetime Lease requested for a renewable rule, in seconds.
   * @param remember Records a rule Sunshine owns; must persist recovery intent before a rule without expiry is added.
   * @param is_cancelled Whether shutdown was requested.
   * @param error Receives the last gateway result.
   * @return Mapping outcome.
   */
  template<mapping_gateway Gateway, class Remember, class Cancelled>
  map_outcome_e map_port(Gateway &gateway, owned_mapping_t wanted, const std::string &lifetime, Remember &&remember, Cancelled &&is_cancelled, int &error) {
    const auto entry = gateway.lookup(wanted);
    error = entry.result;
    const bool disabled = is_disabled_mapping(entry.enabled);
    switch (classify_mapping(entry.result, entry.client, entry.port, entry.description, wanted)) {
      case mapping_state_e::conflict:
        return map_outcome_e::conflict;
      case mapping_state_e::unknown:
        return map_outcome_e::unknown;
      case mapping_state_e::existing:
        // Never renew or remove someone else's rule, but a disabled one does not open the port.
        return disabled ? map_outcome_e::disabled : map_outcome_e::reused;
      case mapping_state_e::ours:
        wanted.permanent = entry.lease == "0";
        if (!remember(wanted)) {
          return map_outcome_e::unrecorded;
        }
        if (wanted.permanent && !disabled) {
          return map_outcome_e::reused;
        }
        // Renew the lease, or enable Sunshine's own disabled rule again.
        break;
      case mapping_state_e::missing:
        break;
    }
    if (is_cancelled()) {
      return map_outcome_e::cancelled;
    }
    error = gateway.add(wanted, lifetime);
    if (error == 725 && !is_cancelled()) {  // OnlyPermanentLeasesSupported
      wanted.permanent = true;
      // Persist intent first: a process crash immediately after adding the rule must be recoverable.
      if (!remember(wanted)) {
        return map_outcome_e::unrecorded;
      }
      error = gateway.add(wanted, "0");
    }
    if (error != 0) {
      return map_outcome_e::failed;
    }
    return remember(std::move(wanted)) ? map_outcome_e::mapped : map_outcome_e::unrecorded;
  }

  /**
   * @brief Map every requested rule on one gateway, stopping early on shutdown.
   * @param gateway Gateway holding the rules.
   * @param rules Desired rules.
   * @param lifetime Lease requested for renewable rules, in seconds.
   * @param remember Records a rule Sunshine owns.
   * @param is_cancelled Whether shutdown was requested.
   * @param report Receives each rule, its outcome and the last gateway result.
   * @return Whether every requested port forwards to its destination.
   */
  template<mapping_gateway Gateway, class Remember, class Cancelled, class Report>
  bool map_ports(Gateway &gateway, const std::vector<owned_mapping_t> &rules, const std::string &lifetime, Remember &&remember, Cancelled &&is_cancelled, Report &&report) {
    bool complete = true;
    for (const auto &rule : rules) {
      if (is_cancelled()) {
        return false;
      }
      int error = 0;
      const auto outcome = map_port(gateway, rule, lifetime, remember, is_cancelled, error);
      report(rule, outcome, error);
      complete = is_mapped(outcome) && complete;
    }
    return complete;
  }

  /**
   * @brief Encode one append-only recovery event before adding or after removing a permanent rule.
   * @param mapping Rule being recorded.
   * @param added Whether this records creation intent rather than completed removal.
   * @return A newline-delimited event, or an empty string for an invalid identity.
   */
  inline std::string mapping_journal_event(const owned_mapping_t &mapping, bool added) {
    for (const auto *field : {&mapping.control_url, &mapping.service, &mapping.target, &mapping.external_port, &mapping.internal_port, &mapping.protocol, &mapping.description}) {
      if (field->empty() || field->find_first_of("\r\n") != std::string::npos) {
        return {};
      }
    }
    std::ostringstream out;
    out << '\n'
        << (added ? '+' : '-') << ' ' << std::quoted(mapping.control_url) << ' ' << std::quoted(mapping.service) << ' '
        << std::quoted(mapping.target) << ' ' << std::quoted(mapping.external_port) << ' ' << std::quoted(mapping.internal_port)
        << ' ' << std::quoted(mapping.protocol) << ' ' << std::quoted(mapping.description) << '\n';
    return out.str();
  }

  /**
   * @brief Recover outstanding permanent rules, ignoring incomplete or invalid journal lines.
   * @param text Previously written recovery events.
   * @return Rules whose creation intent has no matching completed removal.
   */
  inline std::vector<owned_mapping_t> read_mapping_journal(const std::string &text) {
    std::vector<owned_mapping_t> mappings;
    std::istringstream input {text};
    std::string line;
    while (std::getline(input, line)) {
      std::istringstream entry {line};
      char event = 0;
      owned_mapping_t mapping;
      if (!(entry >> event >> std::quoted(mapping.control_url) >> std::quoted(mapping.service) >> std::quoted(mapping.target) >> std::quoted(mapping.external_port) >> std::quoted(mapping.internal_port) >> std::quoted(mapping.protocol) >> std::quoted(mapping.description)) || (event != '+' && event != '-') || mapping.control_url.empty() || mapping.service.empty() || mapping.target.empty() || mapping.external_port.empty() || mapping.internal_port.empty() || (mapping.protocol != "TCP" && mapping.protocol != "UDP") || mapping.description.empty()) {
        continue;
      }
      mapping.permanent = true;
      const auto found = std::find(mappings.begin(), mappings.end(), mapping);
      if (event == '+' && found == mappings.end()) {
        mappings.push_back(std::move(mapping));
      } else if (event == '-' && found != mappings.end()) {
        mappings.erase(found);
      }
    }
    return mappings;
  }
}  // namespace upnp
