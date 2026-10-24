# Atech SDK — guide for coding agents

You are helping someone make an **Atech board** do something. The board is an
ESP32-S3 with numbered ports; modules (button, light grid, sensors, motors, a robot
arm…) slide into the ports. You write one small file, `project.yaml`. The SDK turns
it into firmware, compiles it, flashes it, and then lets you talk to the board from
the laptop.

This text ships inside the package: `atech guide` prints it, and `atech new` puts a
copy in every project as `AGENTS.md` and `CLAUDE.md`.

## The whole workflow

```bash
atech list-boards                  # ask the user which board they are holding
atech list-modules                 # what exists. Never invent a module
atech info <module>                # its C++ API, events, actions and cautions
atech new <name> --board 14port    # project.yaml + this guide
#   ...edit project.yaml...
atech describe <dir>               # what you placed, the API, the events and actions
atech validate <dir>               # placement check
atech build <dir>                  # generate + compile
atech upload <dir>                 # flash the board on USB
atech check                        # reboot it: which modules answered, and why not
atech monitor                      # events coming from the board
atech send <key> [value]           # one action, confirmed by the board (value: r=255 g=0, a number, or JSON)
```

Every command exits non-zero when it fails and says why on stderr (`error: …`, or
the list of placement issues). Cautions are printed on stdout with the result.
`list-boards`, `list-modules`, `info` and `monitor` take `--json`.

## project.yaml

```yaml
name: my_project
board: 14port                        # 8port | 10port | 14port
modules:
  - {id: button,   instance: btn, port: 1}
  - {id: neopixel, instance: led, port: 2}
  - {id: speaker,  instance: spk, ports: [13, 14]}   # 2-port module: an adjacent pair
code:
  setup: |
    // optional, runs once after every module is initialised
  loop: |
    // runs forever
    if (btn.wasPressed()) {
      led.setAll(255, 0, 0);
      led.show();
    }
```

- `instance` is the name of the C++ object you call in `code:`. Its methods are in
  `atech info <module>` and `atech describe`.
- Ports: `atech list-boards` shows which ports take modules and which of them have
  UART. A 2-port module needs two neighbouring ports; a UART module (the robot arm)
  needs a UART port. `atech validate` says what is wrong and which ports would work.
- **Ask the user which port each module is in.** You cannot see the board.
- Power modules (USB-C, power jack) are not in the catalog and need no entry in
  `project.yaml`: they carry no firmware. Leave the ports they sit in unused.
- There are exactly two code blocks, `setup` and `loop`. Keep state in `static`
  variables inside `loop`.
- `atech validate` checks placement only. Mistakes in your C++ show up in `atech build`.

## Rules for the `code:` block

- **Never block.** Use `millis()` for timing; no `delay()` beyond a few ms.
- **Edge calls are yours.** `btn.wasPressed()`, `knob.wasRotatedCW()`,
  `pir.wasTriggered()`, `imu.wasShaken()` return true once per edge and then clear.
  The firmware's own events to the laptop do not use them, so your code and the
  laptop both see every press. Call each edge function in one place only.
- **Do not read `Serial` yourself.** The generated firmware reads it to receive
  actions from the laptop. Reading it in your code steals those bytes.
- **Do not hardcode GPIO numbers.** Pins come from the port.
- **Do not edit `build/`.** It is regenerated on every `atech build`.
- To send your own event to the laptop, print one JSON line:

  ```cpp
  Serial.printf("{\"type\":\"event\",\"payload\":{\"event_type\":\"sensor\",\"key\":\"level\",\"value\":%d}}\n", level);
  ```

- To accept your own action from the laptop, test `atech_action` and confirm it:

  ```cpp
  if (atech_action == "buzz") { spk.playTone(880, 100); atechDone(); }
  ```

  `atech_value` holds the value as a `String`. An action nobody handles is
  answered with `ok:false`, so the laptop knows.

## Talking to the board from the laptop

`atech describe` lists the exact event and action names of a project. They are
built from the instance name, for example `btn`, `th_temperature`, `led_fill`.

```bash
atech monitor --key btn                          # only the button's events
atech send led_fill r=255 g=0 b=0                # prints ok only if the board did it
```

```python
from atech import Board, ActionRejected, NoAcknowledgement, NoBoardFoundError

with Board.connect() as board:                   # or Board.connect(port="/dev/cu.usbmodem1101")
    board.call("led_fill", {"r": 255, "g": 0, "b": 0})         # do something
    joints = board.query("arm_read", reply="arm_positions")   # ask, get the answer back
    last = board.latest("btn")                   # last event seen for a key, or None
    for event in board.events(keys=["btn"]):     # blocks, yields events forever
        print(event.key, event.value)
```

- `board.call(key, value="", timeout=2.0)` returns once the board confirmed. It
  raises `ActionRejected` when the board could not do it (no such action, bad value,
  a servo that does not answer) and `NoAcknowledgement` when nothing came back.
- `board.query(key, value="", reply=<event key>)` is `call` plus the value of the
  event the board answers with. Use it for every read-back.
- `board.send()` only writes to the port and tells you nothing. Use `call`.
- `Board.connect()` takes the first board on USB and raises `NoBoardFoundError` when
  there is none. With several boards attached, pass `port=` (`atech ports` lists them).
- Wait for each action before sending the next: a flood of unconfirmed ones can
  overflow the board's buffer and some get lost.
- **One program per board at a time.** A second program opening the same port gets
  `PortUnavailableError`. Stop `atech monitor` before `atech send` or `atech upload`,
  or listen and send from one Python script as above.

## Things that are true and save time

- **The first build downloads the ESP32 compiler** (about 2.4 GB on disk, once).
  After that a build takes seconds. Do the first build on good wifi. A few modules
  (the light grid, for one) also download a small library the first time a project
  uses them, so a new project can still need the network once.
- **`atech upload` always regenerates and rebuilds from `project.yaml`.** There is no
  stale build to worry about.
- **Port in use?** Only one program can hold the board's port. Close the other one
  (`atech monitor` in another terminal, an editor's serial monitor), or `atech free-port`.
- **Linux, "no permission":** `sudo usermod -aG dialout $USER`, then log in again.
- **No board found?** It must be plugged in with a USB data cable. `atech ports`
  shows what the SDK sees. The SDK never falls back to some other serial port.
- **`atech check` cannot see output-only modules.** A light grid or a display
  reports "initialized" even when nothing is plugged in. Only eyes confirm output.
- **A module does nothing?** Do not re-flash the same firmware. Print a line
  before and after the suspect call and watch `atech monitor --json`; swap the
  module into another port; keep one known-good module in the same build.
- **Displays:** `display()` redraws the whole screen and takes about 273 ms. Draw
  the background once, then use `displayRegion(x, y, w, h)` for what changes.

### Robot arm: power

The Robot Arm module passes the board's high-voltage rail (labelled 12 V) straight
to the servos. That rail is fed by a power module (the power jack module, or the
USB-C PD power module) and carries whatever that module is given. Nothing regulates
it, and the standard USB-C module does not feed it. SO-101 arms come in a 12 V and
a 5 V version, and a 12 V supply on a 5 V arm damages the servos.

Building is harmless. Before you **flash, or tell the user to connect or power the
arm**, ask which arm they have and what voltage is printed on the supply in the
jack, and tell them: 12 V supply for a 12 V arm, 5 V supply for a 5 V arm, never
more than 12 V. `atech validate`, `build` and `upload` print this as a `CAUTION`;
relay it. With nothing in the power jack the servos have no power, so a silent arm
on USB-C alone is expected.

The arm goes stiff when the firmware starts (torque is switched on in setup) and
moves as soon as something tells it to. Have the user clear its reach first.

### Robot arm: no servo answers

If the arm has power and `atech check` still reports it not responding: on a
Robot Arm V1.0 module with six servos, the servos only answered when the board
transmitted on the port's line B. The module transmits on line A by default. Swap
the two lines in `project.yaml` and upload again:

```yaml
  - {id: robot_arm, instance: arm, port: 3, swap_lines: true}
```

Tell the user which of the two worked.

## A module the catalog does not have

```
my_project/
├── project.yaml          # add:  modules_path: ./my_modules
└── my_modules/
    └── <id>/
        ├── module.yaml   # same shape as a bundled one
        ├── <id>.h
        └── <id>.cpp
```

Start from a bundled module: `python -c "import atech.catalog, pathlib;
print(pathlib.Path(atech.catalog.__file__).parent / 'data' / 'modules')"` prints
where they live. A module with the same id as a bundled one replaces it. In
`module.yaml`, every event you declare must be a key the `loop` template really
prints, and every action needs a `handler:` (C++ run when the action arrives;
the value is the `String value`).

## Never

- Invent a module, a method, an event or an action. `atech info` is the truth.
- Skip `atech validate`.
- Tell the user something worked because a command exited 0. Flashing succeeding
  says nothing about the hardware; check with `atech check`, `atech monitor`, or
  ask them what they see.
- Use `delay(1000)` in `loop`.
- Connect or power a robot arm before the supply question above is answered.
