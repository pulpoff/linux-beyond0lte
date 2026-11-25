# s10e-sleepd: the Galaxy S10e sleeps like a phone

`s10e-sleepd` puts the phone to sleep (s2idle) a few seconds after the screen goes off, whenever nothing needs
it awake. Calls, SMS, a unicast packet on Wi-Fi, the power button, a double tap on the screen and the charger
wake it.

Mobian's own policy is a laptop's. gsd-power suspends after 15 idle minutes, through logind, and everything then
treats the resume as a lid opening: the shell turns the screen on, NetworkManager takes Wi-Fi down first. So this daemon
suspends the kernel itself, the way Android does. logind's suspend targets stay masked, and install.sh sets
gsd-power's `sleep-inactive-*-type` to `nothing`. No program hears about a sleep. A call that wakes the phone rings
on a screen that only the call turns on.

## When it stays awake

- **The screen is on.** It reads `org.gnome.ScreenSaver` on the active session's bus. With the AOD on, the panel
  stays lit, but the screen saver counts as active.
- **A logind `sleep` block inhibitor.** gsm2sipd takes one in gateway mode only when the phone's Wi-Fi cannot
  wake it (see below). With the Wi-Fi wake in place the gateway lets the phone sleep: an INVITE or the SIP
  server's OPTIONS wakes it like a call does.
- **An app inhibits suspend** through gnome-session (GtkApplication's inhibit).
- **A call** in ModemManager that has not ended.
- **Audio.** Any ALSA PCM running: music, a ringtone, call audio.
- **A kernel wakeup source is active.** Reading `/sys/power/wakeup_count` waits for it. These are cpd's `cpd-sms`
  (10 s per SMS) and `cpd-call` (through a call), and the modem driver (1 s per CP message).
- **After a wake.** It stays up for `wake_hold` s after anything but its own clock alarm woke it (`wifi_hold` s
  after a packet on Wi-Fi: a call or SMS it starts holds the phone by itself), and for the first
  `boot_grace` s after boot, while the CP makes its one-off file requests.

The panel keeps its last frame while the phone sleeps. With the AOD showing, an RTC alarm wakes the phone at every
minute for `aod_hold` s, so the shell can draw the new time. While the panel's own clock runs (the S6E3FA7's
`self_clock` reads `analog`: the panel ticks its hands by itself) the alarm comes once an hour instead, for the
date and the battery.

## Wi-Fi while asleep

The BCM4375 stays associated through a sleep and wakes the phone on its out-of-band host-wake line (gpa0-7), the
way Samsung's own driver runs it. That needs the kernel's brcmfmac with the host-wake patch and
`options brcmfmac host_wake=gpa0:7` (`/etc/modprobe.d/brcmfmac-hostwake.conf`). WoWLAN is then "any" by
default (`iw phy <phy> wowlan show`), and `/sys/class/net/wlp1s0/device/power/wakeup` reads `enabled`, which is
what gsm2sipd looks at. While the phone sleeps the dongle passes only unicast frames; ARP and ND are answered in
the firmware, and LAN broadcasts (HomePlug beacons every 2 s here) are dropped. A wake by Wi-Fi shows in the log
as IRQ 160 `brcmf_host_wake`. `iw phy <phy> wowlan disable` goes back to dropping Wi-Fi in sleep.

Timers that must fire while the phone sleeps need an alarm clock (`CLOCK_BOOTTIME_ALARM` /
`CLOCK_REALTIME_ALARM`); `CLOCK_MONOTONIC` and kernel timers stop. gsm2sipd arms one for its re-REGISTER.

## Settings

`/etc/s10e-sleepd.conf`, group `[sleep]`: `enabled`, `delay`, `wake_hold`, `wifi_hold`, `aod_clock`, `aod_hold`,
`boot_grace`, `poll` (the shipped file lists the defaults). `s10e.nosleep` on the kernel command line keeps it
awake, and so does stopping the service.

## Use

- Log: `journalctl -u s10e-sleepd`. Each wake is one line, saying how long it slept and which interrupt woke it.
- `pkill -USR1 -x s10e-sleepd` logs what holds it awake right now.
- Install: `make arm64 && ./install.sh [root@host]`.
- Test run over ssh: `systemctl stop s10e-sleepd; s10e-sleepd -v --for 300`. It sleeps only for the next 300 s
  (wall clock; an RTC alarm wakes it for the end) and stays awake after. A sleeping phone drops USB; over Wi-Fi an
  ssh session survives, and every packet of it wakes the phone.
