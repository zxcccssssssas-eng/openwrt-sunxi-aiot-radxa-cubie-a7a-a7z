# RTL8812AU reception stopping after long uptime

The selected package is `kmod-rtw88-8812au`, using the mac80211 backport's
`rtw88_usb` transport. The separate `rtl8812au-ct` vendor driver is unaffected.

## Serial observations (2026-10-07)

The FTDI console at `/dev/ttyUSB0`, 115200 8N1, showed OpenWrt 24.10.3,
kernel 6.6.104, after 22 days of uptime. The RTL8812AU's radio was reported
up, but both station interfaces were disconnected and an active scan
returned no BSS entries. Wi-Fi power saving was off, USB `power/control`
was `on`, and memory was plentiful at inspection time.

Rebinding only the RTL8812AU USB interface and bringing its radio up
restored authentication, association, and received packets. The AIC8800 AP
remained running. This supports a stuck driver/device receive path, but
does not establish the original trigger: the available kernel/system logs
had already rolled over, with extensive AIC8800 debug output.

Rebind changes the PHY/interface number. The existing `wwan` configuration
explicitly named the original station interface, so it needed a runtime
DHCP binding to the new interface. This was applied with netifd's
`add_dynamic` method, leaving persistent UCI configuration unchanged.

## Driver change

The backported USB transport has four receive URBs. Previously, a USB
completion error or a failed atomic buffer allocation silently retired a
request. Enough such failures could stop all reception until USB rebind;
a normal radio restart does not recreate those requests.

`052-wifi-rtw88-usb-retry-transient-RX-failures.patch` adds a delayed retry
per receive request for `EPROTO`, `EILSEQ`, `ETIME`, `ETIMEDOUT`, `ECOMM`, and
`EOVERFLOW`, as well as allocation failures and `ENOMEM`/`EAGAIN` submission
failures. The worker can allocate with `GFP_KERNEL`; a 20 ms delay avoids
a tight retry loop during persistent errors. Buffers are released on failed
submission, and the control block no longer retains a freed buffer pointer.

Disconnect blocks new retries under a spinlock, waits for retry workers,
then kills the URBs. RX teardown drains work before purging the packet
queue, including probe failure cleanup. Cancellation, disconnect, and
halted endpoints are not retried. An `EPIPE` stall or a completely
unresponsive device may still require USB rebind.

The mac80211 package release is increased to 3 so new builds identify the
changed modules and their matching wireless stack dependencies.
The GitHub firmware seed also selects `kmod-rtw88-8812au`, pulling in the
matching transport, chip modules, and firmware for future image builds.

## Validation

The ARM64 mac80211 packages compiled successfully with the existing
Allwinner AIOT configuration and Linux 6.6.104:

```sh
make package/kernel/mac80211/compile -j4 V=s
```

A host fault-injection harness compiles the actual prepared RX functions
with USB/workqueue shims and AddressSanitizer/UndefinedBehaviorSanitizer:

```sh
python3 package/kernel/mac80211/tests/rtw88-usb-rx-recovery.py \
  build_dir/target-aarch64_generic_musl/linux-allwinner_aiot/mac80211-regular/backports-6.12.44/drivers/net/wireless/realtek/rtw88/usb.c
```

It exercises 100 cycles of losing and recovering all four receive requests
for each recoverable error, valid and malformed transfer lengths, repeated
allocation failure, submission failure, terminal completion errors, buffer
ownership, and suppression of retries/submissions after shutdown. The
shims do not test kernel concurrency or USB hardware behavior.

## Live hotfix (2026-10-07)

At the user's request, the patched `rtw88_usb.ko` was transferred over the
115200-baud USB serial console in a compressed archive. Both the archive
and extracted module passed SHA-256 verification on the router before
installation. The running kernel's module version matched the build, and
the packaged `rtw88_core`, `rtw88_88xxa`, `rtw88_8812a`, and `rtw88_8812au`
modules were byte-for-byte identical to the router's copies. This allowed
replacing only the USB transport module while preserving the running
wireless stack and AIC8800 AP.

The original module, configuration copies, module parameter, and a rollback
script are saved in a protected `/root/rtl8812au-hotfix-*` directory on the
router. The installed replacement is `/lib/modules/6.6.104/rtw88_usb.ko`:

```text
Original SHA-256: baa0fce30eb92b11eb50ac5c0687c6008289aa1ad2d43ef6ab6b99970538d1e7
Patched SHA-256:  d10bf213ae542d0f95e6e45de0f2f9f6fc8da5e40750fb5b6055c97e78db0d1a
```

The original USB transport and adapter modules were unloaded, the patched
transport was loaded, and the adapter module was reloaded. Association,
authentication, DHCP, and bidirectional packet traffic recovered on the
new station interface. DHCP was rebound at runtime; persistent UCI
configuration was preserved.
Ten gateway pings and ten public-network pings each returned ten replies
with zero packet loss using the patched adapter.

This manual replacement survives reboot. The package database remains at
release r2 because this is a targeted module hotfix; a package reinstall or
firmware upgrade can overwrite it. For a packaged upgrade, install the
matching complete wireless package set or rebuild firmware, because the
mac80211 package release/ABI changes together.

A multi-day hardware soak is still necessary to confirm that the reported
long-uptime issue is resolved.
