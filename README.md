# Hermes JS VM

**Welcome to Hermes!**

Hermes is a JavaScript virtual machine that accepts [Flow](https://flowtype.org)
type annotations, features host-side static optimizations and compact bytecode.

## Getting Started for React Native developers

First make sure that the Android SDK and NDK are installed, and that the
environment variables `ANDROID_SDK` and `ANDROID_NDK` are set. Here's an
example:

```
$ echo "$ANDROID_SDK"
/opt/android_sdk

$ echo "$ANDROID_NDK"
/opt/android_ndk/r15c
```

Create a base directory to work in, e.g. `~/workspace`, and `cd` into it. Then
follow the steps below (or copy-paste them all):

```
# 1. Use this directory as the workspace
export HERMES_WS_DIR="$PWD"

( set -e

# 2. Clone Hermes here
#    (FB internal:  ln -s ~/fbsource/xplat/hermes hermes  )
git clone git@github.com:facebook/hermes.git

# 3. Clone and build LLVM. This may take a while.
hermes/utils/build_llvm.sh
# for cli release build:
DISTRIBUTE=1 hermes/utils/build_llvm.sh

# 4. Cross-compile LLVM dependencies for all Android ABIs
hermes/utils/crosscompile_llvm.sh

# 5. Compile libhermes for Android
( cd hermes/android && gradle githubRelease)

# 6. Configure the build for the hermes compiler and repl for the host platform
./hermes/utils/configure.sh
# for cli release build
DISTRIBUTE=1 ./hermes/utils/configure.sh

# 7. Build the compiler and repl for cli release
( cd build_release && ninja github-cli-release )

# 8. Build the hermesvm npm
cp build_android/distributions/hermes-runtime-android-v*.tar.gz hermes/npm
cp build_release/github/hermes-cli-*-v*.tar.gz hermes/npm
(cd hermes/npm && yarn && yarn run prepack-dev && yarn link)
# for release build (this will not work with the private repo, unless
# you add a personal access token <https://github.com/settings/tokens>
# to the URLs in fetch.js, like ?access_token=...)
# Create a github release
# Update the release_version number in hermes/CMakeLists.txt
# Add files to the github release.  This will require building on more than one machine:
#   build_android/distributions/hermes-runtime-android-v<version>.tar.gz
#   build_release/github/hermes-cli-darwin-v<version>.tar.gz
#   build_release/github/hermes-cli-linux-v<version>.tar.gz
# Update the release version and file digests in hermes/npm/package.json
# (cd hermes/npm && yarn pack)
# TODO: have travis automate this

# 9. Clone react-native-hermes here
git clone git@github.com:facebookexperimental/react-native-hermes.git

# 10. Replace the RN template app version number (1000.0.0) with the path
#    to your react-native-hermes directory.
printf '%s\n' "%s|1000.0.0|file://${HERMES_WS_DIR:?}/react-native-hermes" wq |
    ed react-native-hermes/template/package.json

# 11. Fetch react-native-hermes' dependencies
( cd react-native-hermes && yarn install )

# 12. Create a React Native demo project from the react-native-hermes template
npx @react-native-community/cli@2.0.0-alpha.16 init AwesomeProject --template "file://${HERMES_WS_DIR:?}/react-native-hermes"
( cd AwesomeProject/node_modules/react-native && yarn link hermesvm )

# 13. Build and run the demo project
( cd AwesomeProject && react-native start ) &
( cd AwesomeProject && npx @react-native-community/cli@2.0.0-alpha.16 run-android )

)

# 14. If you want to build RNTester:
( cd react-native-github && yarn link hermesvm )
( cd react-native-github && ./gradlew RNTester:android:app:installDebug )
```

To set up an existing project to use Hermes:

1. Clone React Native from `git@github.com:facebookexperimental/react-native-hermes.git`
2. Set up the project to use [React Native from source](https://facebook.github.io/react-native/docs/building-from-source),
   from the `react-native-hermes` directory.
3. Override `ReactNativeHost.getJavaScriptExecutorFactory()` to return a
   `HermesExecutorFactory`. See the example in the
   [demo app template](https://github.com/facebookexperimental/react-native-hermes/blob/master/template/android/app/src/main/java/com/helloworld/MainApplication.java#L38)
4. Verify that you are using Hermes by checking that `typeof(HermesInternal)`
   is `"object"` (and not `"undefined"`).

## Debugging React Native apps running Hermes

1. Open [chrome://inspect/](chrome://inspect/) in Chrome
2. Make sure `localhost:8081` appears as a target in the "Configure..." menu
3. Start a debug build of your Hermes-enabled React Native app on an emulator

"Hermes React Native" should now appear as Remote Target, and you can hit
"inspect" to open a debugger.

## Getting Started for Hermes developers

### System Requirements

The project builds and runs on OS X and Linux. The project depends on the tools
that are required to build LLVM (a C++ compiler, CMake, Python, etc) as well as
node.js and babel.

The REPL uses 'libreadline' for editing, if it is installed.

### Getting Sources and building the compiler

Running the LLVM build script will clone LLVM and Clang (which are a dependency)
and setup the symlinks that LLVM needs to build clang. Next, the script will
build llvm and clang into the directory "llvm_build".

    ./hermes/utils/build_llvm.sh

Next, run the configure script. The script will generate a build directory with
Ninja project files. It is possible to configure and build Hermes with any CMake
generator, like GNU Makefiles and Xcode build. Peek into the configure script
if you prefer to use an alternative build system.

    ./hermes/utils/configure.sh

After running the build script, running 'ninja' from the build directory will
build the compiler driver.

## Testing and running Hermes

After compiling the project, the vm driver binary will be located in the `/bin`
directory under the name `./bin/hermes`.  Run `./bin/hermes --help` to learn
more about using the vm test driver.

To run the tests run the `check-hermes` target. If you are using the default
build system, ninja, then the command to run the tests is `ninja check-hermes`.

The default compilation mode is "Debug". This means that the compiler itself is
easy to debug because it has debug info, lots of assertions, and the
optimizations are disabled. If you wish to benchmark the compiler or release it
then you should compile the compiler in Release mode.

When configuring the project add the flag `-DCMAKE_BUILD_TYPE=Release`. Refer to
the LLVM build instructions for more details:

    http://llvm.org/docs/GettingStarted.html

## Debugging Hermes

One excellent tool for catching memory errors is Clang's Address Sanitizer
(ASan). To enable an ASan build, configure the project with the flag
"-DLLVM_USE_SANITIZER="Address". Another option is to add the following line to
the main hermes CMake file:

  set(LLVM_USE_SANITIZER "Address")

For more details about ASan:

  http://clang.llvm.org/docs/AddressSanitizer.html

## Supported targets

At the moment Mac and Linux and the only supported targets. The vm
should compile and run on Windows, but this configuration has not been tested.

## Contributing to Hermes

Contributions to Hermes are welcomed and encouraged!
