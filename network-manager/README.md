# NetworkManager for marble

When NetworkManager disconnects mobile data, it releases the bearer it opened.
The separate IMS bearer used by 81voltd stays connected for voice and SMS.
The device support package configures ModemManager to retain the QRTR modem
over suspend.

## Build and install

The `Build DEBs` workflow builds NetworkManager with the other device packages
on Ubuntu 24.04 arm64. To build it manually in a Noble arm64 build environment,
run these commands as root from the repository root.

```sh
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates fakeroot git
DEB_BUILD_OPTIONS=nocheck DEB_BUILD_PROFILES=nocheck network-manager/build out
```

In **Run workflow**, select a NetworkManager Git repository and a tag, branch,
or commit. The default builds the upstream release tag from the package
changelog. For a local build, set `SRC_NETWORKMANAGER_REPO` and
`SRC_NETWORKMANAGER_REF` to override the same settings.

The build applies this repository's `debian/` directory to the checkout.
The packaging and Ubuntu patches come from
[Ubuntu 1.58.1-1ubuntu3.1](https://launchpad.net/ubuntu/+source/network-manager/1.58.1-1ubuntu3.1).
Noble builds libndp 1.9 from source for the IPv6-only cellular network
translation code (464XLAT). The PPP plugins use the pppd headers for the ABI
versions installed by the supported images.

The image installer takes the runtime packages directly from the device
release. Ubuntu keeps its Netplan profile storage and Debian uses NetworkManager
keyfiles. For a manual install, download the runtime packages from one release
to an empty directory and run `sudo apt install ./*.deb` there. Install the
matching `libnm-dev` too if the target already has the development package.
