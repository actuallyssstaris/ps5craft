#!/usr/bin/env bash
# deploy.sh - Install PS5Craft title onto a jailbroken PS5 console.
#
# Tries, in order:
#   1. elfldr HTTP upload (console port 9021) - preferred, installs the title
#   2. FTP STOR (console port 2121) - usually read-only on stock ftpsrv.elf
# If both fail, prints the manual steps.
#
# Usage:
#   CONSOLE=192.168.2.15 ./deploy.sh
# or
#   ./deploy.sh   # defaults to 192.168.2.15

set -u

CONSOLE="${CONSOLE:-192.168.2.15}"
TITLE_DIR="dist/PPSA90104"
EBoot="$TITLE_DIR/eboot.bin"

title_id="PPSA90104"

have() { command -v "$1" >/dev/null 2>&1; }

echo "== PS5Craft deploy to $CONSOLE =="
echo "Title: $title_id"
echo "Artifact: $TITLE_DIR/"
ls -l "$EBoot" 2>/dev/null || { echo "ERROR: $EBoot not found. Run 'make app' first." >&2; exit 1; }
echo

# ---------- 1. elfldr ----------
if curl -s -m 5 -o /dev/null "http://$CONSOLE:9021/" 2>/dev/null; then
    echo "[1/2] elfldr $CONSOLE:9021 is up - uploading eboot.bin"
    code=$(curl -s -o /tmp/elfldr.resp -w "%{http_code}" \
        -X POST -F "file=@$EBoot" "http://$CONSOLE:9021/" 2>&1)
    echo "   HTTP $code"
    cat /tmp/elfldr.resp 2>/dev/null | head -5
    echo
    if [ "$code" = "200" ] || [ "$code" = "201" ]; then
        echo "OK: title uploaded via elfldr. Launch $title_id from the homebrew launcher."
        exit 0
    fi
else
    echo "[1/2] elfldr $CONSOLE:9021 is down - skipping"
fi

# ---------- 2. FTP ----------
echo "[2/2] FTP $CONSOLE:2121 - testing write access"
python3 - "$CONSOLE" "$EBoot" "$TITLE_DIR" <<'PY'
import sys, os, io
from ftplib import FTP
host, eboot, title_dir = sys.argv[1], sys.argv[2], sys.argv[3]
ftp = FTP()
try:
    ftp.connect(host, 2121, timeout=10)
    ftp.login('anonymous', 'anonymous')
except Exception as e:
    print("   FTP connect/login failed:", e)
    sys.exit(1)

# Try to enter /data/homebrew/PPSA90104
try:
    ftp.cwd('/data/homebrew')
except Exception as e:
    print("   cwd /data/homebrew failed:", e)
    sys.exit(1)

# Create the title dir if missing
try:
    ftp.mkd('PPSA90104')
    print("   created /data/homebrew/PPSA90104")
except Exception as e:
    print("   mkd (may already exist):", e)

try:
    ftp.cwd('/data/homebrew/PPSA90104')
except Exception as e:
    print("   cwd into title dir failed:", e)
    sys.exit(1)

# List current contents (best effort)
try:
    print("   contents:", ftp.nlst())
except Exception:
    print("   nlst not supported by this server")

# Try STOR on eboot.bin
with open(eboot, 'rb') as f:
    try:
        resp = ftp.storbinary(f'STOR eboot.bin', f)
        print("   STOR eboot.bin:", resp)
    except Exception as e:
        print("   STOR eboot.bin FAILED:", e)
        print("   -> FTP is read-only; cannot install via FTP.")
ftp.quit()
PY

echo
echo "===== MANUAL DEPLOY (FTP read-only / elfldr down) ====="
echo "1. Copy the whole folder dist/PPSA90104/ to the console:"
echo "      /data/homebrew/PPSA90104/   (or /data/PlayKit/PPSA90104/)"
echo "   Files: eboot.bin, sce_sys/param.json, sce_module/libc.prx, assets/ca-bundle.crt"
echo "   (FTP to console is read-only; use the homebrew launcher / file manager.)"
echo ""
echo "2. Place the CA bundle so catalog HTTPS works:"
echo "      assets/ca-bundle.crt  ->  <data root>/ca-bundle.crt"
echo "   or set PS5CRAFT_CA_BUNDLE before launching."
echo ""
echo "3. Launch PPSA90104 from the homebrew launcher."
echo "   The backend listens on port 8080 by default."
echo ""
echo "4. From another machine: http://<console-ip>:8080/"
echo "   Catalog -> Vanilla -> 26.3 -> Install -> Start"
echo "   Verify: Minecraft client connects to <console-ip>:25565"
echo "========================================================"
