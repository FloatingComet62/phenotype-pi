"""Stack bridges: which LEDs are which row, and how to reach a stack.

One ESP32 bridge per stack (firmware src/esp32_stack) fronts the Arduino
that drives the stack's three LED strips. A strip ("path") carries several
rows; a row is a range of LED indexes on its strip.

Measured on site, 28 Sep 2026; strips per pin confirmed 13 Oct 2026.
Indexes are the Arduino's, 0-299. Path n is the Arduino's strip n:
path 0 = pin 13 = shelf 1 (bottom) rows 1-3, path 1 = pin 12 = row 4 of
both shelves, path 2 = pin 11 = shelf 2 (upper) rows 1-3.
"""

# (shelf, row) -> (path, first LED, last LED)
LED_MAP = {
    (1, 1): (0, 4, 87),
    (1, 2): (0, 104, 190),
    (1, 3): (0, 205, 299),
    (1, 4): (1, 1, 83),
    (2, 1): (2, 1, 86),
    (2, 2): (2, 105, 189),
    (2, 3): (2, 206, 290),
    (2, 4): (1, 106, 188),
}
# ponytail: one map for every stack, because one stack is wired. Make it
# per-stack (keyed by stack number too) when a second stack's strips are
# measured and turn out to differ.


def led_segment(shelf: int, row: int) -> tuple[int, int, int] | None:
    return LED_MAP.get((shelf, row))


def brightness(intensity_percent: int) -> int:
    """Dashboard intensity 0-100 % -> the Arduino's 0-255."""
    return round(max(0, min(100, intensity_percent)) * 255 / 100)


def stack_host(hosts: str, stack: int) -> str | None:
    """STACK_HOSTS is comma-separated; position n is stack n."""
    names = [h.strip() for h in hosts.split(",")]
    if 1 <= stack <= len(names) and names[stack - 1]:
        return names[stack - 1]
    return None
