/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
#include <src/server/httpcommon.h>

#include <tests/conftest.cpp>

class UrlEscapeTest: public ::testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(http::url_escape(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  ::testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")));

class UrlGetHostTest: public ::testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(http::url_get_host(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  ::testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")));

class DownloadFileTest: public ::testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(DownloadFileTest, Run) {
  auto [url, filename] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/";
  std::basic_string path = test_dir + filename;
  ASSERT_TRUE(http::download_file(url, path));
}

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  ::testing::Values(
    std::make_tuple("https://httpbin.org/base64/aGVsbG8h", "hello.txt"),
    std::make_tuple("https://httpbin.org/redirect-to?url=/base64/aGVsbG8h", "hello-redirect.txt")));
