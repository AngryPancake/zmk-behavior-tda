# ZMK Tap Dance Advanced (TDA)

**ZMK Behavior Module**

`zmk-behavior-tda` is a custom behavior module for ZMK Firmware.  
It enhances Tap Dance by triggering the first action immediately.

## Features
- Immediate first-tap activation.
- Multi-tap upgrade to next behavior.
- Configurable tapping term.

## Usage

```yaml
behaviors:
  - &tda_example
    behavior: zmk,behavior-tda
    tapping-term-ms: 200
    bindings:
      - { behavior: &kp KEY_A }
      - { behavior: &kp KEY_B }
      - { behavior: &kp KEY_C }

keymap:
  - layer:
      bindings:
        - &tda_example
