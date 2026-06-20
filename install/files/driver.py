#!/usr/bin/env python3
"""Drive qemu-system-aarch64 via UNIX serial socket to prep DM250 install SD.

Connects to /tmp/dm250-qemu-serial.sock (set up by the launcher script),
waits for the OpenBSD installer welcome prompt, drops to shell, runs
prep-sd.sh from the host HTTP server, then halts the VM.
"""

import socket
import sys
import threading
import time

SOCK = "/tmp/dm250-qemu-serial.sock"

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(SOCK)
s.settimeout(1.0)

buf = bytearray()
buf_lock = threading.Lock()
done = threading.Event()


def reader():
    while not done.is_set():
        try:
            data = s.recv(4096)
            if not data:
                break
            with buf_lock:
                buf.extend(data)
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        except socket.timeout:
            continue
        except OSError:
            break


t = threading.Thread(target=reader, daemon=True)
t.start()


def expect(pattern: str, timeout: float = 300.0) -> None:
    """Wait until `pattern` appears in the buffer (and consume up to it)."""
    pat = pattern.encode()
    deadline = time.time() + timeout
    while time.time() < deadline:
        with buf_lock:
            idx = buf.find(pat)
            if idx >= 0:
                del buf[: idx + len(pat)]
                return
        time.sleep(0.5)
    raise TimeoutError(f"expected {pattern!r} (last 200 bytes: {bytes(buf[-200:])!r})")


def send(line: str) -> None:
    s.send(line.encode())
    sys.stdout.write(f"\n>>> sent: {line!r}\n")
    sys.stdout.flush()


try:
    # 1. Welcome prompt
    print("\n[driver] waiting for OpenBSD installer welcome prompt...")
    expect("(I)nstall, (U)pgrade, (A)utoinstall or (S)hell?", timeout=300)
    time.sleep(2)
    send("s\n")

    # 2. Shell prompt
    print("\n[driver] waiting for shell prompt...")
    expect("# ", timeout=60)

    # 3. Bring up vio0 and configure DHCP. The install ramdisk does not start
    # dhcpleased automatically; we need to do it ourselves. autoconf-up triggers
    # the kernel's DHCP autoconf path via dhcpleased(8); fall back to running
    # dhcpleased in foreground/background if needed.
    print("\n[driver] bringing up vio0 and requesting DHCP...")
    send("ifconfig vio0 up; ifconfig vio0 inet autoconf\n")
    expect("# ", timeout=10)
    # Start dhcpleased (the install ramdisk has it) explicitly.
    send("dhcpleased >/tmp/dhcp.log 2>&1 &\n")
    expect("# ", timeout=10)

    # 4. Poll until vio0 gets a 10.0.2.x address (qemu user-mode networking).
    DHCP_OK_TAG = "DHCP_OK_TAG_4n0h4"
    for attempt in range(15):
        send("ifconfig vio0 | grep -q 'inet 10\\.0\\.2' && echo " + DHCP_OK_TAG + "\n")
        try:
            expect(DHCP_OK_TAG, timeout=4)
            expect("# ", timeout=4)
            print(f"\n[driver] vio0 DHCP succeeded after attempt {attempt + 1}")
            break
        except TimeoutError:
            expect("# ", timeout=4)
            print(f"\n[driver] vio0 not yet up, retrying ({attempt + 1}/15)")
            time.sleep(2)
    else:
        # Last resort: show full state for debugging and bail.
        send("ifconfig vio0; cat /tmp/dhcp.log; route -n get default\n")
        expect("# ", timeout=10)
        raise RuntimeError("vio0 never got a DHCP address")

    # 5. Fetch prep script from host HTTP server.
    print("\n[driver] fetching prep-sd.sh...")
    send("ftp -V -o /tmp/prep-sd.sh http://10.0.2.2:8000/prep-sd.sh\n")
    expect("# ", timeout=60)

    # 6. Run prep-sd.sh non-interactively.
    print("\n[driver] running prep-sd.sh (this takes several minutes)...")
    send("YES=1 SD=sd1 sh /tmp/prep-sd.sh\n")

    # 7. Wait for the script's success sentinel (could take 5-15 min).
    expect("SD-PREP-COMPLETE", timeout=2400)
    print("\n[driver] prep complete; halting VM...")

    # 8. Shell back, halt.
    expect("# ", timeout=30)
    send("halt -p\n")
    time.sleep(15)

    print("\n[driver] done.")
except Exception as e:
    print(f"\n[driver] ERROR: {e!r}", file=sys.stderr)
    sys.exit(2)
finally:
    done.set()
    try:
        s.close()
    except OSError:
        pass
