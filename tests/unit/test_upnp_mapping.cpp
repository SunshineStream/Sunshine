/**
 * @file tests/unit/test_upnp_mapping.cpp
 * @brief Verify UPnP ownership boundaries and permanent-rule crash recovery.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <map>

// local includes
#include <src/upnp_mapping.h>

namespace {
  /**
   * @brief Construct a rule owned by Sunshine on a given gateway.
   * @param control_url Gateway's SOAP endpoint.
   * @param target Destination address.
   * @param external_port Gateway and destination port.
   * @return Rule for Sunshine's HTTP port.
   */
  upnp::owned_mapping_t make_rule(const std::string &control_url, const std::string &target, const std::string &external_port = "47989") {
    upnp::owned_mapping_t rule;
    rule.control_url = control_url;
    rule.service = "urn:schemas-upnp-org:service:WANIPConnection:1";
    rule.target = target;
    rule.external_port = external_port;
    rule.internal_port = external_port;
    rule.protocol = "TCP";
    rule.description = "Sunshine - Client HTTP";
    return rule;
  }

  /**
   * @brief Construct a representative rule owned by Sunshine.
   * @return Rule on the inner gateway.
   */
  upnp::owned_mapping_t sample_mapping() {
    return make_rule("http://192.168.1.1/control", "192.168.1.20");
  }

  TEST(UpnpMappingOwnershipTest, DistinguishesOwnedManualAndConflictingRules) {
    const auto mapping = sample_mapping();
    EXPECT_EQ(upnp::classify_mapping(0, mapping.target, mapping.internal_port, mapping.description, mapping), upnp::mapping_state_e::ours);
    EXPECT_EQ(upnp::classify_mapping(0, mapping.target, mapping.internal_port, "Manual forwarding", mapping), upnp::mapping_state_e::existing);
    EXPECT_EQ(upnp::classify_mapping(0, "192.168.1.99", mapping.internal_port, mapping.description, mapping), upnp::mapping_state_e::conflict);
    EXPECT_EQ(upnp::classify_mapping(0, mapping.target, "48000", mapping.description, mapping), upnp::mapping_state_e::conflict);
  }

  TEST(UpnpMappingOwnershipTest, UnreportedInternalPortIsNotAnotherDestination) {
    const auto mapping = sample_mapping();
    for (const auto *port : {"0", ""}) {
      EXPECT_EQ(upnp::classify_mapping(0, mapping.target, port, mapping.description, mapping), upnp::mapping_state_e::ours) << port;
      EXPECT_EQ(upnp::classify_mapping(0, mapping.target, port, "Manual forwarding", mapping), upnp::mapping_state_e::existing) << port;
      EXPECT_EQ(upnp::classify_mapping(0, "192.168.1.99", port, mapping.description, mapping), upnp::mapping_state_e::conflict) << port;
    }
  }

  TEST(UpnpMappingOwnershipTest, LookupErrorsAreNeverProofOfOwnershipOrAbsence) {
    const auto mapping = sample_mapping();
    EXPECT_EQ(upnp::classify_mapping(714, "", "", "", mapping), upnp::mapping_state_e::missing);
    for (const int error : {-1, 501, 606, 718, 729}) {
      EXPECT_EQ(upnp::classify_mapping(error, mapping.target, mapping.internal_port, mapping.description, mapping), upnp::mapping_state_e::unknown);
    }
  }

  TEST(UpnpMappingJournalTest, RecoversCreationIntentWithoutNeedingTheLastDiscoveryResult) {
    const auto mapping = sample_mapping();
    const auto recovered = upnp::read_mapping_journal(upnp::mapping_journal_event(mapping, true));
    ASSERT_EQ(recovered.size(), 1);
    EXPECT_EQ(recovered.front(), mapping);
    EXPECT_TRUE(recovered.front().permanent);
  }

  TEST(UpnpMappingJournalTest, KeepsBothGatewaysAndOldDestinationsUntilRemoved) {
    const auto inner = sample_mapping();
    const auto outer = make_rule("http://10.0.0.1/control", "10.0.0.42");
    const auto changed = make_rule("http://10.0.0.1/control", "10.0.0.43");
    auto events = upnp::mapping_journal_event(inner, true) + upnp::mapping_journal_event(outer, true) + upnp::mapping_journal_event(changed, true);
    ASSERT_EQ(upnp::read_mapping_journal(events).size(), 3);
    events += upnp::mapping_journal_event(outer, false);
    const auto recovered = upnp::read_mapping_journal(events);
    ASSERT_EQ(recovered.size(), 2);
    EXPECT_EQ(recovered[0], inner);
    EXPECT_EQ(recovered[1], changed);
  }

  TEST(UpnpMappingJournalTest, RefreshesDoNotDuplicateAndCompletedRemovalClearsIntent) {
    auto mapping = sample_mapping();
    auto events = upnp::mapping_journal_event(mapping, true);
    mapping.generation = 123;
    mapping.permanent = true;
    events += upnp::mapping_journal_event(mapping, true);
    EXPECT_EQ(upnp::read_mapping_journal(events).size(), 1);
    events += upnp::mapping_journal_event(mapping, false);
    events += upnp::mapping_journal_event(mapping, false);
    EXPECT_TRUE(upnp::read_mapping_journal(events).empty());
  }

  TEST(UpnpMappingJournalTest, IgnoresInterruptedWritesWithoutLosingEarlierRules) {
    const auto mapping = sample_mapping();
    const auto complete = upnp::mapping_journal_event(mapping, true);
    const auto recovered = upnp::read_mapping_journal(complete + "+ \"http://incomplete");
    ASSERT_EQ(recovered.size(), 1);
    EXPECT_EQ(recovered.front(), mapping);
    EXPECT_TRUE(upnp::read_mapping_journal(complete + "+ \"http://incomplete" + upnp::mapping_journal_event(mapping, false)).empty());
  }

  TEST(UpnpMappingJournalTest, IgnoresInvalidEventsAndIncompleteIdentities) {
    auto mapping = sample_mapping();
    auto event = upnp::mapping_journal_event(mapping, true);
    event[1] = '?';
    EXPECT_TRUE(upnp::read_mapping_journal(event).empty());
    mapping.protocol = "SCTP";
    EXPECT_TRUE(upnp::read_mapping_journal(upnp::mapping_journal_event(mapping, true)).empty());
    mapping = sample_mapping();
    mapping.control_url.clear();
    EXPECT_TRUE(upnp::read_mapping_journal(upnp::mapping_journal_event(mapping, true)).empty());
    EXPECT_TRUE(upnp::read_mapping_journal("\nmalformed\n").empty());
  }

  TEST(UpnpMappingJournalTest, PreservesQuotedDescriptionsAndEndpointCharacters) {
    auto mapping = make_rule("http://192.168.1.1/control?value=\\quoted", "192.168.1.20");
    mapping.description = "Sunshine - \"Client HTTP\"";
    const auto recovered = upnp::read_mapping_journal(upnp::mapping_journal_event(mapping, true));
    ASSERT_EQ(recovered.size(), 1);
    EXPECT_EQ(recovered.front(), mapping);
  }

  TEST(UpnpMappingJournalTest, RefusesLineBreaksThatWouldSplitARecoveryRecord) {
    auto mapping = sample_mapping();
    mapping.control_url += "\nextra";
    EXPECT_TRUE(upnp::mapping_journal_event(mapping, true).empty());
    mapping = sample_mapping();
    mapping.description += '\r';
    EXPECT_TRUE(upnp::mapping_journal_event(mapping, true).empty());
  }

  /**
   * @brief Gateway that answers lookups from a table and records every change.
   */
  struct recording_gateway_t {
    std::map<std::string, upnp::mapping_entry_t, std::less<>> entries;  ///< Lookup answers by gateway port; other ports report 714.
    std::vector<int> add_results;  ///< Results of successive adds; 0 once exhausted.
    std::vector<std::string> added_leases;  ///< Lease requested by every add.
    int removes = 0;  ///< Number of removals.

    /**
     * @brief Answer a lookup from the table.
     * @param rule Requested rule.
     * @return Configured answer, or 714 (no such mapping).
     */
    upnp::mapping_entry_t lookup(const upnp::owned_mapping_t &rule) const {
      const auto found = entries.find(rule.external_port);
      return found == entries.end() ? upnp::mapping_entry_t {714} : found->second;
    }

    /**
     * @brief Record an add.
     * @param lease Requested lease.
     * @return Next configured result.
     */
    int add(const upnp::owned_mapping_t &, const std::string &lease) {
      const auto index = added_leases.size();
      added_leases.push_back(lease);
      return index < add_results.size() ? add_results[index] : 0;
    }

    /**
     * @brief Record a removal.
     * @return Success.
     */
    int remove(const upnp::owned_mapping_t &) {
      ++removes;
      return 0;
    }
  };

  static_assert(upnp::mapping_gateway<recording_gateway_t>);

  /**
   * @brief Construct a lookup answer that forwards to the rule's destination.
   * @param rule Requested rule.
   * @param description Description reported by the gateway.
   * @param enabled Reported `NewEnabled` value.
   * @param lease Reported remaining lease.
   * @return Lookup answer.
   */
  upnp::mapping_entry_t entry_for(const upnp::owned_mapping_t &rule, const std::string &description, const std::string &enabled, const std::string &lease) {
    return {0, rule.target, rule.internal_port, description, enabled, lease};
  }

  /**
   * @brief One mapping pass through the production mapping path, with its records.
   */
  struct mapping_pass_t {
    recording_gateway_t gateway;  ///< Gateway used by the pass.
    std::vector<upnp::owned_mapping_t> remembered;  ///< Rules recorded as owned by Sunshine.
    std::vector<upnp::map_outcome_e> outcomes;  ///< Outcome reported for each rule.

    /**
     * @brief Map the rules.
     * @param rules Requested rules.
     * @return Whether the pass completed.
     */
    bool run(const std::vector<upnp::owned_mapping_t> &rules) {
      return upnp::map_ports(
        gateway,
        rules,
        "3600",
        [this](const upnp::owned_mapping_t &rule) {
          remembered.push_back(rule);
          return true;
        },
        [] {
          return false;
        },
        [this](const upnp::owned_mapping_t &, upnp::map_outcome_e outcome, int) {
          outcomes.push_back(outcome);
        }
      );
    }
  };

  TEST(UpnpMapPortTest, DisabledManualRuleIsLeftUnchangedAndLeavesThePassIncomplete) {
    const auto rule = sample_mapping();
    mapping_pass_t pass;
    pass.gateway.entries[rule.external_port] = entry_for(rule, "Manual forwarding", "0", "3600");
    EXPECT_FALSE(pass.run({rule}));
    EXPECT_EQ(pass.outcomes, std::vector {upnp::map_outcome_e::disabled});
    EXPECT_TRUE(pass.gateway.added_leases.empty());
    EXPECT_EQ(pass.gateway.removes, 0);
    EXPECT_TRUE(pass.remembered.empty());
  }

  TEST(UpnpMapPortTest, EnabledManualRuleIsReusedWithoutRenewal) {
    const auto rule = sample_mapping();
    mapping_pass_t pass;
    pass.gateway.entries[rule.external_port] = entry_for(rule, "Manual forwarding", "1", "3600");
    EXPECT_TRUE(pass.run({rule}));
    EXPECT_EQ(pass.outcomes, std::vector {upnp::map_outcome_e::reused});
    EXPECT_TRUE(pass.gateway.added_leases.empty());
    EXPECT_EQ(pass.gateway.removes, 0);
    EXPECT_TRUE(pass.remembered.empty());
  }

  TEST(UpnpMapPortTest, DisabledManualRuleDoesNotStopTheOtherPorts) {
    const auto manual = sample_mapping();
    const auto other = make_rule("http://192.168.1.1/control", "192.168.1.20", "47984");
    mapping_pass_t pass;
    pass.gateway.entries[manual.external_port] = entry_for(manual, "Manual forwarding", "0", "3600");
    EXPECT_FALSE(pass.run({manual, other}));
    EXPECT_EQ(pass.outcomes, (std::vector {upnp::map_outcome_e::disabled, upnp::map_outcome_e::mapped}));
    EXPECT_EQ(pass.gateway.added_leases, std::vector<std::string> {"3600"});
    ASSERT_EQ(pass.remembered.size(), 1);
    EXPECT_EQ(pass.remembered.front(), other);
  }

  TEST(UpnpMapPortTest, DisabledOwnedPermanentRuleIsEnabledAgain) {
    const auto rule = sample_mapping();
    mapping_pass_t pass;
    pass.gateway.entries[rule.external_port] = entry_for(rule, rule.description, "0", "0");
    pass.gateway.add_results = {725, 0};
    EXPECT_TRUE(pass.run({rule}));
    EXPECT_EQ(pass.outcomes, std::vector {upnp::map_outcome_e::mapped});
    EXPECT_EQ(pass.gateway.added_leases, (std::vector<std::string> {"3600", "0"}));
    EXPECT_EQ(pass.gateway.removes, 0);
    ASSERT_FALSE(pass.remembered.empty());
    EXPECT_TRUE(pass.remembered.front().permanent);
    EXPECT_TRUE(pass.remembered.back().permanent);
  }

  TEST(UpnpMapPortTest, EnabledOwnedPermanentRuleIsKeptWithoutAdding) {
    const auto rule = sample_mapping();
    mapping_pass_t pass;
    pass.gateway.entries[rule.external_port] = entry_for(rule, rule.description, "1", "0");
    EXPECT_TRUE(pass.run({rule}));
    EXPECT_EQ(pass.outcomes, std::vector {upnp::map_outcome_e::reused});
    EXPECT_TRUE(pass.gateway.added_leases.empty());
    EXPECT_EQ(pass.gateway.removes, 0);
    ASSERT_EQ(pass.remembered.size(), 1);
    EXPECT_TRUE(pass.remembered.front().permanent);
  }

  TEST(UpnpMapPortTest, RecognizesUpnpBooleanSpellings) {
    for (const auto *value : {"0", "false", "False", "NO"}) {
      EXPECT_TRUE(upnp::is_disabled_mapping(value)) << value;
    }
    for (const auto *value : {"1", "true", "YES", ""}) {
      EXPECT_FALSE(upnp::is_disabled_mapping(value)) << value;
    }
  }
}  // namespace
