# Mali-G76 MP12 (Exynos 9820, SM-G970F) on mainline Panfrost — bring-up spec

Status: research only, 2026-10-29. Nothing was built and no register was written.

Labels used below:
- **[V]** verified, either on the phone (kernel 7.2.0-rc2-s10e, builds #6 and #7) or by reading source.
- **[I]** inferred, not proven.

Path abbreviations:
- `DS` = `src/lineage-kernel` (Samsung downstream, kbase bv_r32p1)
- `ML` = `src/linux-9820` (our 7.2 tree)
- `UB` = `src/u-boot/dts/upstream/src/arm64/exynos` (the DTs actually booted)

Note: `UB/exynos9820.dtsi` was edited by someone else at 02:21 while this was being written, so its line numbers may shift again.

---

## 0. TL;DR

| Item | Value |
|---|---|
| GPU regs | `0x18500000`, size `0x5000` [V, DS `exynos9820-mali_common.dtsi:14-16`] |
| IRQs | job = GIC_SPI **288**, mmu = GIC_SPI **287**, gpu = GIC_SPI **289**, level-high. Downstream writes flag `0` [V, ibid. `:17-20`]. GIC is GICv2-style with 3 cells [V, UB `exynos9820.dtsi:688-691`] |
| GPU | Mali-G76 = kbase "tNOx", panfrost `GPU_MODEL(g76, 0x7001)` [V, ML `panfrost_gpu.c:226`]. The only tNOx revision kbase r32p1 knows is **r0p0** [V, DS `bv_r32p1/mali_kbase_hw.c:163-164`], so expect **r0p0** [I]. GPU_ID was not read (see §3) |
| Core clock | `PLL_G3D` lives in **CMU_TOP** (`0x1a240140`), not in CMU_G3D. It runs at **156 MHz** now [V: PLL_CON0 = `0xa0600412` → M=96, P=4, S=2, 26 MHz·96/(4·2²) = 156 MHz; the clk tree agrees]. DVFS goes through **ACPM** (`ACPM_DVFS_G3D`). Our tree already has an ACPM DVFS clock `<&acpm_ipc EXYNOS9820_CLK_ACPM_DVFS_G3D>`, which is assigned 156 MHz at boot [V] |
| Power domain | PMU `0x15862100` (CONFIG) / `0x15862104` (STATUS), bit 0. Mainline `samsung,exynos9820-pd` exists and is in the DT as `pd_embedded_g3d` [V]. **The bootloader leaves it ON and mainline turns it OFF at 0.72 s** ("Disabling unused power domains"; genpd `active_time` 656 ms / 1508 ms, then 1 power-off) [V] |
| Voltage | An S2MPS19 buck, always-on, with its voltage set **by ACPM firmware** as part of G3D DVFS (`CMUCAL_ACPM_VCLK2(dvfs_g3d…)`). Linux never touches a G3D regulator downstream [V, DS `cal-if/exynos9820/acpm_dvfs_exynos9820.h:31`, `gpexbe_clock.c:77`]. Which buck number it is: unknown, and not needed |
| **Blocker** | Powering G3D back ON is **not** a single PMU write. Downstream does a 10-step sequence: PLL_G3D CON2, a reset toggle of `G3D_CONFIGURATION`, an atomic set bit in `EMBEDDED_G3D_OUT`, then EMBEDDED_G3D. After that comes an **EL3 SMC to restore the G3D TZPC** (`0x82000410, WAKEUP, 0x18410204, group 2`). Mainline `exynos-pm-domains` does none of this. If the TZPC is lost, the first non-secure GPU register access is an external abort, which hangs the SoC [I, strong] |
| Workaround (Phase 1) | Keep G3D in the state the bootloader leaves it in. Disable the `pd_embedded_g3d` node (or boot with `pd_ignore_unused`), give the GPU node **no** `power-domains`, and use the ACPM g3d clock. Panfrost then sees a GPU that is powered, clocked and NS-accessible [I] |
| Display | `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128`. phoc (embedded wlroots) has the variable [V, it is a string in `/usr/bin/phoc`]. Mesa 26.2.4 ships `panfrost_dri.so` [V]. simpledrm can import dma-bufs (`DRM_GEM_SHMEM_DRIVER_OPS` → `drm_gem_shmem_prime_import_no_map`) [V] |
| Kernel config | `CONFIG_DRM_PANFROST` is **not set** in the running kernel [V, `/proc/config.gz`] |

---

## 1. Register base, size, IRQs, GPU ID

Downstream node [V] (`DS/arch/arm64/boot/dts/exynos/exynos9820-mali_common.dtsi:14-21`):

```
mali: mali@18500000 {
    compatible = "arm,mali";
    reg = <0x0 0x18500000 0x5000>;
    interrupts = <0 288 0>, <0 287 0>, <0 289 0>;
    interrupt-names = "JOB", "MMU", "GPU";
    g3d_cmu_cal_id = <ACPM_DVFS_G3D>;
```

Mainline form:
- `interrupts = <GIC_SPI 288 IRQ_TYPE_LEVEL_HIGH>, <GIC_SPI 287 …>, <GIC_SPI 289 …>;`
- `interrupt-names = "job", "mmu", "gpu";` (panfrost wants these lowercase names)
- Mali IRQs are level-high. The downstream `0` means "type none". Every Exynos Mali node in `ML/arch/arm64/boot/dts/exynos/` (5433, 7, 7870) uses LEVEL_HIGH [V].

The register size `0x5000` matches downstream and the other Exynos Mali nodes. Panfrost maps the whole first resource.

GPU ID:
- G76 = tNOx, product major 1 in arch 7.0 → panfrost id `0x7001` [V, ML `drivers/gpu/drm/panfrost/panfrost_gpu.c:226`].
- kbase r32p1 (the DDK the S10e ships, `CONFIG_MALI_BIFROST_R32P1=y`, DS `arch/arm64/configs/exynos9820-beyond0lte_defconfig:3368`) lists **only r0p0** for tNOx (`mali_base_hwconfig_issues.h:285`, `mali_kbase_hw.c:163-164`). So expect `GPU_ID = 0x7001_00xx`, i.e. r0p0 [I].
- panfrost has `hw_issues_g76 = 0` (`panfrost_issues.h:259`) and `hw_features_g76` (`panfrost_features.h:85`). The kbase r0p0 issue list includes `BASE_HW_ISSUE_TNOX_1194` (`mali_base_hwconfig_issues.h:290`). That is a userspace/compiler workaround that Mesa handles, not the kernel [I].
- The MP12 core mask comes from SHADER_PRESENT at runtime.

Coherency: downstream runs **non-coherent**.
- `CONFIG_MALI_EXYNOS_LLC` is not set (defconfig `:3414`), so `gpexbe_llc_coherency.c` (SYSREG_G3D `0x18420400` COHERENCY reg) is not built [V].
- So: **no `dma-coherent`**. Panfrost then picks `COHERENCY_NONE` (`panfrost_gpu.c:276-277`).

---

## 2. Clocks

### Downstream topology [V]

`DS/drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c`:

- CMU_TOP `0x1a240000`:
  - `PLL_LOCKTIME_PLL_G3D` +0x008
  - `PLL_CON0_PLL_G3D` +0x140, plus CON2 at +0x148 (used by the PD sequence)
  - `CLK_CON_DIV_CLKCMU_G3D_SWITCH` +0x1870
  - `CLK_CON_GAT_GATE_CLKCMU_G3D_SWITCH` +0x208c (the switch clock comes from PLL_SHARED2 800 MHz and is used while PLL_G3D relocks)
- CMU_G3D `0x18400000`, size 0x8000 (`cmucal-sfr.c:22`):
  - `PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER` +0x100 (GPU core clock mux: OSC vs PLL_G3D)
  - `…G3D_BUS_USER` +0x120
  - `…G3D_SWITCH_USER` +0x140
  - `MUX_CLK_G3D_BUSD` +0x1000
  - `DIV_CLK_G3D_BUSD` +0x1800, `DIV_CLK_G3D_BUSP` +0x1804
  - gate `CLK_BLK_G3D_UID_GPU_IPCLKPORT_CLK` +0x2004 and many bus gates at +0x2000…0x205c
  - QCH regs at +0x30xx
- Downstream kbase never touches these gates or muxes directly. Its only clock call is `cal_dfs_set_rate(ACPM_DVFS_G3D, …)` / `cal_dfs_get_rate` (`DS/drivers/gpu/arm/exynos/backend/gpexbe_clock.c:50,77,82`). `dvfs_g3d` is an **ACPM vclk** (`acpm_dvfs_exynos9820.h:31`), so `cal_dfs_set_rate` → `exynos_acpm_set_rate()` IPC (`cal-if.c:57-66`). The **APM firmware reprograms PLL_G3D and the G3D buck voltage itself**, using fused ASV tables.
- Downstream refuses to change the G3D rate while the PD is off: "can't set clock and voltage in the power-off state" (`DS/drivers/gpu/arm/exynos/frontend/gpex_clock.c:362-364`).

DVFS levels (kHz) from `exynos9820-mali_common.dtsi:29-38`: 702000, 650000, 572000, 433000, 377000, 325000, 260000, 200000, 156000.
- start 260000, max 702000, min 156000.
- `gpu_default_voltage` 800000 µV (`:47`).

### Mainline tree [V]

- `ML/drivers/clk/samsung/clk-exynos9820.c:1904-1920`: CMU_G3D exists (`samsung,exynos9820-cmu-g3d`, `:3825`) but **has no clocks**. It only does auto-clock-gating plus the DRCG sysreg write.
- `fout_pll_g3d` is registered in CMU_TOP (`:3154`, pll_0818x), as are `dout/gout_cmu_g3d_switch` (`:3503`, `:3652`).
- `ML/drivers/clk/samsung/clk-acpm.c:45-72,97-113` + `ML/drivers/firmware/samsung/exynos-acpm.c:885-898`: ACPM DVFS clocks for 9820, with `g3d` = index 10 (`EXYNOS9820_CLK_ACPM_DVFS_G3D`).
  - `recalc_rate` returns a **cached** value. The DT comment says FREQ_GET is broken (UB `exynos9820.dtsi:528-566`).
  - `set_rate` → ACPM IPC.
  - Boot default assigned: G3D = **156 MHz**.
- On the phone [V]:
  - `clk_summary` shows `g3d 156000000` (ACPM, cached) and `fout_pll_g3d 156000000`.
  - The PLL value is computed from the real CMU_TOP register, and a direct read of `0x1a240140 = 0xa0600412` agrees: ENABLE=1, LOCKED=1, MUX_SEL(bit4)=1.
  - `PLL_CON2 0x1a240148 = 0x10000001`, which is the value the downstream *off* sequence writes. The on sequence writes `0x30000003`. Meaning unknown [I].

### What the GPU node needs

`clocks = <&acpm_ipc EXYNOS9820_CLK_ACPM_DVFS_G3D>;`
- Panfrost does `devm_clk_get(dev, NULL)` + `clk_prepare_enable` (ML `panfrost_device.c:43-56`). The ACPM clock has no enable op, so that is a no-op. OK.
- A `fixed-clock` would also satisfy panfrost for Phase 1. The ACPM clock is better because devfreq can later call `clk_set_rate` on it.
- Do **not** point panfrost at `fout_pll_g3d`. `set_rate` on that PLL from Linux would race ACPM, which owns it.
- No "bus" clock is needed. The bus gates are auto/QCH-gated, and downstream never enables them.

---

## 3. Power

### PMU registers (PMU base `0x15860000`, always-on) [V source, V readback]

| Reg | Offset | Read now (G3D genpd "off") |
|---|---|---|
| EMBEDDED_G3D_CONFIGURATION | 0x2100 | `0x00000000` |
| EMBEDDED_G3D_STATUS | 0x2104 | `0x00000000` (off) |
| EMBEDDED_G3D (+8) | 0x2108 | `0x00000080` |
| EMBEDDED_G3D_OUT | 0x2120 | `0x00000008` |
| G3D_CONFIGURATION | 0x2380 | `0x00000001` |
| G3D_STATUS | 0x2384 | `0x00000001` (on) |
| (+8) | 0x2388 | `0x00000010` |

The downstream kbase DT confirms the status offset: `gpu_pmu_status_reg_offset = <0x2104>`, mask 0x1 (`mali_common.dtsi:40-41`).

### Downstream power-ON sequence [V]

`DS/drivers/soc/samsung/cal-if/exynos9820/flexpmu_cal_local_exynos9820.h:555-567`. This is executed **by the kernel** (pmucal RAE), not by ACPM.

```
1  W  0x1a240148 (PLL_CON2_PLL_G3D)          = 0x30000003
2  W  0x1a240140 (PLL_CON0_PLL_G3D) bit4     = 1          (mux → PLL)
3  W  0x15862380 (G3D_CONFIGURATION) bit0    = 0
4  WAIT 0x15862384 bit0 == 0
5  SET_BIT_ATOMIC 0x15862120 (EMBEDDED_G3D_OUT): write 2 to 0x1586e120   (PMU atomic-set alias offset|0xc000, value = bit index → sets bit 2)
6  udelay(280)
7  W  0x15862380 bit0 = 1 ; WAIT 0x15862384 bit0 == 1
8  udelay(280)
9  W  0x15862100 (EMBEDDED_G3D_CONFIGURATION) bit0 = 1
10 WAIT 0x15862104 bit0 == 1
```

Then, in `cal-if/pmucal_local.c:45-52`:
- `exynos_pd_tz_restore(0x18410204)`, which is `smc(0x82000410 SMC_CMD_PREAPRE_PD_ONOFF, 1 = EXYNOS_WAKEUP_PD_DOWN, 0x18410204, 2 = RUNTIME_PM_TZPC_GROUP)` (`DS/drivers/soc/samsung/exynos-el3_mon.c:239-245`, `include/linux/smc.h:42`; `need_smc` from `exynos9820-pm-domains.dtsi:77-83`).
- Then the CMU_G3D/SYSREG_G3D/HPM save-list is restored (`flexpmu_cal_local_exynos9820.h:569-584`).

Power-OFF (`:586-594`):
- disable the HPM QCH
- EMBEDDED_G3D_CONFIGURATION = 0, wait
- udelay(138)
- EMBEDDED_G3D_USER mux → OSC
- PLL_G3D mux → OSC
- PLL_CON2 = 0x10000001

Before the OFF, a TZPC **save** SMC (arg 0) runs (`pmucal_local.c:98-104`).

Notes:
- CMU_G3D is written *after* EMBEDDED_G3D is off in the off sequence. So CMU_G3D sits in the outer `G3D_CONFIGURATION` (0x2380) domain, which is still ON now, not in EMBEDDED_G3D [I].
- The GPU itself (0x18500000) is in EMBEDDED_G3D, which is **OFF now**. **Never read 0x18500000 on the current kernels.**

### Mainline [V]

- `ML/drivers/pmdomain/samsung/exynos-pm-domains.c:36-63,84-86,96`: `samsung,exynos9820-pd`, `local_pwr_cfg = 0x1`. It writes CONFIG and polls STATUS & 1. That is steps 9-10 only: no PLL/G3D_CONFIGURATION/OUT steps and **no TZPC SMC**. There is no TZPC/`0x82000410` code anywhere in ML (grep).
- UB `exynos9820.dtsi:1129-1134`: `pd_embedded_g3d: power-domain@15862100 { compatible = "samsung,exynos9820-pd"; label = "G3D"; }`.
- Phone [V]:
  - `pm_genpd_summary` lists `G3D off-0`.
  - `/sys/kernel/debug/pm_genpd/G3D/active_time` = 656 ms (boot #6) and 1508 ms (boot #7), with idle-state usage 1.
  - So the domain **was on when Linux started** (handed over by S-Boot/U-Boot), and `genpd: Disabling unused power domains` (dmesg 0.7185 s) switched it off with a bare `0 → 0x15862100`, with no TZPC save. G2D and AUD show the same thing (656 ms).

### Regulator [V source]

- Downstream S2MPS19 bucks `BUCK1M…BUCK12M` are all `regulator-always-on` and have no consumers (`DS/arch/arm64/boot/dts/samsung/exynos9820-beyond0lte_common.dtsi:1334+`, e.g. buck5/buck8 450-950 mV).
- The G3D rail voltage is programmed by ACPM during G3D DVFS. No Linux driver ever sets it.
- Mainline describes the S2MPS19 under ACPM (`firmware/power-management/pmic-1`, only `ldo19m`, `ldo27m` on the phone).
- So **no `mali-supply`** is needed. Panfrost's `devm_regulator_bulk_get("mali")` gets the dummy regulator, and the OPP core's optional lookup returns -ENODEV, which panfrost tolerates (`panfrost_devfreq.c:143-151`).
- The rail is on at boot [I: always-on, and the bootloader leaves G3D powered].

### Can it be done from Linux alone, without ACPM?

- **Power**: yes in principle. The whole on/off sequence is plain PMU/CMU_TOP MMIO done by the kernel downstream. But it **also needs the EL3 SMC** for the TZPC. That is EL3 firmware (present on this phone, since Samsung's monitor serves it), not ACPM.
- **Clock/voltage**: the rate and voltage need ACPM. The fixed boot rate (156 MHz, ACPM-set) needs nothing further. Higher clocks go through the existing ACPM DVFS clock.
- **Without** the SMC, the risk is that a GPU access after any PD cycle hits a secure-only TZPC → SError → hang [I].

---

## 4. Panfrost in this tree [V]

- Compatibles: `"arm,mali-bifrost"` → `default_data` (ML `panfrost_drv.c:1172`, `:1074-1079`): one optional PM domain, supply "mali", no special quirks.
- Add the SoC compatible `"samsung,exynos9820-mali", "arm,mali-bifrost"`. For dtbs_check it must also go into `Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml`. No driver change is needed.
- Required: `reg`, 3 `interrupts` + names, `clocks`.
- Optional: `power-domains`, `resets` (`panfrost_device.c:22-30`, optional), `operating-points-v2`, `#cooling-cells`, `dma-coherent` (do not set it).
- Runtime PM: panfrost always enables runtime PM with a 50 ms autosuspend (`panfrost_drv.c:991-995`). With `power-domains` set, genpd would then power G3D off/on every ~3 idle frames through the mainline sequence, which lacks the TZPC SMC. **This is why Phase 1 must not set `power-domains`.**
- devfreq:
  - No OPP table → "continue without devfreq" (`panfrost_devfreq.c:153-158`), so the GPU stays at the ACPM boot rate.
  - With an OPP table and no regulator, `dev_pm_opp_set_rate` → `clk_set_rate(acpm g3d)` → ACPM changes PLL and voltage. OPP rates must be the ECT levels listed above [I: ACPM rounds/rejects others; clk-acpm `determine_rate` passes the rate through].
- Kconfig: `CONFIG_DRM_PANFROST=m` is needed. IOMMU_SUPPORT, PM_DEVFREQ and SIMPLE_ONDEMAND are already `y` in `build-k/.config` [V].

---

## 5. Display integration (simpledrm KMS + panfrost render)

Phone facts [V]:
- Debian forky/sid.
- Mesa **26.2.4-1** (`libgl1-mesa-dri`, `libegl-mesa0`, `libgbm1`, `mesa-vulkan-drivers`, `mesa-libgallium`).
- `/usr/lib/aarch64-linux-gnu/dri/panfrost_dri.so` exists (→ libdril). There is **no** `simpledrm_dri.so`.
- phoc **0.58.0-1+neo2** (wlroots built in; it is not a shared lib).
- phosh 0.58.0-1+neo17.
- `/dev/dri/card0` (simpledrm) only.

The journal of the current phoc start shows why it renders in software today:
- `Initializing DRM backend for /dev/dri/card0 (simpledrm)`
- `Software rendering detected` (EGL on card0 → kms_swrast)
- `Could not match drm and vulkan device`
- `Creating pixman renderer`

wlroots' renderer picks the backend's DRM fd (card0). Mesa maps driver name "simpledrm" to no hardware driver, and kmsro does not cover simpledrm.

Cleanest setup [I, but every piece checked]:

1. **Primary plan: point the renderer at the panfrost render node.**
   - Set `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128` (the string is in `/usr/bin/phoc` [V]).
   - Set it in `/usr/bin/phosh-session`'s environment. That script already exports `WLR_BACKENDS` (`:58-59`). Alternatively use a systemd drop-in `phosh.service.d/gpu.conf` with `Environment=WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128`.
   - wlroots then creates the GLES2 renderer and a GBM allocator on panfrost. The DRM backend stays on simpledrm.
   - Scanout buffers are allocated on panfrost as LINEAR, since simpledrm planes only offer linear. They are PRIME-imported into simpledrm: `DRM_GEM_SHMEM_DRIVER_OPS` → `drm_gem_shmem_prime_import_no_map` (ML `simpledrm.c:832`, `include/drm/drm_gem_shmem_helper.h:300-302`) [V].
   - simpledrm `dma_buf_vmap`s the buffer for its damage blit into the real framebuffer at 0xca000000.
   - Clients follow wlroots' linux-dmabuf feedback main device (renderD128) and use panfrost for EGL.
2. **Cost/caveat** [I]:
   - Panfrost BOs are write-combined when non-coherent. simpledrm's CPU blit therefore reads uncached memory: up to 1080×2280×4 ≈ 9.8 MB per full-screen damage. That is probably 10-20 ms per full frame, still far better than pixman/llvmpipe compositing.
   - If PRIME import into simpledrm fails, the phoc log shows an `addFB2`/`drmPrimeFDToHandle` error. The fallback is `WLR_RENDERER=pixman` (today's path), with clients still GPU-accelerated through panfrost, since they get dmabufs and wlroots pixman copies them.
3. **GTK4/libadwaita apps**: use `GSK_RENDERER=ngl` until panvk on Bifrost v7 is proven. Otherwise GSK may pick Vulkan and end on llvmpipe-vk or a partial panvk [I].
4. **Permissions**: `user` is in `video` but not `render`. Debian's udev makes `renderD*` 0666 (group render) [I, check `ls -l /dev/dri/renderD128` once it exists].
5. **Not recommended**: a fake `simpledrm_dri.so` → kmsro symlink. Mesa's kmsro is name-listed at build time, and the megadriver loader would not honour a symlink alone [I].

Mesa 26.2 panfrost supports Bifrost v7 (G76) GLES 3.1+ [I, general knowledge: in Mesa since 21.x].

---

## 6. Bring-up plan

### Phase 0: pre-checks (read-only; nothing to change)
- Keep `/root/peek.py` away from `0x18400000`–`0x1850ffff` on current kernels.
- PMU `0x1586xxxx` and CMU_TOP `0x1a24xxxx` were read safely here. Note: the ssh session dropped after reading `0x1a240148`. The phone stayed up; uptime was continuous until an unrelated, orderly `systemd-reboot` at 02:23:54 into kernel #7 built at 02:22. The read of `0x1a24014c` in the same batch printed nothing. Treat CMU_TOP offsets beyond the documented ones as untested.

### Phase 1: GPU left powered by the bootloader, fixed 156 MHz, no devfreq

1. Kernel: `CONFIG_DRM_PANFROST=m`. Build into the Mobian modules package as usual.
2. DT (board dts or dtsi), changing `pd_embedded_g3d` so genpd cannot switch G3D off:
   ```dts
   &pd_embedded_g3d { status = "disabled"; };   /* keep bootloader state: ON */
   ```
   Alternative with no DT change: kernel cmdline `pd_ignore_unused`. That keeps *every* boot-on domain on (G2D and AUD too).
3. Add the GPU node inside `soc@0` (UB `exynos9820.dtsi`):
   ```dts
   gpu: gpu@18500000 {
       compatible = "samsung,exynos9820-mali", "arm,mali-bifrost";
       reg = <0x18500000 0x5000>;
       interrupts = <GIC_SPI 288 IRQ_TYPE_LEVEL_HIGH>,
                    <GIC_SPI 287 IRQ_TYPE_LEVEL_HIGH>,
                    <GIC_SPI 289 IRQ_TYPE_LEVEL_HIGH>;
       interrupt-names = "job", "mmu", "gpu";
       clocks = <&acpm_ipc EXYNOS9820_CLK_ACPM_DVFS_G3D>;
       /* NO power-domains: see spec §3/§4 (TZPC SMC missing in mainline pd) */
       /* NO dma-coherent: downstream runs COHERENCY_NONE */
       #cooling-cells = <2>;
       status = "okay";
   };
   ```
   (If the soc node uses 1 address cell, `reg` is as written. Match the neighbouring nodes.)
4. First boot, **before** `modprobe panfrost`:
   - Confirm `cat /sys/kernel/debug/pm_genpd/pm_genpd_summary` no longer lists G3D as off. If the node is disabled it is not listed at all.
   - Confirm `peek.py 15862104` → `…1` (EMBEDDED_G3D on) and `15862384` → `…1`.
   - Only then is reading `0x18500000` (GPU_ID) a reasonable test. Even then the TZPC is assumed NS from boot. Expect `0x7001_0000`-ish (r0p0).
5. `modprobe panfrost`. Expect `clock rate = 156000000`, `mali-g76 id 0x7001 major 0x0 minor 0x0 status 0x0`, the features/shader present mask (MP12 → 12 bits), and `/dev/dri/renderD128`. Then run `kmscube -D /dev/dri/renderD128`-style or `eglinfo -B` / `glmark2-es2-drm` offscreen.
6. phoc: `WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128` (+ `GSK_RENDERER=ngl` for apps), and restart phosh.

### Phase 2: frequency scaling (still with the PD pinned on)

- Add `operating-points-v2` with the ECT levels and **no** `opp-microvolt` (ACPM applies the voltage):
  ```dts
  gpu_opp_table: opp-table-gpu {
      compatible = "operating-points-v2";
      opp-156000000 { opp-hz = /bits/ 64 <156000000>; };
      opp-200000000 { opp-hz = /bits/ 64 <200000000>; };
      opp-260000000 { opp-hz = /bits/ 64 <260000000>; };
      opp-325000000 { opp-hz = /bits/ 64 <325000000>; };
      opp-377000000 { opp-hz = /bits/ 64 <377000000>; };
      opp-433000000 { opp-hz = /bits/ 64 <433000000>; };
      opp-572000000 { opp-hz = /bits/ 64 <572000000>; };
      opp-650000000 { opp-hz = /bits/ 64 <650000000>; };
      opp-702000000 { opp-hz = /bits/ 64 <702000000>; };
  };
  ```
- Start by capping at 433-572 MHz until the G3D TMU (0x100A0000, `exynos9820-tmu.dtsi`) has a cooling map to `&gpu`. Downstream throttles 650→156 MHz over 6 temperature steps (`mali_common.dtsi`, `gpu_temp_throttling`).
- Panfrost devfreq only calls set_rate while the device is active, which matches the downstream rule "no rate change while powered off".

### Phase 3: real runtime power gating (needs driver work)

- Extend `exynos-pm-domains.c` with a 9820-G3D variant, or write a small `exynos9820-g3d-pd`, implementing the §3 on/off sequences (PLL_CON2/CON0, G3D_CONFIGURATION toggle, OUT atomic-set via `+0xc000`, delays), the CMU_G3D save/restore list (`flexpmu_cal_local_exynos9820.h:569-584`), and the EL3 SMC `0x82000410` (save before off with arg 0, restore after on with arg 1; addr `0x18410204`, group 2).
- Also give `cmu_g3d` a `power-domains` link so its auto-clock-gate setup is redone after a cycle.
- Then add `power-domains = <&pd_embedded_g3d>` to the GPU node.

### Risk points (what can hang or kill the SoC)

1. **Any CPU access to 0x18500000 while EMBEDDED_G3D is off** → synchronous external abort / SError. On every current boot the PD is already off after 0.72 s.
2. **TZPC not NS after a power cycle** (mainline pd driver, no SMC) → the first panfrost register access aborts. Phase 1 avoids any cycle. Never `echo` the genpd on/off by hand, and never bind/unbind a PD consumer.
3. **G3D power cycled with the mainline 1-step sequence**: G3D_CONFIGURATION is not toggled, PLL_CON2 is not set and OUT bit2 is not set, so the GPU may come up unclocked or still in reset → GPU register reads hang the bus [I].
4. **Rate change via ACPM while the PD is off** (downstream forbids it): APM firmware behaviour is unknown, and it could stall ACPM IPC, which also serves CPU and MIF DVFS.
5. **`clk_set_rate` on `fout_pll_g3d` from Linux** (races APM). Do not use that clock in the GPU node.
6. **High OPP without thermal**: 702 MHz on 12 cores with no cooling map. Set the cap first.
7. Unknown G3D bus QoS/BTS settings (downstream `CONFIG_MALI_EXYNOS_BTS_MO=y`) are only a performance issue, not a hang [I].

Mitigation for bring-up: boot with `panic=0` (already on the cmdline) so a crash halts instead of looping, and keep panfrost as a module so the first touch of the GPU is a manual `modprobe` over ssh.

---

## 7. Open questions

- The meaning of the PLL_CON2 values `0x30000003` / `0x10000001`, and the meaning of EMBEDDED_G3D_OUT bit 2 / bit 3 (reads 0x8 now).
- Whether the EL3 monitor's TZPC "restore" works without a prior "save" (mainline never saved). It matters for Phase 3 and for recovery after genpd has already cut the domain.
- Whether S-Boot leaves the G3D TZPC in NS state at hand-off. Downstream kbase probes without an SMC while the PD is still bootloader-on, which suggests yes [I].
- The exact S2MPS19 buck number for G3D (irrelevant while ACPM owns it).
- How fast simpledrm blits from an imported WC buffer, measured on the device.
