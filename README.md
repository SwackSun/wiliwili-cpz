# wiliwili-cpz for CardputerZero

Community port of [xfangfang's wiliwili](https://github.com/xfangfang/wiliwili) v1.6.0 for M5Stack CardputerZero running czlite. The port adds LCD and HDMI display handling, a VideoCore IV playback path, and CardputerZero keyboard shortcuts. Port changes date from 2026-11-18. See the [device guide](USAGE.md) for controls.

Public releases use Git tags in the `vX.Y.Z` format and IPK versions in the `X.Y.Z` format. The build records its internal revision as `vX.Y.Z.rcN` in [CMakeLists.txt](CMakeLists.txt).

## Build for local development

Use a Linux host with the Buildroot host dependencies, Git, curl, Python 3 and CMake 3.25 or newer. Set `CM0_BUILDROOT_OUTPUT` to the absolute path of a matching czlite Buildroot output with its host toolchain and `.config`. Install patchelf on the host if the SDK lacks it. Use a checkout path without spaces.

From the root of this checkout, fetch the pinned upstream revision and the recursive submodules used by the port. The upstream repository and each submodule carry their own notices and terms.

```sh
git clone --no-checkout https://github.com/xfangfang/wiliwili.git upstream &&
git -C upstream checkout --detach "$(cat UPSTREAM_REVISION)" &&
git -C upstream submodule update --init --recursive -- \
    library/borealis library/cpr library/OpenCC library/QR-Code-generator \
    library/pystring library/lunasvg
```

Prepare the pinned Buildroot source and build the application. Buildroot checks and downloads the configured target libraries, and the output is staged under `build/stage/`.

```sh
archive=$(awk '{ print $2 }' BUILDROOT.sha256)
revision=${archive#buildroot-}
revision=${revision%.tar.gz}
curl -fL -o "$archive" "https://codeload.github.com/buildroot/buildroot/tar.gz/$revision" &&
sha256sum --check BUILDROOT.sha256 &&
mkdir -p vendor/sources/buildroot vendor/downloads/buildroot &&
tar -xzf "$archive" --strip-components=1 -C vendor/sources/buildroot &&
./scripts/build.sh
```

## License and redistribution

The wiliwili application and CardputerZero board integration use GPLv3. The application patches and adapted entry point are based on xfangfang's v1.6.0 sources. The port modifies playback, display selection, keyboard input and account handling. [Application license](LICENSE)

The media patch applies to mpv 0.40.0. Its new frame header carries an LGPL-2.1-or-later notice. The patched public render header carries the mpv developers' ISC notice. The mpv source archive identifies the terms for the renderer and build files. [mpv copyright and license texts](LICENSES/mpv-0.40.0/)

Live chat uses libcurl's WSS client. The CardputerZero build omits DLNA casting, Mongoose, libpdr, and the Xbox and PlayStation controller fonts. Existing controller theme settings fall back to keyboard hints. A user-provided `icon.ttf` can still override them. The release uses original CardputerZero illustrations and Lucide symbols. [Resource notices](assets/NOTICE.md) · [libcurl license](https://curl.se/docs/copyright.html)

The corresponding source archive alongside the release IPK contains the pinned upstream and library sources, port and media patches, and build and installation instructions. The license describes equivalent source access under section 6(d). [Application license](LICENSE)

## Acknowledgments

Thank you to xfangfang for creating wiliwili and sharing its source. The original player's interface and support for handheld controls gave this CardputerZero port its starting point. Thank you also to the contributors who helped bring wiliwili to so many platforms. [Original wiliwili project](https://github.com/xfangfang/wiliwili)
