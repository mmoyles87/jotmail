#!/usr/bin/env python3
"""Fill the demo server's Notes folder with sample notes, written the way iOS writes them.

    seed.py HOST PORT CA_FILE

Standard library only. The two images are drawn here, so the demo carries no image files.
"""
import base64
import email.utils
import html
import imaplib
import math
import quopri
import random
import re
import ssl
import struct
import sys
import time
import uuid
import zlib

# --- images ---------------------------------------------------------------------------


def png(width, height, pixel):
    """A PNG from pixel(x, y) -> (r, g, b)."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x, y))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def ridge(width, rng, base, rough):
    """A mountain skyline: midpoint displacement, one height per column."""
    n = 1
    while n < width:
        n *= 2
    h = [0.0] * (n + 1)
    h[0], h[n] = rng.uniform(-1, 1), rng.uniform(-1, 1)
    step, scale = n, 1.0
    while step > 1:
        for i in range(step // 2, n, step):
            h[i] = (h[i - step // 2] + h[i + step // 2]) / 2 + rng.uniform(-scale, scale)
        step //= 2
        scale *= rough
    return [base - h[int(x * n / width)] * 60 for x in range(width)]


def mountains(width=960, height=540):
    rng = random.Random(7)
    top, horizon = (36, 38, 84), (247, 170, 112)
    sun_x, sun_y, sun_r = width * 0.66, height * 0.40, 42
    layers = [  # (base height, roughness, colour), far to near
        (height * 0.52, 0.62, (150, 112, 150)),
        (height * 0.60, 0.60, (104, 74, 120)),
        (height * 0.70, 0.58, (66, 46, 88)),
        (height * 0.80, 0.55, (34, 26, 52)),
    ]
    skylines = [ridge(width, rng, b, r) for b, r, _ in layers]
    lake = height * 0.86

    def sky(x, y):
        c = mix(top, horizon, (y / (height * 0.62)) ** 1.4)
        d = math.hypot(x - sun_x, y - sun_y)
        if d < sun_r:
            return (255, 230, 178)
        return mix(c, (255, 214, 160), max(0.0, 1 - (d - sun_r) / 260) ** 2 * 0.55)

    def pixel(x, y):
        if y >= lake:  # the lake mirrors what is above it, darker and rippled
            my = int(2 * lake - y + math.sin(y * 0.9 + x * 0.05) * 2)
            return mix(pixel(x, max(0, min(my, int(lake) - 1))), (20, 22, 48), 0.45)
        colour = sky(x, y)
        for (sky_line, (_, _, c)) in zip(skylines, layers):
            if y >= sky_line[x]:
                haze = max(0.0, 1 - (y - sky_line[x]) / 140)
                colour = mix(c, horizon, 0.18 * haze)
        return colour

    return png(width, height, pixel)


def garden(width=800, height=480):
    rng = random.Random(3)
    grass, path, frame, soil = (96, 140, 72), (196, 182, 150), (150, 108, 70), (78, 56, 40)
    beds = [(60, 60, 230, 360), (285, 60, 230, 360), (510, 60, 230, 360)]
    plants = []
    greens = [(66, 150, 70), (110, 170, 60), (40, 120, 80), (200, 70, 60), (240, 200, 70)]
    for bi, (bx, by, bw, bh) in enumerate(beds):
        for row in range(5):
            for col in range(3):
                cx = bx + 40 + col * (bw - 80) / 2 + rng.uniform(-6, 6)
                cy = by + 40 + row * (bh - 80) / 4 + rng.uniform(-6, 6)
                plants.append((cx, cy, rng.uniform(16, 24), greens[(bi * 2 + row) % len(greens)]))
    noise = [rng.uniform(-10, 10) for _ in range(997)]

    def pixel(x, y):
        n = noise[(x * 31 + y * 17) % 997]
        if 440 <= y < 470 or (x < 30 or x >= width - 30):
            base = path
        else:
            base = grass
        for bx, by, bw, bh in beds:
            if bx <= x < bx + bw and by <= y < by + bh:
                edge = min(x - bx, bx + bw - 1 - x, y - by, by + bh - 1 - y)
                base = frame if edge < 10 else soil
                break
        for cx, cy, r, c in plants:
            d = math.hypot(x - cx, y - cy)
            if d < r:
                return mix(c, (255, 255, 255), 0.25 * (1 - d / r))
        return tuple(max(0, min(255, int(v + n))) for v in base)

    return png(width, height, pixel)


# --- notes ------------------------------------------------------------------------------

HOUR, DAY = 3600, 86400
NOTES = [  # (seconds ago, markdown); [attachment:<name>] lines name an image below
    (2 * HOUR, """# Dolomites trip
**Sept 12–19** · fly into Venice, car from Marco Polo
[attachment:mountains.png]
## Route
1. Cortina d’Ampezzo, 2 nights
2. Val Gardena, 3 nights: Seceda ridge at sunrise
3. Alta Badia, 2 nights
## Book before June
- Rifugio Lagazuoi for the night of the 15th
- Car hire with a roof box
- Via ferrata kit in Cortina
Trail conditions: https://www.dolomiti.it/en"""),
    (26 * HOUR, """# Sourdough schedule
Feed the starter at 9 pm the night before, **1:1:1**.
1. 8:00 autolyse: 450 g flour, 330 g water
2. 9:00 add 90 g levain and 10 g salt
3. 9:30–12:00 four sets of stretch and folds
4. Bulk until it has risen by half
5. Shape, then into the fridge overnight
Bake at *250 °C*: lid on for 20 minutes, lid off for 25."""),
    (3 * DAY, """# Garden plan
[attachment:garden.png]
## Bed 1
- Tomatoes: San Marzano, Sungold
- Basil along the front edge
## Bed 2
- Beans up the trellis
- Lettuce underneath, sown every two weeks
## Bed 3
- Garlic in October, *harvest in July*"""),
    (6 * DAY, """# Books to read
- *The Overstory*, Richard Powers
- *Piranesi*, Susanna Clarke
- *A Psalm for the Wild-Built*, Becky Chambers
- *The Dawn of Everything*, Graeber and Wengrow
Library hold placed for **Piranesi**."""),
    (13 * DAY, """# Bike service
- Chain at 0.5 wear: replace before spring
- Rear brake pads
- Tubeless sealant, 60 ml a wheel
- True the front wheel
Shop quote **$145**, ready Thursday."""),
    (22 * DAY, """# Q4 planning
## Decisions
- Beta to ten customers by **Nov 15**
- Weekly sync moves to Tuesday mornings
## Open questions
1. Do we need a second region before launch?
2. Who owns the onboarding docs?"""),
    (35 * DAY, """# Shakshuka
- 2 tbsp olive oil, 1 onion, 1 red pepper
- 3 cloves garlic, 1 tsp cumin, 1 tsp paprika
- 800 g tinned tomatoes
- 6 eggs, feta, parsley
Simmer the sauce for **15 minutes**, then make wells for the eggs. Lid on, 6–8 minutes."""),
    (61 * DAY, """# Packing list
- Passport, chargers, plug adapter
- Rain shell and a warm layer
- Headlamp
- Trail shoes *and* something for dinner"""),
    (140 * DAY, """# Shed ideas
- Pegboard wall for hand tools
- Rain barrel off the back gutter
- Skylight panel in the roof
Paint: *Farrow & Ball, Card Room Green*"""),
    (390 * DAY, """# Quotes
“Simplicity is prerequisite for reliability.” *Edsger Dijkstra*
“The best way out is always through.” *Robert Frost*"""),
]
IMAGES = {"mountains.png": mountains, "garden.png": garden}


def inline(text):
    out = html.escape(text, quote=False)
    out = re.sub(r"\[([^\]]+)\]\((https?://[^)\s]+)\)", r'<a href="\2">\1</a>', out)
    out = re.sub(r'(?<!href=")(https?://[^\s<]+)', r'<a href="\1">\1</a>', out)
    out = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", out)
    return re.sub(r"\*([^*]+)\*", r"<i>\1</i>", out)


def to_html(md, cids):
    out, open_list = [], None
    for line in md.split("\n"):
        kind = "ul" if line.startswith("- ") else "ol" if re.match(r"\d+\. ", line) else None
        if kind != open_list:
            if open_list:
                out.append(f"</{open_list}>")
            if kind:
                out.append(f"<{kind}>")
            open_list = kind
        if kind:
            item = re.sub(r"^(- |\d+\. )", "", line)
            out.append(f"<li>{inline(item)}</li>")
        elif m := re.match(r"\[attachment:(.+)\]$", line):
            out.append(f'<div><object type="application/x-apple-msg-attachment" data="cid:{cids[m[1]]}"><br></object><br></div>')
        elif m := re.match(r"(#{1,3}) (.*)", line):
            n = len(m[1])
            out.append(f"<div><h{n}>{inline(m[2])}</h{n}></div>")
        elif not line.strip():
            out.append("<div><br></div>")
        else:
            out.append(f"<div>{inline(line)}</div>")
    if open_list:
        out.append(f"</{open_list}>")
    return "<html><head></head><body>" + "".join(out) + "</body></html>"


def message(md, when, user):
    names = re.findall(r"^\[attachment:(.+)\]$", md, re.M)
    cids = {n: str(uuid.uuid4()).upper() for n in names}
    title = md.split("\n", 1)[0].lstrip("# ")
    date = email.utils.formatdate(when, localtime=True)
    head = [f"From: {user}", "X-Uniform-Type-Identifier: com.apple.mail-note", "Mime-Version: 1.0 (Mac OS X Notes 4.13 \\(3195\\))",
            f"Date: {date}", f"X-Mail-Created-Date: {email.utils.formatdate(when - 3 * DAY, localtime=True)}",
            f"Subject: {email.header.Header(title, 'utf-8').encode()}",
            f"X-Universally-Unique-Identifier: {str(uuid.uuid4()).upper()}", f"Message-Id: <{uuid.uuid4()}@example.com>"]
    body = quopri.encodestring(to_html(md, cids).encode()).decode()
    text = "Content-Transfer-Encoding: quoted-printable\nContent-Type: text/html;\n\tcharset=utf-8"
    if not names:
        return "\n".join(head + [text, "", body]).replace("\n", "\r\n").encode()
    b = f"Apple-Mail=_{uuid.uuid4()}".upper()
    parts = [f"--{b}\n{text}\n\n{body}"]
    for n in names:
        data = base64.encodebytes(IMAGES[n]()).decode()
        parts.append(f"--{b}\nContent-Transfer-Encoding: base64\nContent-Disposition: inline;\n\tfilename={n}\n"
                     f"Content-Type: image/png;\n\tname=\"{n}\"\nContent-Id: <{cids[n]}>\n\n{data}")
    head.append(f'Content-Type: multipart/related;\n\ttype="text/html";\n\tboundary="{b}"')
    return "\n".join(head + [""] + parts + [f"--{b}--", ""]).replace("\n", "\r\n").encode()


def main():
    import email.header  # noqa: F401  (used by message())
    host, port, ca = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    user = "demo@example.com"
    ctx = ssl.create_default_context(cafile=ca)
    m = imaplib.IMAP4_SSL(host, port, ssl_context=ctx)
    m.login(user, "demo")
    now = time.time()
    for ago, md in reversed(NOTES):  # oldest first, so UIDs follow the dates
        typ, _ = m.append("Notes", r"(\Seen)", imaplib.Time2Internaldate(now - ago), message(md, now - ago, user))
        if typ != "OK":
            sys.exit("append failed")
    print(f"seeded {len(NOTES)} notes")


if __name__ == "__main__":
    import email.header
    main()
