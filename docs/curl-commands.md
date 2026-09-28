# Curl commands

Every command the room's boards understand, ready to paste. Three ways in, from closest to the hardware to furthest:

| Way in | Run it from | Dashboard stays in step? |
| --- | --- | --- |
| 1. Straight to a board | The Pi, or anything on the room Wi-Fi | No |
| 2. Through the Pi's edge service | The Pi | No |
| 3. Through the website's API | Anywhere | Yes |

Use 3 for normal operation. Use 1 when testing wiring.

Set these once in the shell you are using:

```bash
STACK=192.168.8.192      # stack 1 bridge. Addresses move; see "Finding a board" below.
RELAY=192.168.8.197      # relay board: valves and AC 1
DHT1=192.168.8.198       # Zone 1 sensor board: exhaust fan
DHT4=192.168.8.199       # Zone 4 sensor board: AC 2
API=https://phenotype.103-25-130-37.nip.io/api/v1
```

## 1. Stack bridge, straight to the board

### Raw protocol reference

| You want to send | Curl |
| --- | --- |
| `path,start,end,r,g,b,brightness` | `/led?path=&start=&end=&r=&g=&b=&brightness=` |
| `3,speed` | `/pump?speed=` |

### Status and readings

```bash
# Everything: readings, last commands, line counters, signal, uptime
curl -s http://$STACK/status

# pH and pump current only. Answers 503 if the Arduino has sent nothing for 5 s.
curl -s http://$STACK/reading

# Is the Arduino talking? lines_ok should climb by 2 every second.
curl -s http://$STACK/status | python3 -c "import sys,json; d=json.load(sys.stdin); print({k:d[k] for k in ('arduino_online','lines_ok','lines_bad','ph','motor_voltage','commands_sent','restarts')})"
```

### Pump (path 3)

```bash
curl -s "http://$STACK/pump?speed=128"    # sends 3,128  on, the sketch's default speed
curl -s "http://$STACK/pump?speed=255"    # sends 3,255  full speed
curl -s "http://$STACK/pump?speed=0"      # sends 3,0    off
```

### LEDs, one row at a time

Colour below is warm white (255,255,200) at brightness 222. Change `r`, `g`, `b` (0-255) and `brightness` (0-255) as needed.

```bash
# Shelf 1
curl -s "http://$STACK/led?path=0&start=4&end=87&r=255&g=255&b=200&brightness=222"      # row 1  -> 0,4,87,...
curl -s "http://$STACK/led?path=0&start=104&end=190&r=255&g=255&b=200&brightness=222"   # row 2  -> 0,104,190,...
curl -s "http://$STACK/led?path=0&start=205&end=299&r=255&g=255&b=200&brightness=222"   # row 3  -> 0,205,299,...
curl -s "http://$STACK/led?path=2&start=1&end=83&r=255&g=255&b=200&brightness=222"      # row 4  -> 2,1,83,...

# Shelf 2
curl -s "http://$STACK/led?path=1&start=1&end=86&r=255&g=255&b=200&brightness=222"      # row 1  -> 1,1,86,...
curl -s "http://$STACK/led?path=1&start=105&end=189&r=255&g=255&b=200&brightness=222"   # row 2  -> 1,105,189,...
curl -s "http://$STACK/led?path=1&start=206&end=290&r=255&g=255&b=200&brightness=222"   # row 3  -> 1,206,290,...
curl -s "http://$STACK/led?path=2&start=106&end=188&r=255&g=255&b=200&brightness=222"   # row 4  -> 2,106,188,...
```

### LEDs, whole strip

`start` and `end` default to 0 and 299, `brightness` to 255.

```bash
curl -s "http://$STACK/led?path=0&r=0&g=0&b=255"       # strip 0 blue     -> 0,0,299,0,0,255,255
curl -s "http://$STACK/led?path=1&r=255&g=0&b=0"       # strip 1 red      -> 1,0,299,255,0,0,255
curl -s "http://$STACK/led?path=2&r=0&g=255&b=0"       # strip 2 green    -> 2,0,299,0,255,0,255

# Everything off
for p in 0 1 2; do curl -s "http://$STACK/led?path=$p&r=0&g=0&b=0&brightness=0" >/dev/null; done
```

### Wiring test: which strip is on which pin

Lights each strip a different colour, two seconds apart.

```bash
curl -s "http://$STACK/led?path=0&r=255&g=0&b=0&brightness=60" >/dev/null; sleep 2   # pin 13 red
curl -s "http://$STACK/led?path=1&r=0&g=255&b=0&brightness=60" >/dev/null; sleep 2   # pin 12 green
curl -s "http://$STACK/led?path=2&r=0&g=0&b=255&brightness=60" >/dev/null            # pin 11 blue
```

Brightness 60 keeps the current low. 900 LEDs at full white is tens of amps.

## 2. Through the Pi's edge service

Run on the Pi. The Pi looks the row up in its map and finds the board by MAC, so no addresses or LED numbers are needed.

```bash
EDGE=http://127.0.0.1:8090

# A row's LEDs. intensity is 0-100 %.
curl -s -X POST $EDGE/stack-led -H 'content-type: application/json' \
  -d '{"stack":1,"shelf":1,"row":1,"r":255,"g":255,"b":200,"intensity":87}'

# Row off (keeps nothing; the website is where colours are remembered)
curl -s -X POST $EDGE/stack-led -H 'content-type: application/json' \
  -d '{"stack":1,"shelf":1,"row":1,"r":0,"g":0,"b":0,"intensity":0}'

# Pump of stack 1
curl -s -X POST $EDGE/relay-proxy -H 'content-type: application/json' -d '{"channel":"pump1","state":true}'
curl -s -X POST $EDGE/relay-proxy -H 'content-type: application/json' -d '{"channel":"pump1","state":false}'

# Valves (the master valve opens and closes by itself)
curl -s -X POST $EDGE/relay-proxy -H 'content-type: application/json' -d '{"channel":"valve1","state":true}'
curl -s -X POST $EDGE/relay-proxy -H 'content-type: application/json' -d '{"channel":"valve1","state":false}'

# AC 1, AC 2, exhaust fan
curl -s -X POST $EDGE/relay-proxy -H 'content-type: application/json' -d '{"channel":"ac","state":true}'
curl -s -X POST $EDGE/relay-proxy -H 'content-type: application/json' -d '{"channel":"ac2","state":true}'
curl -s -X POST $EDGE/relay-proxy -H 'content-type: application/json' -d '{"channel":"exhaust","state":true}'

curl -s $EDGE/health
```

## 3. Through the website's API

Works from anywhere and keeps the dashboard in step. **Toggles flip the current state**, so read the state first.

Actuator ids:

| Device | Id |
| --- | --- |
| Stack 1 row LEDs, shelf 1 rows 1-4 | 7111, 7112, 7113, 7114 |
| Stack 1 row LEDs, shelf 2 rows 1-4 | 7121, 7122, 7123, 7124 |
| Stack 1 circulation pump | 8001 |
| Stack 1 / 2 / 3 water valve | 8101 / 8102 / 8103 |
| AC 1 / AC 2 / exhaust fan | 9001 / 9002 / 9003 |

```bash
# State of everything
curl -s $API/v2/actuators | python3 -c "import sys,json; [print(a['id'], a['name'], 'ON' if a['state'] else 'OFF', a['last_error'] or '') for a in json.load(sys.stdin) if a['id'] in (7111,7112,7113,7114,7121,7122,7123,7124,8001,8101,8102,8103,9001,9002,9003)]"

# Flip a device: row LED power, pump, valve, AC, fan
curl -s -X POST $API/v2/actuators/7111/toggle      # shelf 1 row 1 LEDs
curl -s -X POST $API/v2/actuators/8001/toggle      # pump
curl -s -X POST $API/v2/actuators/8101/toggle      # stack 1 valve
curl -s -X POST $API/v2/actuators/9001/toggle      # AC 1

# Set a row's colour. intensity is 0-100 %. Sent to the strip only while the row is on.
curl -s -X PUT $API/v2/actuators/7111/led -H 'content-type: application/json' \
  -d '{"r":255,"g":255,"b":200,"intensity":87}'

# Pump schedule: local farm time (UTC), minutes per run
curl -s -X PUT $API/v2/actuators/8001/schedule -H 'content-type: application/json' \
  -d '{"enabled":true,"starts":["06:00","18:00"],"run_minutes":360}'

# Turn the schedule off and keep manual control
curl -s -X PUT $API/v2/actuators/8001/schedule -H 'content-type: application/json' \
  -d '{"enabled":false,"starts":["06:00","18:00"],"run_minutes":360}'

# Latest tank pH for stack 1 (sensor 2201)
curl -s "$API/v2/sensors" | python3 -c "import sys,json; [print(s['name'], s.get('latest_value'), s.get('last_seen_at')) for s in json.load(sys.stdin) if s['id']==2201]"
```

## Other boards, straight to the board

```bash
# Relay board: valves on relays 1-3, master valve on relay 4, AC 1
curl -s http://$RELAY/status
curl -s "http://$RELAY/relay?ch=4&state=on"     # master valve
curl -s "http://$RELAY/relay?ch=1&state=on"     # branch 1. No water unless the master is open too.
curl -s "http://$RELAY/all?state=off"           # all four valves closed, AC untouched
curl -s "http://$RELAY/ac?state=on"             # AC 1 OFF  (wired inverted: on = AC off)
curl -s "http://$RELAY/ac?state=off"            # AC 1 ON

# Zone 1 sensor board: exhaust fan on SSR channel 1
curl -s http://$DHT1/reading
curl -s "http://$DHT1/relay?ch=1&state=on"
curl -s "http://$DHT1/relay?ch=1&state=off"

# Zone 4 sensor board: AC 2, not inverted
curl -s http://$DHT4/status
curl -s "http://$DHT4/ac?state=on"
curl -s "http://$DHT4/ac?state=off"
```

## Finding a board

Addresses come from the router and move when it restarts. On the Pi:

```bash
# Refresh the table, then look a board up by its MAC
for ip in $(seq 2 254); do ping -c1 -W1 192.168.8.$ip >/dev/null 2>&1 & done; wait
ip -4 neigh | grep -v 'FAILED\|INCOMPLETE' | sort -V
```

| Board | MAC |
| --- | --- |
| stack1 bridge | b4:bf:e9:0e:01:b4 |
| relay | c0:cd:d6:d0:04:64 |
| dht1 | 38:3e:51:6f:45:a8 |
| dht2 | 0c:b8:15:75:b4:70 |
| dht3 | 08:a6:f7:b1:39:48 |
| dht4 | 38:3e:51:6f:2b:e4 |

## Reading the answers

- `[http 200]` with JSON: the board took the command.
- `{"error": "... must be 0-255"}` with 400: a value was out of range. Nothing was sent.
- No answer at all: the board is off the network, restarting, or the router has dropped it. See the README's troubleshooting section.
- `arduino_online: false` on the stack bridge: the command went onto the wire, but the Arduino has sent nothing back for 5 s. The bridge sends the command again every minute.
