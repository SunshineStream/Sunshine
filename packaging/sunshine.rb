require "language/node"

class Sunshine < Formula
  include Language::Python::Virtualenv

  CUDA_VERSION = "13.1".freeze
  CUDA_FORMULA = "cuda@#{CUDA_VERSION}".freeze
  COVERAGE_BUILDPATH_FILE = "coverage-buildpath.txt".freeze
  COVERAGE_LCOV = "coverage.lcov".freeze
  COVERAGE_PROFDATA = "coverage.profdata".freeze
  COVERAGE_XML = "coverage.xml".freeze
  GCOV_PREFIX_STRIP_FILE = "gcov-prefix-strip.txt".freeze
  GCC_VERSION = "14".freeze
  GCC_FORMULA = "gcc@#{GCC_VERSION}".freeze
  LLVM_PROFILE_FILE_ENV = "LLVM_PROFILE_FILE".freeze
  TEST_BINARY = "test_sunshine".freeze
  TEST_RESULTS_XML = "tests/test_results.xml".freeze
  IS_UPSTREAM_REPO = ENV.fetch("GITHUB_REPOSITORY", "") == "SunshineStream/Sunshine"

  desc "@PROJECT_DESCRIPTION@"
  homepage "@PROJECT_HOMEPAGE_URL@"
  url "@GITHUB_CLONE_URL@",
    tag:      "@GITHUB_TAG@",
    revision: "@GITHUB_COMMIT@"
  version "@BUILD_VERSION@"
  license all_of: ["GPL-3.0-only"]
  head "@GITHUB_CLONE_URL@", branch: "@GITHUB_DEFAULT_BRANCH@"

  # https://docs.brew.sh/Brew-Livecheck#githublatest-strategy-block
  livecheck do
    url :stable
    regex(/^v?(\d+\.\d+\.\d+)$/i)
    strategy :github_latest do |json, regex|
      match = json["tag_name"]&.match(regex)
      next if match.blank?

      match[1]
    end
  end

  bottle do
    root_url "https://ghcr.io/v2/lizardbyte/homebrew"
  end

  option "with-docs", "Enable docs build"
  option "with-static-boost", "Enable static link of Boost libraries"
  option "without-static-boost", "Disable static link of Boost libraries" # default option

  # Keep coverage instrumentation in the test binary after test-bot rebuilds the formula from its bottle.
  skip_clean "bin/#{TEST_BINARY}" if IS_UPSTREAM_REPO

  depends_on "cmake" => :build
  depends_on "doxygen" => :build if build.with? "docs"
  depends_on "graphviz" => :build if build.with? "docs"
  depends_on "node" => :build
  depends_on "pkgconf" => :build
  depends_on "boost"
  depends_on "curl"
  depends_on "icu4c@79"
  depends_on "miniupnpc"
  depends_on "openssl@4"
  depends_on "opus"
  depends_on "qtbase"
  depends_on "qtsvg"

  on_sonoma do
    depends_on xcode: ["16.2", :build] # required for jthreads on macos-14
  end

  on_linux do
    depends_on GCC_FORMULA => [:build, :test]
    depends_on "gcovr" => [:build, :test]
    depends_on "lizardbyte/homebrew/#{CUDA_FORMULA}" => :build
    depends_on "python3" => :build
    depends_on "imagemagick" => :test
    depends_on "at-spi2-core"
    depends_on "avahi"
    depends_on "cairo"
    depends_on "gdk-pixbuf"
    depends_on "glib"
    depends_on "gnu-which"
    depends_on "harfbuzz"
    depends_on "libcap"
    depends_on "libdrm"
    depends_on "libice"
    depends_on "libsm"
    depends_on "libva"
    depends_on "libx11"
    depends_on "libxcb"
    depends_on "libxcursor"
    depends_on "libxext"
    depends_on "libxfixes"
    depends_on "libxi"
    depends_on "libxinerama"
    depends_on "libxrandr"
    depends_on "libxtst"
    depends_on "mesa"
    depends_on "numactl"
    depends_on "pango"
    depends_on "pipewire"
    depends_on "pulseaudio"
    depends_on "shaderc"
    depends_on "systemd"
    depends_on "vulkan-loader"
    depends_on "wayland"

    # Jinja2 is required at build time by the glad OpenGL/EGL loader generator (Linux only).
    # Declared as resources per https://docs.brew.sh/Formula-Cookbook#python-dependencies
    resource "markupsafe" do
      url "https://files.pythonhosted.org/packages/7e/99/7690b6d4034fffd95959cbe0c02de8deb3098cc577c67bb6a24fe5d7caa7/markupsafe-3.0.3.tar.gz"
      sha256 "722695808f4b6457b320fdc131280796bdceb04ab50fe1795cd540799ebe1698"
    end

    resource "jinja2" do
      url "https://files.pythonhosted.org/packages/df/bf/f7da0350254c0ed7c72f3e33cef02e048281fec7ecec5f032d4aac52226b/jinja2-3.1.6.tar.gz"
      sha256 "0137fb05990d35f1275a587e9aee6d56da821fc83491a0fb838183be43f66d6d"
    end
  end

  conflicts_with "sunshine-beta", because: "sunshine and sunshine-beta cannot be installed at the same time"

  fails_with :clang do
    build 1400
    cause "Requires C++23 support"
  end

  fails_with :gcc do
    version "12" # fails with GCC 12.x and earlier
    cause "Requires C++23 support"
  end

  fails_with :gcc do
    version "13"
    cause "Array out of bounds error when compiling glad sources"
  end

  ## @brief Configure build metadata, the Linux compiler, and the glad Python environment.
  def setup_build_environment
    ENV["BRANCH"] = "@GITHUB_BRANCH@"
    ENV["BUILD_VERSION"] = "@BUILD_VERSION@"
    ENV["COMMIT"] = "@GITHUB_COMMIT@"

    if OS.linux?
      # Use GCC because gcov from llvm cannot handle our paths
      gcc_path = Formula[GCC_FORMULA]
      ENV["CC"] = "#{gcc_path.opt_bin}/gcc-#{GCC_VERSION}"
      ENV["CXX"] = "#{gcc_path.opt_bin}/g++-#{GCC_VERSION}"
    end

    return unless OS.linux?

    # Install jinja2 (required by the glad OpenGL/EGL loader generator) into a
    # temporary virtualenv. We pass its Python path to cmake via Python_EXECUTABLE
    # so glad uses the venv Python that has jinja2, and set GLAD_SKIP_PIP_INSTALL=ON
    # to prevent cmake from trying to install Python dependencies again.
    # Follows https://docs.brew.sh/Formula-Cookbook#python-dependencies
    venv = virtualenv_create(buildpath/"venv", "python3")
    venv.pip_install [resource("markupsafe"), resource("jinja2")]
    @glad_python = (buildpath/"venv/bin/python3").to_s
  end

  ## @brief Add CUDA compiler arguments for Linux builds.
  ## @param args CMake arguments to extend.
  def add_cuda_args(args)
    return unless OS.linux?

    cuda_path = Formula["lizardbyte/homebrew/#{CUDA_FORMULA}"]
    nvcc_path = "#{cuda_path.opt_libexec}/homebrew/bin/nvcc"
    gcc_path = Formula[GCC_FORMULA]

    args << "-DSUNSHINE_ENABLE_CUDA=ON"
    args << "-DCMAKE_CUDA_COMPILER:PATH=#{nvcc_path}"
    args << "-DCMAKE_CUDA_TOOLKIT_ROOT_DIR:PATH=#{cuda_path.opt_libexec}"
    args << "-DCMAKE_CUDA_HOST_COMPILER=#{gcc_path.opt_bin}/gcc-#{GCC_VERSION}"
    ohai "CUDA enabled with nvcc at: #{nvcc_path}"
  end

  def release_homebrew_testpath
    testpath_value = ENV.fetch("HOMEBREW_TEST_ARTIFACTS_DIR", "")
    return Pathname.new(testpath_value) unless testpath_value.empty?

    return if ENV.fetch("HOMEBREW_TEST_BOT", "") != "1"

    temp_path = ENV.fetch("HOMEBREW_TEMP", "")
    return if temp_path.empty?

    Pathname.new(temp_path)/name/"test"
  end

  def ensure_artifact_exists(path)
    odie "#{path} was not created" unless path.exist?
  end

  def with_llvm_profile_file(artifact_dir)
    original_profile_file = ENV.fetch(LLVM_PROFILE_FILE_ENV, nil)
    ENV[LLVM_PROFILE_FILE_ENV] = "#{artifact_dir}/sunshine-%p.profraw"
    yield
  ensure
    if original_profile_file
      ENV[LLVM_PROFILE_FILE_ENV] = original_profile_file
    else
      ENV.delete(LLVM_PROFILE_FILE_ENV)
    end
  end

  def coverage_report_path(artifact_dir)
    artifact_dir/(OS.mac? ? COVERAGE_LCOV : COVERAGE_XML)
  end

  ## @brief Export and validate source coverage with LLVM on macOS or gcovr on Linux.
  ## @param artifact_dir Directory containing test profiles and receiving the report.
  ## @param coverage_buildpath Source build path used to map coverage records.
  def generate_coverage_report(artifact_dir, coverage_buildpath)
    return if coverage_buildpath.to_s.empty?

    coverage_report = coverage_report_path(artifact_dir)

    if OS.mac?
      profile_files = Dir["#{artifact_dir}/*.profraw"]
      odie "No LLVM profile data was created" if profile_files.empty?

      profile_data = artifact_dir/COVERAGE_PROFDATA
      llvm_profdata = Utils.safe_popen_read("xcrun", "--find", "llvm-profdata").strip
      system llvm_profdata, "merge", "--sparse", "-o", profile_data.to_s, *profile_files
      lcov = Utils.safe_popen_read(
        Utils.safe_popen_read("xcrun", "--find", "llvm-cov").strip,
        "export",
        "-format=lcov",
        "-instr-profile=#{profile_data}",
        (bin/TEST_BINARY).to_s,
      )
      coverage_report.write lcov_for_source_files(lcov, coverage_buildpath)
      has_lines = coverage_report.read.lines.any? { |line| line.start_with?("DA:") }
      odie "#{coverage_report} does not contain any source lines" unless has_lines
    else
      cd "#{coverage_buildpath}/build" do
        gcc_path = Formula[GCC_FORMULA]
        system "gcovr", "tests/CMakeFiles/#{TEST_BINARY}.dir/__/src",
          "-r", "../src",
          "--gcov-executable", "#{gcc_path.opt_bin}/gcov-#{GCC_VERSION}",
          "--exclude-noncode-lines",
          "--exclude-throw-branches",
          "--exclude-unreachable-branches",
          "--xml-pretty",
          "-o=#{coverage_report}"
      end

      lines_valid = coverage_report.read[/lines-valid="(\d+)"/, 1].to_i
      odie "#{coverage_report} does not contain any source lines" if lines_valid.zero?
    end

    ensure_artifact_exists coverage_report
  end

  def relative_lcov_record(record, source_prefixes)
    lines = record.lines
    source_index = lines.index { |line| line.start_with?("SF:") }
    return unless source_index

    source_path = Pathname.new(lines[source_index].delete_prefix("SF:").strip).cleanpath.to_s
    # Homebrew remaps the formula build path to ".". LLVM may then resolve that
    # relative path from CMake's compilation directory at "build/tests".
    relative_source_path = if source_path.start_with?("build/tests/src/")
      source_path.delete_prefix("build/tests/")
    elsif source_path.start_with?("src/")
      source_path
    else
      source_prefix = source_prefixes.find { |prefix| source_path.start_with?(prefix) }
      "src/#{source_path.delete_prefix(source_prefix)}" if source_prefix
    end
    return unless relative_source_path

    lines[source_index] = "SF:#{relative_source_path}\n"
    "#{lines.join}end_of_record\n"
  end

  ## @brief Retain LCOV records for source files and normalize their paths.
  ## @param lcov LCOV report contents.
  ## @param coverage_buildpath Source build path used to identify absolute records.
  ## @return LCOV records with paths relative to the repository root.
  def lcov_for_source_files(lcov, coverage_buildpath)
    coverage_buildpath = Pathname.new(coverage_buildpath.to_s)
    paths = [coverage_buildpath.to_s]
    paths << coverage_buildpath.realpath.to_s if coverage_buildpath.exist?
    source_prefixes = paths.uniq.map { |path| "#{path}/src/" }
    records = lcov.split("end_of_record\n")
    records.filter_map { |record| relative_lcov_record(record, source_prefixes) }.join
  end

  def collect_test_artifacts
    artifact_dir = release_homebrew_testpath
    return unless IS_UPSTREAM_REPO
    return unless artifact_dir

    coverage_buildpath = OS.mac? ? buildpath.realpath : HOMEBREW_TEMP/"coverage"
    mkdir_p artifact_dir
    (artifact_dir/COVERAGE_BUILDPATH_FILE).write coverage_buildpath.to_s
  end

  ## @brief Configure the bundled tests and platform coverage tooling.
  ## @param args CMake arguments to extend.
  def add_test_args(args)
    unless IS_UPSTREAM_REPO
      args << "-DBUILD_TESTS=OFF"
      ohai "Building tests: disabled"
      return
    end

    args << "-DBUILD_TESTS=ON"
    test_runtime = opt_libexec/"tests"
    args << "-DSUNSHINE_TEST_SOURCE_DIR=#{test_runtime}"
    args << "-DSUNSHINE_TEST_RUNTIME_DIR=#{test_runtime}"
    coverage_arg = if OS.mac?
      "-DSUNSHINE_LLVM_COVERAGE=ON"
    else
      # gcovr writes intermediate files next to the mapped sources, so use Homebrew's writable temp tree.
      coverage_runtime = HOMEBREW_TEMP/"coverage"
      "-DSUNSHINE_TEST_GCOV_ROOT=#{coverage_runtime}"
    end
    args << coverage_arg
    ohai "Building tests: enabled"
  end

  ## @brief Install the test binary and fixtures used by the upstream Homebrew jobs.
  def install_test_fixtures
    return unless IS_UPSTREAM_REPO

    bin.install "build/tests/#{TEST_BINARY}"

    test_runtime = libexec/"tests"
    %w[docs src src_assets test_assets].each do |directory|
      test_runtime.install "build/tests/#{directory}"
    end
    boost_test_fixtures = %w[
      cmake/compile_definitions/macos.cmake
      cmake/dependencies/Boost_Sunshine.cmake
      cmake/macros/common.cmake
      cmake/prep/options.cmake
      package-lock.cmake
      packaging/linux/Arch/PKGBUILD
      packaging/linux/copr/Sunshine.spec
      packaging/linux/flatpak/modules/boost.json
      packaging/linux/flatpak/scripts/additional-install.sh
      scripts/linux_build.sh
      tests/scripts/test_additional_install.sh
    ]
    if OS.mac?
      boost_test_fixtures += %w[
        .github/workflows/ci-macos.yml
        scripts/macos_build.sh
      ]
    end
    boost_test_fixtures.each do |fixture|
      (test_runtime/Pathname.new(fixture).dirname).install "build/tests/#{fixture}"
    end
    test_runtime.install "sunshine.png"
    (test_runtime/"tests/unit").install "tests/unit/test_video.cpp"

    if OS.linux?
      coverage_runtime = test_runtime/"coverage"
      coverage_runtime.install "src"
      (test_runtime/GCOV_PREFIX_STRIP_FILE).write "#{buildpath.each_filename.count}\n"
      coverage_notes = buildpath.glob("build/**/*.gcno")
      odie "No gcov notes were created" if coverage_notes.empty?

      coverage_notes.each do |coverage_note|
        relative_note = coverage_note.relative_path_from(buildpath)
        (coverage_runtime/relative_note.dirname).install coverage_note
      end
    end
  end

  ## @brief Build and install Sunshine with platform dependencies and optional CI test fixtures.
  def install
    setup_build_environment
    standard_args = std_cmake_args

    args = %W[
      -DBUILD_WERROR=ON
      -DCMAKE_INSTALL_PREFIX=#{prefix}
      -DGLAD_SKIP_PIP_INSTALL=ON
      -DHOMEBREW_ALLOW_FETCHCONTENT=ON
      -DOPENSSL_ROOT_DIR=#{formula_opt_prefix("openssl@4")}
      -DSUNSHINE_ASSETS_DIR=sunshine/assets
      -DSUNSHINE_BUILD_HOMEBREW=ON
      -DSUNSHINE_PUBLISHER_NAME='LizardByte'
      -DSUNSHINE_PUBLISHER_WEBSITE='https://app.lizardbyte.dev'
      -DSUNSHINE_PUBLISHER_ISSUE_URL='https://app.lizardbyte.dev/support'
    ]
    if OS.linux?
      # GCC treats Homebrew's lib directory as implicit, so CMake omits its automatic rpath.
      # Keep curl's indirect dependencies ahead of incompatible system libraries when linking and running.
      args << "-DCMAKE_BUILD_RPATH=#{HOMEBREW_PREFIX}/lib"
      args << "-DCMAKE_INSTALL_RPATH=#{HOMEBREW_PREFIX}/lib"
    end
    args << "-DSUNSHINE_EXECUTABLE_PATH=#{opt_bin}/sunshine" if OS.linux?
    # Point cmake at the venv Python that has jinja2 installed (set up in setup_build_environment)
    args << "-DPython_EXECUTABLE=#{@glad_python}" if @glad_python

    add_test_args(args)

    if build.with? "docs"
      ohai "Building docs: enabled"
      args << "-DBUILD_DOCS=ON"
    else
      ohai "Building docs: disabled"
      args << "-DBUILD_DOCS=OFF"
    end

    if build.without? "static-boost"
      args << "-DBOOST_USE_STATIC=OFF"
      ohai "Disabled statically linking Boost libraries"
    else
      args << "-DBOOST_USE_STATIC=ON"
      ohai "Enabled statically linking Boost libraries"

      unless formula_any_version_installed?("icu4c")
        odie <<~EOS
          icu4c must be installed to link against static Boost libraries,
          either install icu4c or use brew install sunshine --with-static-boost instead
        EOS
      end
      ENV.append "CXXFLAGS", "-I#{formula_opt_include("icu4c")}"
      icu4c_lib_path = formula_opt_lib("icu4c").to_s
      ENV.append "LDFLAGS", "-L#{icu4c_lib_path}"
      ENV["LIBRARY_PATH"] = icu4c_lib_path
      ohai "Linking against ICU libraries at: #{icu4c_lib_path}"
    end

    add_cuda_args(args)

    system "cmake", "-S", ".", "-B", "build", "-G", "Unix Makefiles",
            *standard_args,
            *args

    system "make", "-C", "build"
    system "make", "-C", "build", "install"

    install_test_fixtures

    # codesign the binary on intel macs
    system "codesign", "-s", "-", "--force", "--deep", bin/"sunshine" if OS.mac? && Hardware::CPU.intel?

    bin.install "src_assets/linux/misc/postinst" if OS.linux?

    collect_test_artifacts
  end

  service do
    run [opt_bin/"sunshine", "~/.config/sunshine/sunshine.conf"] if OS.mac?
    name linux: "app-@PROJECT_FQDN@" if OS.linux?
  end

  post_install_steps do
    on_linux do
      warn <<~EOS
        ATTENTION: To complete installation, you must run the following command:
        `sudo {{bin}}/postinst`
      EOS
    end

    on_macos do
      warn <<~EOS
        Gamepads are not currently supported on macOS.
      EOS
    end
  end

  def caveats
    <<~EOS
      Thanks for installing @PROJECT_NAME@!

      To get started, review the documentation at:
        https://docs.lizardbyte.dev/projects/sunshine
    EOS
  end

  ## @brief Verify the installed binary, CI fixtures, test results, and normalized source coverage.
  test do
    # test that the binary runs at all
    system bin/"sunshine", "--version"

    if OS.linux?
      assert_path_exists lib/"udev/rules.d/60-sunshine.rules"
      assert_path_exists lib/"modules-load.d/60-sunshine.conf"
    end

    if IS_UPSTREAM_REPO
      artifact_dir = release_homebrew_testpath
      if artifact_dir
        coverage_buildpath = artifact_dir/COVERAGE_BUILDPATH_FILE
        test_runtime = opt_libexec/"tests"
        assert_path_exists coverage_buildpath
        assert_path_exists bin/TEST_BINARY
        assert_path_exists test_runtime/"docs/getting_started.md"
        assert_path_exists test_runtime/"cmake/dependencies/Boost_Sunshine.cmake"
        assert_path_exists test_runtime/"scripts/macos_build.sh" if OS.mac?
        assert_path_exists test_runtime/"src/config.cpp"
        assert_path_exists test_runtime/"src_assets/common/assets/web/public/assets/locale/en.json"
        assert_path_exists test_runtime/"test_assets/web/images/logo-sunshine.svg"
        assert_path_exists test_runtime/"tests/unit/test_video.cpp"
        if OS.linux?
          assert_path_exists test_runtime/"tests/scripts/test_additional_install.sh"
          assert_path_exists test_runtime/"packaging/linux/flatpak/scripts/additional-install.sh"
          assert_path_exists test_runtime/"src_assets/linux/misc/60-sunshine.conf"
          assert_path_exists test_runtime/"src_assets/linux/misc/60-sunshine.rules"
          assert_path_exists test_runtime/"coverage/src/config.cpp"
          assert_path_exists test_runtime/GCOV_PREFIX_STRIP_FILE
          source_notes = test_runtime.glob("coverage/build/tests/CMakeFiles/#{TEST_BINARY}.dir/__/src/**/*.gcno")
          assert source_notes.any?, "No installed source gcov notes were found"
        end

        if (artifact_dir/TEST_RESULTS_XML).exist?
          assert_path_exists artifact_dir/TEST_RESULTS_XML
          generate_coverage_report artifact_dir, coverage_buildpath.read.strip
          assert_path_exists coverage_report_path(artifact_dir)
        end
      elsif ENV.fetch("HOMEBREW_BOTTLE_BUILD", "false") != "true"
        artifact_dir = testpath
        mkdir_p artifact_dir/"tests"
        test_results = artifact_dir/TEST_RESULTS_XML

        if OS.mac?
          with_llvm_profile_file(artifact_dir) do
            system bin/TEST_BINARY, "--gtest_color=yes", "--gtest_output=xml:#{test_results}"
          end
        else
          system bin/TEST_BINARY, "--gtest_color=yes", "--gtest_output=xml:#{test_results}"
        end

        ensure_artifact_exists test_results
        generate_coverage_report testpath, ENV.fetch("HOMEBREW_BUILDPATH", "")
      end

      lcov = <<~LCOV
        SF:./src/remapped.cpp
        DA:1,1
        end_of_record
        SF:build/tests/src/remapped_from_compile_dir.cpp
        DA:1,1
        end_of_record
        SF:src/relative.cpp
        DA:1,1
        end_of_record
        SF:#{testpath}/src/absolute.cpp
        DA:1,1
        end_of_record
        SF:./tests/excluded.cpp
        DA:1,1
        end_of_record
        SF:build/tests/tests/excluded_from_compile_dir.cpp
        DA:1,1
        end_of_record
      LCOV
      expected_lcov = <<~LCOV
        SF:src/remapped.cpp
        DA:1,1
        end_of_record
        SF:src/remapped_from_compile_dir.cpp
        DA:1,1
        end_of_record
        SF:src/relative.cpp
        DA:1,1
        end_of_record
        SF:src/absolute.cpp
        DA:1,1
        end_of_record
      LCOV
      assert_equal expected_lcov, lcov_for_source_files(lcov, testpath)

      missing_buildpath = testpath/"missing-buildpath"
      missing_path_lcov = <<~LCOV
        SF:#{missing_buildpath}/src/missing.cpp
        DA:1,1
        end_of_record
      LCOV
      expected_missing_path_lcov = <<~LCOV
        SF:src/missing.cpp
        DA:1,1
        end_of_record
      LCOV
      assert_equal expected_missing_path_lcov, lcov_for_source_files(missing_path_lcov, missing_buildpath)
    end
  end
end
