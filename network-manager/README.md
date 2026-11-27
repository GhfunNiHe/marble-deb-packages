# NetworkManager for Debian on marble

Debian's NetworkManager 1.58.1 disconnects every modem bearer when it stops
a mobile data connection. On marble, 81voltd uses a separate bearer for IMS
voice and SMS. The patch in this directory limits teardown to the bearer
returned by NetworkManager's own connection request. The device support
package configures ModemManager to keep the QRTR modem across suspend.

## Build on Debian arm64

From the repository root, run the same entry point as the package workflow
in a Debian sid arm64 build container. It verifies the source archives from
Debian Snapshot, extends Debian's changelog and patch series, and builds the
binary packages.

```sh
apt-get update
apt-get install -y --no-install-recommends build-essential ca-certificates fakeroot
DEB_BUILD_OPTIONS=nocheck network-manager/build-debian out
```

The release asset `network-manager-debian_1.58.1-1_arm64.tar` contains
`network-manager`, `libnm0` and `network-manager-tui` built together.
The Debian image workflow selects the bundle by the source version of the
distribution's NetworkManager. For manual installation on that Debian
version, extract the tar and install the three packages together with APT.
When updating the base version, check upstream's disconnect handling and
repeat the suspend test with both an IMS bearer and a mobile data bearer.
