# marble 主电量计验证

`xiaomi-mipps-auth` 为 Redmi Note 12 Turbo / POCO F5 的主电量计运行手机自带的 `batterysecret`。电量计验证需要 marble 内核驱动提供 `verify_slave_flag`、`verify_digest` 和 `authentic`。充电头的 MiPPS 握手由内核处理。这个包仅包含启动服务和准备工具。

在装有 `dpkg-dev`、`debhelper` 及 ARM64 构建环境的 Debian 或 Ubuntu 中，进入本目录运行 `dpkg-buildpackage -b -us -uc`。生成的 `xiaomi-mipps-auth_*_arm64.deb` 位于上一级目录。把包复制到手机后，从包所在目录运行 `sudo apt install ./xiaomi-mipps-auth_*_arm64.deb`。

首次使用时，需要从本机 ROM 准备验证器的运行目录。以下命令以 root 身份在手机上执行。假设 `/mnt/marble-rom/` 下的 `system`、`system_ext`、`vendor` 和 `apex` 分别对应 Android 的同名只读目录，`system` 直接包含 `bin` 和 `lib64`，`apex` 下已挂载 `com.android.runtime` 和 `com.android.vndk.v31`。

```sh
set -eu
rom=/mnt/marble-rom
runtime=/var/lib/xiaomi-mipps-auth/android
work=$(mktemp -d /run/marble-gauge.XXXXXXXX)
trap 'rm -f "$work/board"; rmdir "$work"' EXIT
install -d /var/lib/xiaomi-mipps-auth
/usr/libexec/xiaomi-mipps-board-property "$rom/vendor/build.prop" "$work/board"
/usr/libexec/prepare-android-runtime --system "$rom/system" --system-ext "$rom/system_ext" \
    --vendor "$rom/vendor" --apex "$rom/apex" --board-properties "$work/board" --output "$runtime"
systemctl start xiaomi-mipps-fuel-gauge.service
cat /sys/class/power_supply/qcom-battmgr-usb/device/xiaomi/authentic
```

准备工具验证动态链接依赖，并要求 `--output` 指向尚未创建的目录。已有运行目录时可直接启动服务。读数 `1` 表示 ADSP 已接受主电量计的验证结果。之后电源驱动就绪或插入 USB-C 设备时，udev 会在需要时启动服务。可用 `journalctl -b -u xiaomi-mipps-fuel-gauge.service` 查看本次启动记录。
