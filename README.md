# Gimli

Gimli is an Adafruit MatrixPortal S3 LED matrix display that will be controlled over MQTT.

## Status

- Firmware in this folder is currently scaffold/boilerplate.
- This README defines the target behavior and MQTT contract for implementation.

## Goal

Render text and animate it on the matrix in response to MQTT messages.

## Hardware Target

- Board: Adafruit MatrixPortal S3
- Panel: Medium 16x32 RGB LED Matrix (6mm pitch)
- Logical resolution used by firmware: 32x16

## MQTT Direction

Gimli is not currently being controlled by Noodles text commands.

For now, a separate publisher system will send text/animation events directly to Gimli.

We still want to stay close to the existing style used elsewhere in the workspace:

- app-scoped topics under `apps/...`
- compact JSON payloads
- short event names

### Broker

- Host: `192.168.8.100` (also `mose.local`)
- Port: `1883`

### Subscriber

- Client: `gimli-matrixportal` (proposed)
- Primary topic: `apps/gimli/text`

## Active Event Format (Current)

Primary render event:

```json
{"event":"render_text","text":"HELLO WORLD","direction":"left","speed":30}
```

Field notes:

- `event`: currently `render_text`
- `text`: required string to display
- `direction`: `left` or `right`
- `speed`: optional integer

Additional simple events:

```json
{"event":"clear"}
```

```json
{"event":"test"}
```

If no text has been received yet, Gimli should use a default string (for example: `GIMLI`).

## Future Compatibility Mode (Optional)

If we later route Gimli through Noodles app control flow, we can additionally support:

- Topic: `apps/gimli/control`
- JSON key events, for example:

```json
{"key":14,"pressed":true}
```

- Plain text encoder events, for example:

```text
enc1-gimli-right
```

This mode is not required for the initial implementation.

## Pending Event Catalog

Detailed animation event variants will be added here once provided.

Planned additions:

- Additional animation modes
- Timing/easing controls
- Priority/interrupt behavior
- Validation/default behavior for missing fields

## MQTT Testing

Subscribe and watch Gimli topic traffic:

```bash
mosquitto_sub -h mose.local -t "apps/gimli/text" -v
```

Send a basic render event:

```bash
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"render_text","text":"HELLO FROM MAC","direction":"left","speed":30}'
```

Send right-to-left text:

```bash
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"render_text","text":"RIGHT SCROLL","direction":"right","speed":30}'
```

Test faster/slower animation:

```bash
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"render_text","text":"FAST","direction":"left","speed":12}'
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"render_text","text":"SLOW","direction":"left","speed":80}'
```

Clear the display:

```bash
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"clear"}'
```

Run test pattern:

```bash
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"test"}'
```

Run test2 (wild mode):

```bash
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"test2"}'
```

Run test3 (multicolor block sizes):

```bash
mosquitto_pub -h mose.local -t "apps/gimli/text" -m '{"event":"test3"}'
```

## Build

Built with PlatformIO using:

- Board: `adafruit_matrixportal_esp32s3`
- Framework: `arduino`
