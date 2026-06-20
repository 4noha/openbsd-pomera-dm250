#!/bin/sh
# pomera-lid-watch (machdep.lidaction=0 prerequisite):
#   lid close  -> backlight off. SoC/SD keep running so resume is instant.
#   closed 2h  -> full suspend (deep sleep) to save battery.
#   lid open   -> restore backlight (if suspended, lid-open wakes -> SD re-enum ~53s).
SENSOR=hw.sensors.gpiokeys0.indicator0
STATE=/var/run/lid-brightness
DEFBR=50
SUSPEND_AFTER=7200
raw=$(sysctl -n "$SENSOR" 2>/dev/null)
case "$raw" in On*) last=open ;; Off*) last=closed ;; *) last="" ;; esac
t_close=0
[ "$last" = closed ] && t_close=$(date +%s)
while :; do
  raw=$(sysctl -n "$SENSOR" 2>/dev/null)
  case "$raw" in On*) cur=open ;; Off*) cur=closed ;; *) sleep 2; continue ;; esac
  if [ "$cur" != "$last" ]; then
    if [ "$cur" = closed ]; then
      b=$(wsconsctl display.brightness 2>/dev/null | sed 's/.*=//; s/%//')
      case "$b" in [0-9]*) echo "$b" > "$STATE" ;; esac
      wsconsctl display.brightness=0 >/dev/null 2>&1
      t_close=$(date +%s)
    else
      b=$(cat "$STATE" 2>/dev/null); case "$b" in [0-9]*) : ;; *) b=$DEFBR ;; esac
      wsconsctl display.brightness="$b" >/dev/null 2>&1
      t_close=0
    fi
    last=$cur
  fi
  if [ "$cur" = closed ] && [ "$t_close" -gt 0 ]; then
    now=$(date +%s)
    if [ $((now - t_close)) -ge $SUSPEND_AFTER ]; then
      t_close=0
      /etc/pomera-suspend >/dev/null 2>&1
      last=""
    fi
  fi
  sleep 2
done
