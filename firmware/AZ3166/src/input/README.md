# Button Input

Turn sampled button levels into debounced, one-shot press events. The core
debouncer is independent of Arduino GPIO and can be reused with another input
source; the two-button controller adds the AZ3166 application's pin and event
mapping.

## Components and Design

| Component | Responsibility |
| --- | --- |
| [ButtonDebouncer.h](ButtonDebouncer.h) | Track raw and stable pressed states and emit an edge after the debounce interval. |
| [ButtonController.h](ButtonController.h) | Sample two active-low pins and return application-facing button events. |
| `ButtonEvents` | Report `uploadRequested` and `toggleUploadPause` independently. |

### ButtonDebouncer

`begin(rawPressed, now)` establishes the initial state without generating a
press. `update(rawPressed, now)` returns true only when a newly pressed state
remains stable for the configured interval. Holding the button does not repeat
events, and release updates state without emitting a press.

Both the input and timestamp are supplied by the caller, making bounce traces,
initially held buttons, and timer wraparound easy to test. If `begin()` is
omitted, the first update initializes the state without generating an event.

### ButtonController

The controller owns one debouncer per pin. `begin()` configures the pins as
inputs and records their initial levels. `update()` reads GPIO and `millis()`;
`updateFromInputs()` accepts already-normalized pressed states and a timestamp.

The controller reports events but does not call the cloud uploader. The
application decides how those events affect scheduling. Both flags may be set
when the buttons are pressed together.

## Reuse in Another Sketch

For the board's two user buttons:

```cpp
#include <Arduino.h>
#include "src/input/ButtonController.h"

ButtonController buttons(USER_BUTTON_A, USER_BUTTON_B, 50);

void setup() {
    Serial.begin(115200);
    buttons.begin();
}

void loop() {
    ButtonEvents events = buttons.update();
    if (events.uploadRequested) {
        Serial.println("Button A pressed");
    }
    if (events.toggleUploadPause) {
        Serial.println("Button B pressed");
    }
}
```

For a different input source, use the lower-level debouncer directly:

```cpp
#include "src/input/ButtonDebouncer.h"

ButtonDebouncer selectionButton(50);

bool sampleSelection(bool rawPressed, uint32_t now) {
    return selectionButton.update(rawPressed, now);
}
```

Normalize the physical level to a pressed boolean before calling the debouncer.
Choose the sampling cadence and debounce interval for the switch being used;
the application currently uses 50 milliseconds.

## Ownership and Limits

- Each debouncer/controller owns mutable state. Poll it from one execution
  context, or provide synchronization if sharing it.
- `ButtonDebouncer` depends only on fixed-width integer types. `ButtonController`
  depends on Arduino/AZ3166 GPIO types and assumes active-low inputs.
- The controller uses `INPUT`, not an internal pull-up mode. The supplied board
  pins must have appropriate electrical biasing; adapt that setup for other pins.
- The controller's event names and startup log reflect this application's upload
  controls. Reuse `ButtonDebouncer` when you need neutral input semantics.
- Neither component starts a thread or buffers missed input. Long blocking work
  between samples can delay or miss presses. Long-press, double-click, repeat,
  and release notifications are not implemented.
- Timestamp arithmetic assumes an unsigned millisecond counter with the same
  width as `uint32_t`; do not reset the time source while preserving old state.

## Verification and Related Guides

From the repository root:

```powershell
& .\firmware\tests\run-button-debouncer-tests.ps1 -Action Verify
& .\firmware\tests\run-button-controller-tests.ps1 -Action Verify
```

The focused suites use synthetic input sequences to check bounce, holds,
startup state, simultaneous buttons, and timer wraparound. Hardware execution
requires an explicit detected ST-Link port and the test harness restores
production afterward.

See [ButtonDebouncer.cpp](ButtonDebouncer.cpp),
[ButtonController.cpp](ButtonController.cpp), the [cloud guide](../cloud/README.md),
and the [firmware design](../../../../docs/firmware-design.md#11-button-input).