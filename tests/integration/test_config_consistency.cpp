/**
 * @file tests/integration/test_config_consistency.cpp
 * @brief Test configuration consistency across all configuration files
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "src/file_handler.h"

class ConfigConsistencyTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    // Define the expected mapping between documentation sections and UI tabs
    expectedDocToTabMapping = {
      {"General", "general"},
      {"Input", "input"},
      {"Audio/Video", "av"},
      {"Network", "network"},
      {"Config Files", "files"},
      {"Advanced", "advanced"},
      {"NVIDIA NVENC Encoder", "nv"},
      {"Intel QuickSync Encoder", "qsv"},
      {"AMD AMF Encoder", "amd"},
      {"VideoToolbox Encoder", "vt"},
      {"VA-API Encoder", "vaapi"},
      {"Vulkan Encoder", "vulkan"},
      {"Software Encoder", "sw"}
    };
  }

  /**
   * @brief Read an integration-test fixture copied beside the test executable.
   *
   * @param relativePath Repository-relative path of the copied fixture.
   * @return Contents of the fixture, or an empty string when it cannot be read.
   */
  static std::string readFixture(const std::string_view relativePath) {
    const auto fixturePath = std::format("{}/{}", SUNSHINE_TEST_BIN_DIR, relativePath);
    return file_handler::read_file(fixturePath.c_str());
  }

  // Extract config options from config.cpp - the authoritative source
  static std::set<std::string, std::less<>> extractConfigCppOptions() {
    std::set<std::string, std::less<>> options;
    std::string content = readFixture("src/config.cpp");

    // Regex patterns to match different config option types in config.cpp
    const std::vector patterns = {
      std::regex(R"DELIM((?:string_f|path_f|string_restricted_f)\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
      std::regex(R"DELIM((?:int_f|int_between_f)\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
      std::regex(R"DELIM(bool_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
      std::regex(R"DELIM((?:double_f|double_between_f)\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
      std::regex(R"DELIM(generic_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
      std::regex(R"DELIM(list_prep_cmd_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM"),
      std::regex(R"DELIM(map_int_int_f\s*\(\s*vars\s*,\s*"([^"]+)")DELIM")
    };

    for (const auto &pattern : patterns) {
      std::sregex_iterator iter(content.begin(), content.end(), pattern);

      for (std::sregex_iterator end; iter != end; ++iter) {
        std::string optionName = (*iter)[1].str();
        options.insert(optionName);
      }
    }

    return options;
  }

  /**
   * @brief Parse the ordered configuration-tab defaults used by the Web UI.
   *
   * @return Parsed configuration-tab array, or an empty array when the fixture is unavailable.
   */
  static nlohmann::ordered_json extractConfigTabs() {
    const std::string content = readFixture("src_assets/common/assets/web/configs/config_tabs.json");
    if (content.empty()) {
      return nlohmann::ordered_json::array();
    }

    return nlohmann::ordered_json::parse(content);
  }

  /**
   * @brief Extract option names and tab IDs from the ordered Web UI defaults.
   *
   * @tparam Container Map type receiving option-to-tab or tab-to-options entries.
   * @param container Destination container.
   */
  template<typename Container>
  static void extractOptionsFromConfigTabs(Container &container) {
    for (const auto &tab : extractConfigTabs()) {
      const std::string tabId = tab.at("id").get<std::string>();
      for (const auto &[optionName, value] : tab.at("options").items()) {
        (void) value;
        if constexpr (std::is_same_v<Container, std::map<std::string, std::string, std::less<>>>) {
          container[optionName] = tabId;
        } else if constexpr (std::is_same_v<Container, std::map<std::string, std::vector<std::string>, std::less<>>>) {
          container[tabId].push_back(optionName);
        }
      }
    }
  }

  // Helper function to trim whitespace from string
  static void trimWhitespace(std::string &str) {
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
  }

  // Helper function to extract option name from the Markdown line
  static std::string extractOptionFromMarkdownLine(const std::string &line) {
    const std::regex optionPattern(R"(^### ([^#\r\n]+))");
    if (std::smatch optionMatch; std::regex_search(line, optionMatch, optionPattern)) {
      std::string optionName = optionMatch[1].str();
      trimWhitespace(optionName);
      return optionName;
    }
    return "";
  }

  // Extract config options from the Web UI defaults
  static std::map<std::string, std::string, std::less<>> extractConfigHtmlOptions() {
    std::map<std::string, std::string, std::less<>> options;
    extractOptionsFromConfigTabs(options);
    return options;
  }

  // Extract config options from the Web UI defaults with order preserved
  static std::map<std::string, std::vector<std::string>, std::less<>> extractConfigHtmlOptionsWithOrder() {
    std::map<std::string, std::vector<std::string>, std::less<>> optionsByTab;
    extractOptionsFromConfigTabs(optionsByTab);
    return optionsByTab;
  }

  // Helper function to process markdown line for section headers
  static bool processSectionHeader(const std::string &line, std::string &currentSection) {
    const std::regex sectionPattern(R"(^## ([^#\r\n]+))");

    if (std::smatch sectionMatch; std::regex_search(line, sectionMatch, sectionPattern)) {
      currentSection = sectionMatch[1].str();
      trimWhitespace(currentSection);
      return true;
    }

    return false;
  }

  // Helper function to process markdown line for option headers
  static bool processOptionHeader(const std::string &line, const std::string_view currentSection, std::map<std::string, std::string, std::less<>> &options) {
    if (currentSection.empty()) {
      return false;
    }

    if (const std::string optionName = extractOptionFromMarkdownLine(line); !optionName.empty()) {
      options[optionName] = currentSection;
      return true;
    }

    return false;
  }

  // Extract config options from configuration.md
  static std::map<std::string, std::string, std::less<>> extractConfigMdOptions() {
    std::map<std::string, std::string, std::less<>> options;
    const std::string content = readFixture("docs/configuration.md");

    std::istringstream stream(content);
    std::string line;
    std::string currentSection;

    while (std::getline(stream, line)) {
      if (processSectionHeader(line, currentSection)) {
        continue;
      }

      processOptionHeader(line, currentSection, options);
    }

    return options;
  }

  // Helper function to process markdown option line for order-preserved extraction
  static void processMarkdownOptionLine(const std::string &line, const std::string &currentSection, std::map<std::string, std::vector<std::string>, std::less<>> &optionsBySection) {
    if (currentSection.empty()) {
      return;
    }

    if (const std::string optionName = extractOptionFromMarkdownLine(line); !optionName.empty()) {
      optionsBySection[currentSection].push_back(optionName);
    }
  }

  // Extract config options from configuration.md with order preserved
  static std::map<std::string, std::vector<std::string>, std::less<>> extractConfigMdOptionsWithOrder() {
    std::map<std::string, std::vector<std::string>, std::less<>> optionsBySection;
    const std::string content = readFixture("docs/configuration.md");

    std::istringstream stream(content);
    std::string line;
    std::string currentSection;

    while (std::getline(stream, line)) {
      if (processSectionHeader(line, currentSection)) {
        continue;
      }

      processMarkdownOptionLine(line, currentSection, optionsBySection);
    }

    return optionsBySection;
  }

  // Helper function to find the config section end
  static size_t findConfigSectionEnd(const std::string &content, size_t configStart) {
    size_t braceCount = 1;
    size_t configEnd = configStart;

    while (configStart < content.length() && braceCount > 0) {
      if (content[configStart] == '{') {
        braceCount++;
      } else if (content[configStart] == '}') {
        braceCount--;
      }
      configEnd = configStart;
      configStart++;
    }

    return configEnd;
  }

  // Helper function to extract keys from a config section
  static void extractKeysFromConfigSection(const std::string_view configSection, std::set<std::string, std::less<>> &options) {
    const std::regex keyPattern(R"DELIM("([^"]+)":\s*)DELIM");
    std::string configStr(configSection);
    std::sregex_iterator iter(configStr.begin(), configStr.end(), keyPattern);

    for (const std::sregex_iterator end; iter != end; ++iter) {
      options.insert((*iter)[1].str());
    }
  }

  // Extract config options from en.json
  static std::set<std::string, std::less<>> extractEnJsonConfigOptions() {
    std::set<std::string, std::less<>> options;
    const std::string content = readFixture("src_assets/common/assets/web/public/assets/locale/en.json");

    // Look for the config section
    const std::regex configSectionPattern(R"DELIM("config":\s*\{)DELIM");
    std::smatch match;

    if (!std::regex_search(content, match, configSectionPattern)) {
      return options;
    }

    // Find the config section and extract keys
    const size_t configStart = match.position() + match.length();
    const size_t configEnd = findConfigSectionEnd(content, configStart);
    const std::string configSection = content.substr(configStart, configEnd - configStart);

    extractKeysFromConfigSection(configSection, options);

    return options;
  }

  std::map<std::string, std::string, std::less<>> expectedDocToTabMapping;

  // Helper function to check if an option exists in HTML options
  static bool isOptionInHtml(const std::string &option, const std::map<std::string, std::string, std::less<>> &htmlOptions) {
    return htmlOptions.contains(option);
  }

  // Helper function to check if an option exists in MD options
  static bool isOptionInMd(const std::string &option, const std::map<std::string, std::string, std::less<>> &mdOptions) {
    return mdOptions.contains(option);
  }

  // Helper function to validate option existence across files
  static void validateOptionExistence(const std::string &option, const std::map<std::string, std::string, std::less<>> &htmlOptions, const std::map<std::string, std::string, std::less<>> &mdOptions, const std::set<std::string, std::less<>> &jsonOptions, std::vector<std::string> &missingFromFiles) {
    if (!isOptionInHtml(option, htmlOptions)) {
      missingFromFiles.push_back(std::format("config_tabs.json missing: {}", option));
    }

    if (!isOptionInMd(option, mdOptions)) {
      missingFromFiles.push_back(std::format("configuration.md missing: {}", option));
    }

    if (!jsonOptions.contains(option)) {
      missingFromFiles.push_back(std::format("en.json missing: {}", option));
    }
  }

  // Helper function to check tab correspondence with documentation sections
  static void checkTabCorrespondence(const std::string &tab, const std::map<std::string, std::string, std::less<>> &expectedDocToTabMapping, const std::set<std::string, std::less<>> &mdSections, std::vector<std::string> &inconsistencies) {
    bool found = false;

    for (const auto &[docSection, expectedTab] : expectedDocToTabMapping) {
      if (expectedTab != tab) {
        continue;
      }

      if (!mdSections.contains(docSection)) {
        inconsistencies.push_back(std::format("Tab '{}' maps to doc section '{}' but section not found", tab, docSection));
      }
      found = true;
      break;
    }

    if (!found) {
      inconsistencies.push_back(std::format("Tab '{}' has no corresponding documentation section", tab));
    }
  }

  // Helper function to check if a test fake option is found in missing files
  static void checkTestDummyDetection(const std::vector<std::string> &missingFromFiles, const std::string &testDummyOption, bool &foundMissingDummyInHtml, bool &foundMissingDummyInMd, bool &foundMissingDummyInJson) {
    for (const auto &missing : missingFromFiles) {
      if (!missing.contains(testDummyOption)) {
        continue;
      }

      if (missing.contains("config_tabs.json")) {
        foundMissingDummyInHtml = true;
      }
      if (missing.contains("configuration.md")) {
        foundMissingDummyInMd = true;
      }
      if (missing.contains("en.json")) {
        foundMissingDummyInJson = true;
      }
    }
  }

  // Helper function to create comma-separated string from vector
  static std::string buildCommaSeparatedString(const std::vector<std::string> &options) {
    std::string result;
    for (size_t i = 0; i < options.size(); ++i) {
      if (i > 0) {
        result += ", ";
      }
      result += options[i];
    }
    return result;
  }
};

TEST_F(ConfigConsistencyTest, AllConfigOptionsExistInAllFiles) {
  const auto cppOptions = extractConfigCppOptions();
  const auto htmlOptions = extractConfigHtmlOptions();
  const auto mdOptions = extractConfigMdOptions();
  const auto jsonOptions = extractEnJsonConfigOptions();

  // Options that are internal/special and shouldn't be in UI/docs
  const std::set<std::string, std::less<>> internalOptions = {
    "flags",  // Internal config flags, not user-configurable
    "external_audio"  // Deprecated migration-only key, unified into keep_default_sink: remove this exception together with the external_audio migration shim in config parsing (legacy_external_audio in src/config.cpp) when that migration is dropped
  };

  std::vector<std::string> missingFromFiles;

  // Check that all config.cpp options exist in other files (except internal ones)
  for (const auto &option : cppOptions) {
    if (internalOptions.contains(option)) {
      continue;  // Skip internal options
    }

    validateOptionExistence(option, htmlOptions, mdOptions, jsonOptions, missingFromFiles);
  }

  if (!missingFromFiles.empty()) {
    std::string errorMsg = "Config options missing from files:\n";
    for (const auto &missing : missingFromFiles) {
      errorMsg += std::format("  {}\n", missing);
    }
    FAIL() << errorMsg;
  }
}

TEST_F(ConfigConsistencyTest, AllConfigSidebarTabsUseEnglishLocaleKeys) {
  const auto jsonOptions = extractEnJsonConfigOptions();
  std::vector<std::string> errors;

  for (const auto &tab : extractConfigTabs()) {
    const std::string tabId = tab.at("id").get<std::string>();
    const std::string nameKey = tab.at("nameKey").get<std::string>();
    constexpr std::string_view configPrefix = "config.";
    if (!nameKey.starts_with(configPrefix)) {
      errors.push_back(std::format("Tab '{}' does not use a localized name key", tabId));
    } else if (const std::string localeKey = nameKey.substr(configPrefix.size()); !jsonOptions.contains(localeKey)) {
      errors.push_back(std::format("Tab '{}' references missing en.json key '{}'", tabId, nameKey));
    }
  }

  EXPECT_TRUE(errors.empty()) << buildCommaSeparatedString(errors);
}

TEST_F(ConfigConsistencyTest, ConfigSidebarTabsDoNotNavigateAway) {
  const std::string content = readFixture("src_assets/common/assets/web/Config.vue");
  const std::regex tabButtonPattern(R"(<button\s+type="button"\s+class="nav-link")");
  const std::sregex_iterator tabButtonBegin(content.begin(), content.end(), tabButtonPattern);
  const std::sregex_iterator tabButtonEnd;

  EXPECT_EQ(std::distance(tabButtonBegin, tabButtonEnd), 2);
  EXPECT_EQ(content.find("href=\"#\""), std::string::npos);
}

TEST_F(ConfigConsistencyTest, KeybindingsAreAvailableInWebUi) {
  const std::string content = readFixture("src_assets/common/assets/web/configs/tabs/Inputs.vue");
  const std::string selectContent = readFixture("src_assets/common/assets/web/configs/VirtualKeyCodeSelect.vue");
  const std::string keyCodeContent = readFixture("src_assets/common/assets/web/configs/virtual_key_codes.js");

  EXPECT_NE(
    content.find("id=\"keybindings\" class=\"mb-3\" v-if=\"config.keyboard === 'enabled'\""),
    std::string::npos
  );
  EXPECT_NE(content.find("class=\"keybinding-grid\""), std::string::npos);
  EXPECT_NE(content.find("v-for=\"(binding, index) in keybindingPairs\""), std::string::npos);
  EXPECT_NE(content.find("v-model=\"binding.source\""), std::string::npos);
  EXPECT_NE(content.find("v-model=\"binding.destination\""), std::string::npos);
  EXPECT_NE(selectContent.find("v-for=\"keyCode in virtualKeyCodes\""), std::string::npos);
  EXPECT_NE(selectContent.find("{{ keyCode.code }} ({{ keyCode.description }})"), std::string::npos);
  EXPECT_NE(content.find("@click=\"addKeybinding\""), std::string::npos);
  EXPECT_NE(content.find("@click=\"removeKeybinding(index)\""), std::string::npos);
  EXPECT_NE(content.find("windows/win32/inputdev/virtual-key-codes"), std::string::npos);

  const std::regex keyCodePattern(R"(keyCode\((0x[0-9A-F]{2}),)");
  const std::sregex_iterator keyCodeBegin(keyCodeContent.begin(), keyCodeContent.end(), keyCodePattern);
  const std::sregex_iterator keyCodeEnd;
  std::set<std::string, std::less<>> uniqueKeyCodes;
  for (auto keyCode = keyCodeBegin; keyCode != keyCodeEnd; ++keyCode) {
    uniqueKeyCodes.insert((*keyCode)[1].str());
  }

  EXPECT_EQ(std::distance(keyCodeBegin, keyCodeEnd), 196);
  EXPECT_EQ(uniqueKeyCodes.size(), 196);
}

TEST_F(ConfigConsistencyTest, ConfigTabsMatchDocumentationSections) {
  auto htmlOptions = extractConfigHtmlOptions();
  auto mdOptions = extractConfigMdOptions();

  // Get unique tabs and sections
  std::set<std::string, std::less<>> htmlTabs;
  std::set<std::string, std::less<>> mdSections;

  for (const auto &tab : htmlOptions | std::views::values) {
    htmlTabs.insert(tab);
  }

  for (const auto &section : mdOptions | std::views::values) {
    mdSections.insert(section);
  }

  std::vector<std::string> inconsistencies;

  // Check that each HTML tab has a corresponding documentation section
  for (const auto &tab : htmlTabs) {
    checkTabCorrespondence(tab, expectedDocToTabMapping, mdSections, inconsistencies);
  }

  // Check that each documentation section has a corresponding HTML tab
  for (const auto &section : mdSections) {
    if (!expectedDocToTabMapping.contains(section)) {
      inconsistencies.push_back(std::format("Documentation section '{}' has no corresponding UI tab", section));
    }
  }

  if (!inconsistencies.empty()) {
    std::string errorMsg = "Tab/Section mapping inconsistencies:\n";
    for (const auto &inconsistency : inconsistencies) {
      errorMsg += std::format("  {}\n", inconsistency);
    }
    FAIL() << errorMsg;
  }
}

TEST_F(ConfigConsistencyTest, ConfigOptionsInSameOrderWithinSections) {
  // Extract options with order preserved
  auto htmlOptionsByTab = extractConfigHtmlOptionsWithOrder();
  auto mdOptionsBySection = extractConfigMdOptionsWithOrder();

  std::vector<std::string> orderInconsistencies;

  // Compare order for each tab/section pair
  for (const auto &[docSection, tabId] : expectedDocToTabMapping) {
    if (!htmlOptionsByTab.contains(tabId) || !mdOptionsBySection.contains(docSection)) {
      continue;  // Skip if either tab or section doesn't exist
    }

    const auto &htmlOrder = htmlOptionsByTab.at(tabId);
    const auto &mdOrder = mdOptionsBySection.at(docSection);

    // Find options that exist in both HTML and MD for this section
    std::vector<std::string> commonOptions;
    for (const auto &option : htmlOrder) {
      if (std::ranges::find(mdOrder, option) != mdOrder.end()) {
        commonOptions.push_back(option);
      }
    }

    // Filter MD order to only include common options in the same order they appear in MD
    std::vector<std::string> mdOrderFiltered;
    for (const auto &option : mdOrder) {
      if (std::ranges::find(commonOptions, option) != commonOptions.end()) {
        mdOrderFiltered.push_back(option);
      }
    }

    // Compare the order of common options
    if (commonOptions != mdOrderFiltered && !commonOptions.empty() && !mdOrderFiltered.empty()) {
      // Create readable string representations of the option lists
      std::string htmlOrderStr = buildCommaSeparatedString(commonOptions);
      std::string mdOrderStr = buildCommaSeparatedString(mdOrderFiltered);

      std::string detailMsg = std::format(
        "Section '{}' (tab '{}') has different option order:\n"
        "  HTML order: [{}]\n"
        "  MD order:   [{}]",
        docSection,
        tabId,
        htmlOrderStr,
        mdOrderStr
      );
      orderInconsistencies.push_back(detailMsg);
    }
  }

  if (!orderInconsistencies.empty()) {
    std::string errorMsg = "Config option order inconsistencies:\n";
    for (const auto &inconsistency : orderInconsistencies) {
      errorMsg += std::format("  {}\n", inconsistency);
    }
    FAIL() << errorMsg;
  }
}

TEST_F(ConfigConsistencyTest, DummyConfigOptionsDoNotExist) {
  const auto cppOptions = extractConfigCppOptions();
  const auto htmlOptions = extractConfigHtmlOptions();
  const auto mdOptions = extractConfigMdOptions();
  const auto jsonOptions = extractEnJsonConfigOptions();

  // List of fake config options that should NOT exist in any files
  const std::vector<std::string> dummyOptions = {
    "dummy_config_option",
    "nonexistent_setting",
    "fake_config_parameter",
    "test_dummy_option",
    "invalid_config_key"
  };

  std::vector<std::string> unexpectedlyFound;

  // Check that none of the fake options exist in any of the config files
  for (const auto &dummyOption : dummyOptions) {
    if (cppOptions.contains(dummyOption)) {
      unexpectedlyFound.push_back(std::format("config.cpp contains dummy option: {}", dummyOption));
    }

    if (htmlOptions.contains(dummyOption)) {
      unexpectedlyFound.push_back(std::format("config_tabs.json contains dummy option: {}", dummyOption));
    }

    if (mdOptions.contains(dummyOption)) {
      unexpectedlyFound.push_back(std::format("configuration.md contains dummy option: {}", dummyOption));
    }

    if (jsonOptions.contains(dummyOption)) {
      unexpectedlyFound.push_back(std::format("en.json contains dummy option: {}", dummyOption));
    }
  }

  // This test should pass (i.e., no fake options should be found)
  // If any fake options are found, it indicates a problem with the test data
  if (!unexpectedlyFound.empty()) {
    std::string errorMsg = "Dummy config options unexpectedly found in files:\n";
    for (const auto &found : unexpectedlyFound) {
      errorMsg += std::format("  {}\n", found);
    }
    FAIL() << errorMsg;
  }
}

TEST_F(ConfigConsistencyTest, TestFrameworkDetectsMissingOptions) {
  const auto cppOptions = extractConfigCppOptions();
  const auto htmlOptions = extractConfigHtmlOptions();
  const auto mdOptions = extractConfigMdOptions();
  const auto jsonOptions = extractEnJsonConfigOptions();

  // Add a fake option to the cpp options to simulate a missing option scenario
  std::set<std::string, std::less<>> modifiedCppOptions = cppOptions;
  const std::string testDummyOption = "test_framework_validation_option";
  modifiedCppOptions.insert(testDummyOption);

  // Options that are internal/special and shouldn't be in UI/docs
  std::set<std::string, std::less<>> internalOptions = {
    "flags"  // Internal config flags, not user-configurable
  };

  std::vector<std::string> missingFromFiles;

  // Check that the fake option is detected as missing from other files
  for (const auto &option : modifiedCppOptions) {
    if (internalOptions.contains(option)) {
      continue;  // Skip internal options
    }

    if (!htmlOptions.contains(option)) {
      missingFromFiles.push_back(std::format("config_tabs.json missing: {}", option));
    }

    if (!mdOptions.contains(option)) {
      missingFromFiles.push_back(std::format("configuration.md missing: {}", option));
    }

    if (!jsonOptions.contains(option)) {
      missingFromFiles.push_back(std::format("en.json missing: {}", option));
    }
  }

  // Verify that the test framework detected the missing fake option
  bool foundMissingDummyInHtml = false;
  bool foundMissingDummyInMd = false;
  bool foundMissingDummyInJson = false;

  checkTestDummyDetection(missingFromFiles, testDummyOption, foundMissingDummyInHtml, foundMissingDummyInMd, foundMissingDummyInJson);

  // The test framework should have detected the fake option as missing from all files
  EXPECT_TRUE(foundMissingDummyInHtml) << "Test framework failed to detect missing option in config_tabs.json";
  EXPECT_TRUE(foundMissingDummyInMd) << "Test framework failed to detect missing option in configuration.md";
  EXPECT_TRUE(foundMissingDummyInJson) << "Test framework failed to detect missing option in en.json";

  // Verify we have at least 3 missing entries (one for each file type)
  EXPECT_GE(missingFromFiles.size(), 3) << "Test framework should detect missing dummy option in all three file types";
}
