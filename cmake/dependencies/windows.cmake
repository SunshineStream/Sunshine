# windows specific dependencies

set(Boost_USE_STATIC_LIBS ON)  # cmake-lint: disable=C0103
find_package(Boost 1.71.0 COMPONENTS locale log filesystem program_options REQUIRED)