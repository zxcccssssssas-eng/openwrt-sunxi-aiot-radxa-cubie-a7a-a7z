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

The new module has not been installed on the router. Serial rebind recovery
was tested with the original module; a multi-day hardware soak of the
patched build remains necessary to confirm the reported long-uptime issue
is resolved. Install a matching complete wireless package set or rebuild
firmware, because the mac80211 package release/ABI changes together.
