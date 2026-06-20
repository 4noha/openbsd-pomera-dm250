<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# wake-android-tether — Android-side BT mux server

The pomera DM250 (running OpenBSD + [`panctl/`](../panctl/)) connects to
an Android phone over Bluetooth RFCOMM and tunnels all of its outbound
TCP/UDP through a single mux frame stream. This subtree is the
**Android side** of that pair: a Kotlin app that accepts the RFCOMM
connection, demuxes the frames, opens the requested sockets on the
phone's cellular link, and forwards bytes back to pomera.

```
+------------------+  RFCOMM (single BT channel)   +----------------+
| pomera (panctl)  | <===========================> | Android        |
|  BTstack client  |     mux v0 frames             |  RfcommService |
|  pf divert       |                               |  MuxServer     |
+--------+---------+                               +--------+-------+
         |                                                  |
         | outbound TCP/UDP                                 | TCP/UDP on
         | (Tailscale, ssh, http, ...)                      | cell radio
         v                                                  v
   (intercepted by pf)                              (phone's cellular APN)
```

The wire protocol lives in [`../panctl/PROTOCOL.md`](../panctl/PROTOCOL.md)
(mux v0 draft). The design rationale and the path NOT taken (Samsung MDX,
Google Instant Tethering, Shizuku-based hidden API…) are in
[`../panctl/PLANS.md`](../panctl/PLANS.md).

> [!CAUTION]
> **Alpha / WIP.** This app is published so the public install flow has
> a referenceable companion and so reviewers can read the Android side
> alongside [`panctl/`](../panctl/). **The two halves do not yet
> exchange a single working mux frame.** Per
> [`panctl/STATUS.md`](../panctl/STATUS.md) the project is at:
>
> - Pomera side: BTstack RFCOMM client smoke test passes; main `panctl/`
>   binary is still skeleton. Mux frame encode/decode not wired into the
>   pf divert path yet.
> - Android side: this app's skeleton (RfcommService + MuxServer +
>   Frame{,Writer} + TcpStream / UdpStream) is in place but **E2E echo
>   has not been demonstrated**. Pairing flow works; the mux frame
>   handshake hasn't been exercised against panctl yet.
>
> If you came here expecting a working tether: please wait for a release
> tagged ≥ `v1.0-beta`. Until then build, sideload, and read the source
> only.

## Required Android features

- **Android 12 (SDK 31) or later**. Tested on Pixel / Samsung Galaxy /
  Xiaomi. No carrier-specific hooks; no Samsung MDX, no Shizuku, no root.
- Runtime permissions: `BLUETOOTH_CONNECT` / `BLUETOOTH_ADVERTISE` /
  `BLUETOOTH_SCAN` (for RFCOMM accept + SDP), `INTERNET` (outbound on
  cell), `FOREGROUND_SERVICE` + `FOREGROUND_SERVICE_CONNECTED_DEVICE` +
  `POST_NOTIFICATIONS` (persistent listener), `RECEIVE_BOOT_COMPLETED`
  (auto-restart after reboot).
- BT pairing with pomera-side `panctl` over Secure Simple Pairing /
  Secure Connections. The first contact is via Settings → Bluetooth on
  the phone, after which `BtConnectReceiver` auto-starts `RfcommService`
  on every subsequent `ACL_CONNECTED` broadcast from pomera.

## Source layout

```
wake-android-tether/
├── README.md, build.gradle.kts, settings.gradle.kts, gradle.properties
├── gradle/wrapper/                  # gradle 8.5+ wrapper
├── gradlew / gradlew.bat
└── app/
    ├── build.gradle.kts
    └── src/main/
        ├── AndroidManifest.xml
        ├── java/com/fournoha/wakeandroidtether/
        │   ├── ui/MainActivity.kt          # toggle the listener, show state
        │   ├── bt/
        │   │   ├── Protocol.kt             # RFCOMM SDP UUID + mux version
        │   │   ├── RfcommService.kt        # foreground service, accept loop
        │   │   ├── BtConnectReceiver.kt    # ACL_CONNECTED -> start service
        │   │   └── BluetoothStateReceiver.kt
        │   ├── mux/
        │   │   ├── Frame.kt                # binary frame encode/decode
        │   │   ├── FrameWriter.kt
        │   │   ├── MuxServer.kt            # per-conn demux + stream tracker
        │   │   ├── TcpStream.kt            # mux <-> Socket
        │   │   └── UdpStream.kt            # mux <-> DatagramSocket
        │   ├── boot/BootReceiver.kt        # respawn listener after reboot
        │   └── prefs/Prefs.kt              # master toggle + tunables
        └── res/{layout, values}/           # minimal — toggle + status text
```

No launcher icon resources are bundled; OS-default is used (this is an
alpha build aimed at sideloaders, not Play). If you want a custom icon,
drop one under `app/src/main/res/mipmap-*/` and add
`android:icon="@mipmap/ic_launcher"` to the `<application>` tag.

## Download (sideload-ready APK)

Each tagged build is also published as a GitHub Release with the
debug-signed APK attached:

- **Latest pre-release**: [`wake-android-tether-v0.3.0`](https://github.com/4noha/openbsd-pomera-dm250/releases/tag/wake-android-tether-v0.3.0)
- All releases: <https://github.com/4noha/openbsd-pomera-dm250/releases>

Sideload directly without building:

```bash
gh release download wake-android-tether-v0.3.0 \
  --repo 4noha/openbsd-pomera-dm250 \
  --pattern '*.apk' \
  --output wake-android-tether.apk
adb install wake-android-tether.apk
```

Verify the SHA-256 against the value listed in the release notes
before installing.

## Build

Standard Gradle. Wrapper JAR is checked in.

```bash
# on mac / linux
cd wake-android-tether
./gradlew :app:assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk

# install on a phone over USB (ADB)
adb install app/build/outputs/apk/debug/app-debug.apk
```

For release builds you'll need your own signing config — none is bundled
(no shared keystore, no Play, no Firebase App Distribution). See the
upstream Android docs for `signingConfigs {}` in
`app/build.gradle.kts`.

## Usage (once the app is installed)

1. Launch the app. Grant the BT + notification permissions when asked.
2. Toggle the "listener enabled" switch on. The persistent notification
   shows "Listening on BT RFCOMM (mux)…".
3. Pair the phone with the pomera (`bluetoothctl scan on` from pomera, or
   Settings → Bluetooth on the phone — both routes work because pomera-side
   panctl advertises with `auto_accept` during the pairing window). See
   [`../install/06-thinclient.md`](../install/06-thinclient.md) §6.2 for
   the pomera-side pairing runbook.
4. From pomera, start `panctl` (`doas rcctl start panctl`). On the phone,
   the notification flips to "Mux up with <pomera-bdaddr>".

If step 4 stops at "Listening…" with no transition, you've hit the
current limit — E2E echo is still being worked on (see
[`../panctl/STATUS.md`](../panctl/STATUS.md) §「次フェーズ」 step 6).

## How this slot maps to the project's plan

This app implements **Axis 1-G** (Android-side custom RFCOMM service)
× **Axis 2-B** (mux protocol over a single RFCOMM channel) × **Axis 3-E**
(per-socket TCP/UDP demultiplex on the Android side) from
[`../panctl/PLANS.md`](../panctl/PLANS.md). The earlier `1-E` path
(Shizuku + hidden `TetheringManager.startTethering()`) was abandoned
after modern Android tightened the hidden API surface; the new path
needs no signature permissions and no Shizuku.

## License

MIT — see the repository top-level [`LICENSE`](../LICENSE). All `.kt`
and `.xml` files carry an inline `SPDX-License-Identifier: MIT` header.
