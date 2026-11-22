# Contributing
Read our contribution guide in our organization level
[docs](https://docs.lizardbyte.dev/latest/developers/contributing.html).

## Recommended Tools

| Tool                                                                                                                                                                           | Description                                                             |
|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------|
| <a href="https://www.jetbrains.com/clion/"><img src="https://resources.jetbrains.com/storage/products/company/brand/logos/CLion_icon.svg" width="30" height="30"></a><br>CLion | Recommended IDE for C and C++ development. Free for non-commercial use. |

## Project Patterns

### Web UI
* The Web UI uses [Vite](https://vitejs.dev) as its build system.
* The Web UI is a [Vue Router](https://router.vuejs.org) single-page application. Vite builds one `index.html` entry
  document, and the route implementations remain split into Vue components in `./src_assets/common/assets/web`.
* Browser routes use history mode. Sunshine serves the same `index.html` entry document for every Web UI route,
  while API and static asset routes continue to be handled independently by the configuration server.
* The Style System is provided by [Bootstrap](https://getbootstrap.com).
* Icons are provided by [Lucide](https://lucide.dev) and [Simple Icons](https://simpleicons.org).
* The JS framework used by the more interactive pages is [Vue.js](https://vuejs.org).

#### Building

@tabs{
  @tab{CMake |:| ```bash
    cmake -B build -G Ninja -S . --target web-ui
    ninja -C build web-ui
    ```}
  @tab{Manual |:| ```bash
    npm run dev
    ```}
}

#### Testing

Run the Web UI unit tests with:

```bash
npm test
```

### Localization
Sunshine and related LizardByte projects are being localized into various languages.
The default language is `en` (English).

![](https://app.lizardbyte.dev/dashboard/crowdin/LizardByte_graph.svg)

@admonition{Community |:| We are looking for language coordinators to help approve translations.
The goal is to have the bars above filled with green!
If you are interested, please reach out to us on our Discord server.}

#### CrowdIn
The translations occur on [CrowdIn][crowdin-url].
Anyone is free to contribute to the localization there.

##### Translation Basics
* The brand names *LizardByte* and *Sunshine* should never be translated.
* Other brand names should never be translated. Examples include *AMD*, *Intel*, and *NVIDIA*.

##### CrowdIn Integration
How does it work?

When a change is made to Sunshine source code, a workflow generates new translation templates
that get pushed to CrowdIn automatically.

When translations are updated on CrowdIn, a push gets made to the *l10n_master* branch and a PR is made against the
*master* branch. Once the PR is merged, all updated translations are part of the project and will be included in the
next release.

#### Extraction

##### Web UI
Sunshine uses [Vue I18n](https://vue-i18n.intlify.dev) for localizing the UI.
The following is a simple example of how to use it.

* Add the string to the `./src_assets/common/assets/web/public/assets/locale/en.json` file, in English.
  ```json
  {
   "index": {
     "welcome": "Hello, Sunshine!"
   }
  }
  ```

  > [!NOTE]
  > The JSON keys should be sorted alphabetically. You can use [jsonabc](https://novicelab.org/jsonabc)
  > to sort the keys.

  > [!IMPORTANT]
  > Due to the integration with Crowdin, it is important to only add strings to the *en.json* file,
  > and to not modify any other language files. After the PR is merged, the translations can take place
  > on [CrowdIn][crowdin-url]. Once the translations are complete, a PR will be made
  > to merge the translations into Sunshine.

* Use the string in the Vue component.
  ```html
  <template>
    <div>
      <p>{{ $t('index.welcome') }}</p>
    </div>
  </template>
  ```

  > [!TIP]
  > More formatting examples can be found in the
  > [Vue I18n guide](https://kazupon.github.io/vue-i18n/guide/formatting.html).

##### C++

There should be minimal cases where strings need to be extracted from C++ source code; however it may be necessary in
some situations. For example the system tray icon could be localized as it is user interfacing.

* Wrap the string to be extracted in a function as shown.
  ```cpp
  #include <boost/locale.hpp>
  #include <string>

  std::string msg = boost::locale::translate("Hello world!");
  ```

> [!TIP]
> More examples can be found in the documentation for
> [boost locale](https://www.boost.org/doc/libs/1_70_0/libs/locale/doc/html/messages_formatting.html).

> [!WARNING]
> The below is for information only. Contributors should never include manually updated template files, or
> manually compiled language files in Pull Requests.

Strings are automatically extracted from the code to the `locale/sunshine.po` template file. The generated file is
used by CrowdIn to generate language specific template files. The file is generated using the
`.github/workflows/localize.yml` workflow and is run on any push event into the `master` branch. Jobs are only run if
any of the following paths are modified.

```yaml
- 'src/**'
```

When testing locally, it may be desirable to manually extract, initialize, update, and compile strings. Python and
uv are required for this, along with the Python dependencies in the Sunshine `pyproject.toml`. From the repository
root, install these with the following command.

```bash
uv sync --locked
```

Additionally, [xgettext](https://www.gnu.org/software/gettext) must be installed.

* Extract, initialize, and update
  ```bash
  uv run --locked --no-sync lb-localize --root-dir . --extract --init --update
  ```

* Compile
  ```bash
  uv run --locked --no-sync lb-localize --root-dir . --compile
  ```

> [!IMPORTANT]
> Due to the integration with CrowdIn, it is important to not include any extracted or compiled files in
> Pull Requests. The files are automatically generated and updated by the workflow. Once the PR is merged, the
> translations can take place on [CrowdIn][crowdin-url]. Once the translations are
> complete, a PR will be made to merge the translations into Sunshine.

### Testing

#### Clang Format
Source code is tested against the `.clang-format` file for linting errors.

From the repository root, apply clang-format locally with the installed lizardbyte-common script. This will modify
files in place.

```bash
uv sync --locked
uv run --locked --no-sync lb-update-clang-format
```

#### Unit Testing
Sunshine uses [Google Test](https://github.com/google/googletest) for unit testing. Google Test is included in the
repo as a submodule. The test sources are located in the `./tests` directory.

The tests need to be compiled into an executable, and then run. The tests are built using the normal build process, but
can be disabled by setting the `BUILD_TESTS` CMake option to `OFF`.

To run the tests, execute the following command.

```bash
./build/tests/test_sunshine
```

To see all available options, run the tests with the `--help` flag.

```bash
./build/tests/test_sunshine --help
```

> [!TIP]
> See the googletest [FAQ](https://google.github.io/googletest/faq.html) for more information on how to use Google Test.

We use [gcovr](https://www.gcovr.com) to generate code coverage reports,
and [Codecov](https://about.codecov.io) to analyze the reports for all PRs and commits.

Codecov will fail a PR if the total coverage is reduced too much, or if not enough of the diff is covered by tests.
In some cases, the code cannot be covered when running the tests inside of GitHub runners. For example, any test that
needs access to the GPU will not be able to run. In these cases, the coverage can be omitted by adding comments to the
code. See the [gcovr documentation](https://gcovr.com/en/stable/guide/exclusion-markers.html#exclusion-markers) for
more information.

Even if your changes cannot be covered in the CI, we still encourage you to write the tests for them. This will allow
maintainers to run the tests locally.

[crowdin-url]: https://translate.lizardbyte.dev
