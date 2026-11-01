#!/bin/sh
# Fake Minecraft server used by host tests: reads stdin commands, writes logs.
echo "fake server starting"
while IFS= read -r line; do
  case "$line" in
    stop) echo "stopping"; exit 0 ;;
    save-all*) echo "saved the game" ;;
    save-on) echo "saving enabled" ;;
    save-off) echo "saving disabled" ;;
    *) echo "got: $line" ;;
  esac
done
