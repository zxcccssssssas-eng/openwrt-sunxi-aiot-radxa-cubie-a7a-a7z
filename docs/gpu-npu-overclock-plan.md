# Plan: GPU / NPU driver port and optional overclock kmod

This is an implementation plan for Radxa Cubie A7A / A7Z on this OpenWrt 24.10 tree
(`target/linux/allwinner`, kernel **6.6.104**, SoC **Allwinner A733 / sun60iw2**).

It is **not** a code drop. The BSP already contains most of the NPU kernel source
and the GPU/NPU device-tree wiring. What is missing is the **correct GPU kernel
driver**, **OpenWrt kmod packaging**, **firmware**, and an **optional** overclock
module that stays off by default.

## 1. What this tree already has

| Area | Status in this repo | Notes |
| --- | --- | --- |
| Kernel | 6.6.104 via `include/kernel-6.6` | Matches Radxa 6.6 vendor line (`aw2511`) more closely than 5.15 |
| BSP hook | `patches-6.6/0001-add-sunxi-bsp-patch.patch` | Adds `bsp/` to Kconfig, Makefile, and `-I bsp/include` |
| Display | `CONFIG_AW_DRM=y` (sunxi-drm HDMI/DSI) | Scanout works independently of 3D GPU |
| GPU DT | `gpu@1800000`, `compatible = "img,gpu"` | Clocks, reset, `gpu_opp_table`, cooling cells present |
| GPU supply | `&gpu { gpu-supply = <&reg_dcdc4>; }` in A7A/A7Z DTS | Node has no `status`; it is enabled by default |
| GPU driver | lima + panfrost only, both **disabled** | Those are **Mali** drivers. A733 is **PowerVR BXM-4-64**, so they will never bind |
| NPU DT | `npu@3600000`, `compatible = "allwinner,npu"` | SoC dtsi has `status = "disable"`; board DTS sets `okay` + `npu-supply` |
| NPU galcore | `CONFIG_AW_NNA_GALCORE=m` | Source is in-tree; **no OpenWrt `KernelPackage`**, so `.ko` is not installed |
| NPU VIPLite | `CONFIG_AW_NNA_VIP` is not set | `vip1/` + `vip2/` sources are present (VIPLite 2.0.3) |
| CPU DVFS | `CONFIG_AW_CPUFREQ_DT=y` | README already marks CPU / frequency scaling as done |
| Overclock | none | No kmod, no extra OPP, no menuconfig option |
| Device images | `radxa_cubie-a7a`, `radxa_cubie-a7z` | `DEVICE_PACKAGES` is only uboot + `kmod-mac80211` |

Hardware (Radxa / Allwinner datasheet):

- CPU: 2× A76 up to **2.0 GHz** + 6× A55 up to **~1.8 GHz**
- GPU: Imagination **BXM-4-64 MC1** (OpenGL ES 3.2 / Vulkan 1.3 / OpenCL 3.0)
- NPU: VeriSilicon **VIP9000**, rated **3 TOPS**
- Stock DT clocks: GPU OPP 400 / 600 / 800 / **1008 MHz**; NPU OPP 492 / 852 / **1008** / 1120 MHz; `npu-vf = <1008>`

## 2. Recommended strategy

Do this as **three independent, menuconfig-optional packages**. Do not bake GPU,
NPU, or overclock into the default router image.

```
Phase 0  Inventory + kmod wrappers for what already builds
Phase 1  NPU: ship galcore (and later VIPLite) as OpenWrt kmods
Phase 2  GPU: vendor pvrsrvkm first, mainline drm/imagination as a second option
Phase 3  Optional overclock kmod (default n), plus safer DT extra-OPP overlay
Phase 4  Userspace (glibc vs musl) — only after kernel nodes exist
```

Why this order:

1. NPU kernel source is already compiled as `m`. Packaging it is the smallest win.
2. GPU has **no matching driver** in this tree. That is a real port, not a toggle.
3. Overclock without working clk/regulator/thermal paths is just a brick risk.
4. Vendor userspace is **glibc**. OpenWrt default is **musl**. Kernel modules can
   ship first; userspace is a separate decision.

## 3. Phase 0 — packaging rules for this tree

OpenWrt only installs modules that have a `KernelPackage` definition. A Kconfig
`=m` in `aiot/config-6.6` is not enough.

Use two packaging styles:

### 3.1 In-tree BSP modules (preferred for NPU)

Add wrappers in `target/linux/allwinner/modules.mk` (same pattern as
`package/kernel/linux/modules/*.mk`):

```make
define KernelPackage/aw-nna-galcore
  SUBMENU:=Other modules
  TITLE:=Allwinner Vivante NPU (galcore)
  DEPENDS:=@TARGET_allwinner
  KCONFIG:=CONFIG_AW_NNA_GALCORE=m
  FILES:=$(LINUX_DIR)/bsp/drivers/npu/aw_nna_galcore/galcore.ko
  AUTOLOAD:=$(call AutoLoad,50,galcore,1)
endef
```

Confirm the built `.ko` path after one kernel compile. The Kbuild sets
`MODULE_NAME ?= galcore` and `obj-$(CONFIG_AW_NNA_GALCORE) = galcore.o`.

### 3.2 Out-of-tree modules (GPU vendor DDK + overclock)

Use `package/kernel/<name>/Makefile` like `package/kernel/button-hotplug`:

- `include $(INCLUDE_DIR)/kernel.mk`
- `FILES:=$(PKG_BUILD_DIR)/foo.ko`
- `AUTOLOAD` only if safe to load at boot
- `DEPENDS:=@TARGET_allwinner`
- Keep `KCONFIG:=` empty for pure out-of-tree sources

### 3.3 menuconfig surface

Add a small submenu under **Kernel modules → Allwinner A733 extras**:

- `kmod-aw-nna-galcore` (NPU unified)
- `kmod-aw-nna-vip` (NPU VIPLite, mutually exclusive with galcore)
- `kmod-img-bxm` / `kmod-drm-imagination` (GPU, mutually exclusive)
- `kmod-sunxi-overclock` (**default n**, not in `DEVICE_PACKAGES`)

Do **not** add GPU/NPU/overclock to `DEVICE_PACKAGES` in
`target/linux/allwinner/image/aiot.mk` until they autoload cleanly on both
boards.

## 4. Phase 1 — NPU driver port

A733 NPU is Vivante VIP9000. This BSP already has two kernel stacks that both
bind `compatible = "allwinner,npu"`:

| Stack | Kconfig | Device node | Userspace | Fit for OpenWrt |
| --- | --- | --- | --- | --- |
| Unified / galcore | `AW_NNA_GALCORE` | `/dev/galcore` | TIM-VX / OpenVX / OpenCL | Heavier; version-locked |
| VIPLite / vipcore | `AW_NNA_VIP` + `NNA_VIP2` | `/dev/vipcore` | VIPLite + ACUITY NBG | Smaller; better first target |

They **cannot** be loaded together. Same DT node, same IRQ 65, same clocks.

### 4.1 What to do with galcore (already `=m`)

1. Wrap it as `kmod-aw-nna-galcore` (section 3.1).
2. Keep board DTS as-is: `&npu { npu-supply = <&reg_dcdc2>; status = "okay"; }`.
3. On first boot, check:
   - `dmesg | grep -i galcore`
   - `/dev/galcore` exists
   - `clk_npu` / `clk_parent` / `clk_bus` / resets / `npu-supply` all succeed
4. Fix the SoC dtsi typo `status = "disable"` → `disabled` while touching NPU DT
   (harmless today because board DTS overrides it).

Platform glue already exists:

- `bsp/drivers/npu/aw_nna_galcore/os/linux/kernel/platform/allwinner/gc_hal_kernel_platform_allwinner.c`
- Reads `npu-vf`, SID VF table, OPP voltages, `clk_npu`, regulator `"npu"`
- Includes `<sunxi-sid.h>` — already on the BSP include path from patch 0001

### 4.2 Version lock (do not ignore)

Galcore / userspace versions in the wild (must match **exactly**):

| Piece | Version | Where |
| --- | --- | --- |
| This tree's galcore | **6.4.18.6.904649** | `aw_nna_galcore/inc/gc_hal_version.h` |
| Radxa `allwinner-bsp` galcore | 6.4.18.x (same family) | `drivers/npu/aw_nna_galcore` |
| Public unified userspace (`ai-sdk` / `unified-tina`) | **6.4.15.3.690884** | [ZIFENG278/ai-sdk](https://github.com/ZIFENG278/ai-sdk) |
| Community 6.6 galcore patch set | 6.4.15.3 + `galcore_6.6_kernel_api_drift.patch` | [MaverickLong](https://github.com/MaverickLong/Radxa-A733-NPU-Unified-Driver-Support-Package) |
| Some Radxa SDK notes | 6.4.18.0.906669 | newer official HAL; still not this tree's 6.4.18.6 |

Mixing any of these pairs is a known NPU failure mode. This tree already
includes 6.6 API ifdefs, so the MaverickLong drift patch is a reference, not
a required import, **unless** we downgrade to 6.4.15.3 for userspace match.

Plan:

- **Kernel-only milestone:** ship current 6.4.18 galcore so `/dev/galcore` appears.
- **Userspace milestone:** either
  - downgrade in-tree galcore to **6.4.15.3** (only public userspace) **or**
  - wait for a matching 6.4.18.6 userspace.
- Prefer **VIPLite** for OpenWrt inference (`kmod-aw-nna-vip` + `libVIPhal`
  from `ai-sdk` `viplite-tina`) because VIPLite is smaller and does not need
  the unified HAL version dance.
- `reserved-memory` in `sun60iw2p1.dtsi` currently only reserves BL31. If
  galcore probe fails on CMA/contiguous alloc, add an NPU reserved/CMA node
  before chasing driver bugs.

### 4.3 VIPLite as the OpenWrt-friendly NPU option

1. Add `KernelPackage/aw-nna-vip` with
   `KCONFIG:=CONFIG_AW_NNA_VIP=m CONFIG_NNA_VIP2=y`.
2. Make it conflict with `kmod-aw-nna-galcore`.
3. Enable `NPU_SET_CLK_VOL` if voltage/clock from DT should stay in-driver.
4. Probe test: `/dev/vipcore` after `insmod` (`DEVICE_NAME` is `vipcore`,
   not `vipscore`).
5. Blacklist the unused stack (`galcore` vs `vipcore`) so they cannot both
   bind `allwinner,npu`.

VIPLite in this tree is **2.0.3** (`vip_lite_version.h`, patch string
`4-AW-2025-10-27`). Userspace must match that VIPLite ABI, not galcore.

### 4.4 NPU userspace on musl

Vendor libs are `aarch64-none-linux-gnu` (glibc). OpenWrt 24.10 default libc is
musl. Do **not** expect TIM-VX / ACUITY binaries to run on a stock image.

Options, in order of least pain:

1. Kernel kmod only (this plan's first deliverable).
2. Optional `CONFIG_USE_GLIBC` target or a glibc chroot/container for NPU apps.
3. Rebuild VIPLite userspace against musl if source is available (usually not).

Document this in the package description so nobody files "NPU kmod loaded but
onnxruntime fails" as a kernel bug.

## 5. Phase 2 — GPU driver port

### 5.1 Do not enable lima / panfrost

`bsp/drivers/gpu/` is Allwinner's copy of Lima (Mali-400) and Panfrost
(Mali Midgard/Bifrost). A733 DT is `compatible = "img,gpu"`. Enabling those
Kconfigs wastes image space and will not bind.

Leave `CONFIG_AW_DRM_LIMA` / `CONFIG_AW_DRM_PANFROST` unset.

### 5.2 Two GPU stacks (pick one per image)

```
                    sunxi-drm (already =y)
                         │  HDMI / DSI scanout  → /dev/dri/card0
                         │
          ┌──────────────┴──────────────┐
          │                             │
   vendor pvrsrvkm                 mainline drm/imagination
   (img-bxm / DDK 24.2)            (powervr.ko + firmware)
          │                             │
   /dev/dri/card1 + renderD128     /dev/dri/renderD128
   closed libVK_IMG / GLES         Mesa pvr Vulkan
```

Display and render are **split**. sunxi-drm stays the KMS device. The 3D driver
is render-only and talks through DRM-PRIME.

### 5.3 Track A — vendor `pvrsrvkm` (first GPU milestone)

This is what Radxa Debian actually ships (`img-bxm-dkms`, DDK `24.2@6603887`,
firmware BVNC **`36.56.104.183`**).

Source is **not** in this tree and **not** in public `radxa/allwinner-bsp`
(`drivers/gpu/` there is also only lima/panfrost). The DDK lives in the
Radxa apt package `img-bxm-dkms` / Tina `rogue_km`. Import it as an
out-of-tree OpenWrt package (OpenWrt has no DKMS):

1. Vendor drop: Radxa `img-bxm-dkms` (`rogue_km` / `sunxi_linux` kbuild) or
   the matching Allwinner Tina `pvrsrvkm` tree.
2. New package: `package/kernel/img-bxm/` (or `pvrsrvkm`).
3. Build with the same flags Radxa uses:
   `BUILD=release`, `KERNELDIR=$(LINUX_DIR)`, platform `sunxi_linux`.
4. Known 6.6 compile break: `rgx_sunxi/sunxi_platform.c` includes
   `<sunxi-sid.h>`. This tree already has
   `target/linux/allwinner/files/bsp/include/sunxi-sid.h` and the kernel
   `-I$(srctree)/bsp/include` flag. Pass that include path into the DKMS
   Makefile (`EXTRA_CFLAGS += -I$(LINUX_DIR)/bsp/include`).
   ([Radxa forum: img-bxm-dkms on 6.6.98](https://forum.radxa.com/t/failed-to-build-module-img-bxm-dkms/30839))
5. Apply the community DRM-PRIME import patch from
   [ayiejosh/a733-powervr-fex](https://github.com/ayiejosh/a733-powervr-fex)
   (`gem_prime_import` / `prime_fd_to_handle`). Vendor DDK left these empty;
   without them buffer sharing with sunxi-drm / Mesa / wlroots fails.
6. Firmware package: `package/firmware/img-bxm-firmware` installing
   `36.56.104.183` (or the exact filename the DDK requests) under
   `/lib/firmware/...`.
7. DT: keep `compatible = "img,gpu"`. Vendor driver matches that. Add
   `status = "okay"` explicitly on `&gpu` for clarity.
8. Power: consider enabling `CONFIG_AW_GPU_PM_DOMAINS` and the commented
   `power-domains = <&pd SUN60IW2_PCK_GPU_TOP>` once the driver probes.
9. **Do not** use `pvrsrvkm` as a live desktop compositor. Community bring-up
   on 6.6 reports a kernel deadlock (mutex spin-on-owner in IRQ) if a
   compositor runs on the vendor render node. Off-screen GLES/Vulkan is the
   supported shape on this kernel.

License: vendor DDK is proprietary. Keep it as an **optional** package with a
clear license note. Do not enable it in default images.

### 5.4 Track B — mainline `drm/imagination` backport (second GPU option)

Upstream `drivers/gpu/drm/imagination` (`CONFIG_DRM_POWERVR`) is the open
driver. BXM-4-64 MC1 is marked supported in the **6.18 / 7.3** era, not in
stock 6.6.

Port path:

1. Copy `drivers/gpu/drm/imagination` from a recent kernel (6.18+ or
   drm-misc-next) into this tree as either:
   - `target/linux/allwinner/files/bsp/drivers/gpu/imagination/` + Kconfig, or
   - `package/kernel/drm-imagination/` out-of-tree backport.
2. Backport helpers the 6.6 DRM core is missing (GEM/shmem, scheduler, VM
   helpers). This is the hard part; expect API shim patches, not a clean copy.
3. Firmware from
   [imagination linux-firmware powervr](https://gitlab.freedesktop.org/imagination/linux-firmware/-/tree/powervr/powervr)
   — package as `powervr-firmware`.
4. DT compatible: mainline wants `img,img-bxm-4-64` (or the current binding),
   **not** `img,gpu`. Add a fallback:

   ```dts
   compatible = "img,img-bxm-4-64", "img,gpu";
   ```

   Keep clocks/resets/OPP as they are; map them in the platform glue if the
   mainline driver expects different `clock-names`.
5. Userspace: Mesa `pvr` Vulkan. Same musl problem as NPU. Mesa on OpenWrt
   lives in the `video` feed and is a large build.

[alexcaoys/allwinner-bsp](https://github.com/alexcaoys/allwinner-bsp) already
runs this stack on Cubie A7Z with Linux **6.18**. Treat that as the reference
for clock names, firmware path, and Mesa flags — not as something to merge
wholesale into 6.6.

### 5.5 Which GPU track to implement first

| Goal | Track |
| --- | --- |
| `/dev/dri/renderD128` on this 6.6 OpenWrt image | **A: pvrsrvkm** |
| GLES/Vulkan apps in a glibc chroot | **A** |
| Long-term open driver, no DDK | **B**, ideally after a kernel bump |
| Default router image | **neither** |

Do not try to run both `pvrsrvkm` and `drm/imagination` on the same DT node.

## 6. Phase 3 — optional overclock kmod

Datasheet limits (treat as the **safe ceiling** for anything we ship):

| Block | Stock DT | Datasheet / Radxa | Community extreme (do not default) |
| --- | --- | --- | --- |
| CPU A76 | OPP up to ~2002 MHz | 2.0 GHz | ~2080 MHz |
| CPU A55 | OPP up to ~1794 MHz | ~1.8 GHz | firmware often caps ~1716–1794 |
| GPU | 400–1008 MHz | ~1.0 GHz class | 1488 MHz |
| NPU | 492–1120 MHz, `npu-vf=1008` | 3 TOPS | 2520 MHz via sysfs poke |

Community reference (out of tree, 5.15-oriented):
`llm_unified_overclock.ko` / `cpu_overclock.ko` /
`echo 2520,1488 > .../3600000.npu/llm_overclock`.
That is a useful **API sketch**, not something to enable by default.

### 6.1 Package shape

New out-of-tree package `package/kernel/sunxi-overclock/`:

```
package/kernel/sunxi-overclock/
  Makefile
  src/Makefile
  src/sunxi_overclock.c
  src/sunxi_overclock.h
```

`KernelPackage/sunxi-overclock`:

- `DEPENDS:=@TARGET_allwinner`
- `AUTOLOAD` **omitted** (manual `insmod` or an init script that only runs when
  `/etc/config/overclock` is enabled)
- `config` file with `option enabled 0`
- Package description must say: extra heat, extra voltage, warranty void,
  can reset or damage the board

menuconfig: **Kernel modules → Allwinner A733 extras → kmod-sunxi-overclock**
default **n**. Never add it to `DEFAULT_PACKAGES` or `DEVICE_PACKAGES`.

### 6.2 Safer design than a raw clock poke

Implement **two layers**. The kmod is optional; the DT overlay is the default
recommendation.

**Layer 1 — extra OPP overlay (no kmod)**

- DT overlay or a `CONFIG` that adds already-characterized OPPs:
  - NPU: enable the existing **1120 MHz** node (already in
    `sun60iw2p1-cpu-vf.dtsi`, gated by VF bin)
  - GPU: keep 1008 MHz as max unless a measured 1.2 GHz point is added with
    voltage
  - CPU: do not add points above the VF table; some bins already have 2.0 GHz
- Users pick the overlay in U-Boot / extlinux. No kernel module required.

**Layer 2 — `kmod-sunxi-overclock` (opt-in)**

A small platform driver that:

1. Looks up `npu@3600000` and `gpu@1800000` by DT.
2. Uses **clk + regulator + pm_opp** APIs (`clk_set_rate`,
   `dev_pm_opp_set_rate`, `regulator_set_voltage`), not raw MMIO.
3. Refuses a rate that has no OPP voltage (no "set 2520 MHz at 800 mV").
4. Exports sysfs, for example:
   - `/sys/module/sunxi_overclock/parameters/npu_mhz`
   - `/sys/module/sunxi_overclock/parameters/gpu_mhz`
   - `/sys/module/sunxi_overclock/parameters/profile` = `stock|npu1120|custom`
5. Hard caps in Kconfig:
   - `SUNXI_OVERCLOCK_NPU_MAX_KHZ` default **1120000**
   - `SUNXI_OVERCLOCK_GPU_MAX_KHZ` default **1008000**
   - `SUNXI_OVERCLOCK_ALLOW_EXTREME` default **n** (only then raise caps)
6. Hooks thermal: if `npu_thermal_zone` / `gpu_thermal_zone` hit the existing
   crit trips, drop back to stock OPP.
7. Does **not** touch DRAM/DMC. `CONFIG_AW_DMC_DEVFREQ` is already on; RAM
   overclock is out of scope.

Do **not** vendor-import a module that writes undocumented PLL registers to
reach 2520 / 1488 as the packaged default. If someone wants that, they can
raise the Kconfig cap on their own build.

### 6.3 Init / LuCI (optional follow-up)

- `/etc/init.d/sunxi-overclock` reads UCI and writes sysfs only when
  `enabled=1`.
- LuCI page later; not required for the kmod to be useful.

## 7. Concrete file list (when implementing)

| Path | Change |
| --- | --- |
| `target/linux/allwinner/modules.mk` | `kmod-aw-nna-galcore`, `kmod-aw-nna-vip` |
| `target/linux/allwinner/aiot/config-6.6` | Keep galcore `=m`; add VIP `=n`; do not enable lima/panfrost |
| `package/kernel/img-bxm/` | Vendor GPU DDK + PRIME patch + `sunxi-sid.h` cflags |
| `package/firmware/img-bxm-firmware/` | PowerVR firmware blob |
| `package/kernel/drm-imagination/` | Optional mainline backport (phase 2B) |
| `package/firmware/powervr-firmware/` | linux-firmware powervr files |
| `package/kernel/sunxi-overclock/` | Optional kmod + UCI, default disabled |
| `target/linux/allwinner/files/arch/arm64/boot/dts/allwinner/sun60i-a733-cubie-a7{a,z}.dts` | Explicit `&gpu { status = "okay"; }`; optional extra OPP overlay |
| `sun60iw2p1.dtsi` | Fix NPU `status = "disable"` typo; optional GPU compatible fallback |
| `target/linux/allwinner/image/aiot.mk` | Only add NPU/GPU kmods after probe is proven |

## 8. Verification (on hardware)

Kernel-only checks, no userspace blobs required:

```sh
# NPU
lsmod | grep -E 'galcore|vipcore'
ls -l /dev/galcore /dev/vipcore
dmesg | grep -iE 'galcore|vipcore|npu'
cat /sys/kernel/debug/clk/clk_npu/clk_rate   # path may differ

# GPU
ls -l /dev/dri/
dmesg | grep -iE 'pvrsrvkm|powervr|img,gpu'
# expect sunxi-drm as card0; render node only after GPU kmod

# clocks / thermal
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq
cat /sys/class/thermal/thermal_zone*/temp

# overclock (only if package installed and enabled)
cat /sys/module/sunxi_overclock/parameters/profile
```

Userspace checks (glibc image or chroot):

- NPU VIPLite: ACUITY NBG sample
- NPU unified: TIM-VX sample **only** if kernel/userspace versions match
- GPU vendor: `vulkaninfo` / off-screen GLES
- GPU mainline: Mesa `pvr` + firmware load line in dmesg

## 9. Risks

- **Wrong GPU driver:** lima/panfrost will never work on A733.
- **galcore ABI mismatch:** 6.4.18 kernel vs 6.4.15 userspace.
- **musl:** vendor NPU/GPU userspace will not link.
- **pvrsrvkm compositor deadlock** on 6.6 vendor DDK.
- **Overclock:** extra voltage on DCDC2 (NPU) / DCDC4 (GPU) / CPU rails;
  thermal zones already exist — use them.
- **License:** do not enable proprietary DDK in default images.
- **Image size:** Mesa + firmware + galcore is large for a router squashfs.
  Keep these as optional ipk, not part of the factory image.

## 10. Suggested implementation order

1. `kmod-aw-nna-galcore` wrapper + boot probe on A7A/A7Z.
2. `kmod-aw-nna-vip` as a conflicting alternative.
3. `kmod-img-bxm` + firmware + `sunxi-sid.h` cflags + PRIME patch.
4. Explicit `&gpu` status / compatible cleanup in DTS.
5. `kmod-sunxi-overclock` default n, caps at DT/datasheet max, UCI off.
6. (Later) mainline `drm/imagination` backport or a 6.18 kernel bump.
7. (Later) glibc userspace story for TIM-VX / Mesa.

## 12. Implementation status (executed)

Track **B** (mainline `drm/imagination` backport) was implemented instead of
vendor `pvrsrvkm`, as requested.

| Package | menuconfig | Default | Notes |
| --- | --- | --- | --- |
| `kmod-aw-nna-galcore` | Allwinner A733 extras | extra `.ipk`, not in image | `/dev/galcore`, ABI 6.4.18.6 |
| `kmod-aw-nna-vip` | Allwinner A733 extras | extra `.ipk`, not in image | `/dev/vipcore`, conflicts with galcore |
| `kmod-drm-powervr` | Allwinner A733 extras | not built in CI | 6.18+ PowerVR + bundled drm_gpuvm, `exp_hw_support=1` |
| `powervr-firmware` | Firmware | pulled by GPU kmod | `powervr/rogue_36.56.104.183_v1.fw` |
| `kmod-sunxi-overclock` | Allwinner A733 extras | extra `.ipk`, not in image | UCI `/etc/config/overclock`, caps 1120/1008 MHz |

CI `firmware.config` sets NPU/overclock packages to `=m` (build ipk, skip
squashfs). `aiot/config-6.6` no longer forces `CONFIG_AW_NNA_GALCORE=m`;
the KernelPackage KCONFIG enables it only when the extra is selected.

Enable with `make menuconfig` (do not add to `DEVICE_PACKAGES` until probe is
proven on hardware):

```
Kernel modules → Allwinner A733 extras → kmod-drm-powervr
Kernel modules → Allwinner A733 extras → kmod-aw-nna-vip   (or galcore)
```


## 11. Sources used for this plan

- This tree: `target/linux/allwinner/`, board DTS, `aw_nna_galcore`, `aw_nna_vip`
- [Radxa Cubie A7A docs](https://docs.radxa.com/en/cubie/a7a) — SoC / GPU / NPU specs
- [Radxa NPU / ACUITY guide](https://docs.radxa.com/en/cubie/a7a/app-dev/npu-dev/cubie-acuity-sdk)
- [MaverickLong unified-driver notes](https://github.com/MaverickLong/Radxa-A733-NPU-Unified-Driver-Support-Package) — 6.4.15 vs 6.4.18 lock
- [ZIFENG278/ai-sdk](https://github.com/ZIFENG278/ai-sdk) — `unified-tina` / `viplite-tina` userspace
- [radxa/allwinner-bsp](https://github.com/radxa/allwinner-bsp) — NPU sources public; PowerVR DDK is **not** in that git
- [ayiejosh/a733-powervr-fex](https://github.com/ayiejosh/a733-powervr-fex) — pvrsrvkm PRIME, 6.6 deadlock
- [alexcaoys/allwinner-bsp](https://github.com/alexcaoys/allwinner-bsp) — mainline PowerVR on 6.18
- [Radxa img-bxm-dkms 6.6 thread](https://forum.radxa.com/t/failed-to-build-module-img-bxm-dkms/30839) — `sunxi-sid.h`
- Allwinner A733 datasheet — 2.0 GHz CPU, BXM-4-64, 3 TOPS NPU
