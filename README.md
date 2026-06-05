# Practica 3 — PIC16F873A Bluetooth Control System

A two-part embedded system project:

- **PIC firmware** (`PIC/Practica3.X/`) — written in C for the XC8 compiler, runs on a PIC16F873A microcontroller at 4 MHz.
- **Android app** (`android-app/`) — written in Kotlin with Jetpack Compose, communicates with the PIC over Bluetooth Classic via an HC-06 module.

The system reads four analogue voltages, drives a stepper motor, and controls a fan via PWM — all commanded wirelessly from a smartphone.

---

## Hardware

### Microcontroller

| Property | Value |
|---|---|
| Device | PIC16F873A (DIP-28) |
| Clock | 4 MHz XT crystal |
| Compiler | MPLAB XC8 |
| IDE | MPLAB X |

### Pin mapping

| PIC Pin | Port/Function | Connected to |
|---|---|---|
| 2 | RA0 / AN0 | Analogue voltage input 0 (0–5 V) |
| 3 | RA1 / AN1 | Analogue voltage input 1 (0–5 V) |
| 4 | RA2 / AN2 | Analogue voltage input 2 (0–5 V) |
| 5 | RA3 / AN3 | Analogue voltage input 3 (0–5 V) |
| 13 | RC2 / CCP1 | PWM output → fan motor driver |
| 17 | RC6 / TX | USART TX → HC-06 RX |
| 18 | RC7 / RX | USART RX ← HC-06 TX |
| 21 | RB0 | Stepper driver IN1 |
| 22 | RB1 | Stepper driver IN2 |
| 23 | RB2 | Stepper driver IN3 |
| 24 | RB3 | Stepper driver IN4 |

> **Important:** LVP (Low-Voltage Programming) must be programmed as OFF so that pin 24 (RB3/PGM) is available as a general-purpose output for the stepper.

### Required external components

| Component | Purpose |
|---|---|
| HC-06 Bluetooth module | Wireless serial link (3.3 V logic — use a voltage divider on the HC-06 RX line if powering PIC at 5 V) |
| ULN2003 / L293D (or similar) | Stepper motor driver — do **not** connect the motor coils directly to the PIC |
| N-channel MOSFET or L298N | Fan driver — pin 13 drives the gate/enable, not the fan directly |
| 4 MHz crystal + 2× 22 pF caps | Oscillator |

---

## Project structure

```
Practica3/
├── PIC/
│   └── Practica3.X/
│       └── main.c          ← PIC firmware (this document focuses here)
└── android-app/
    └── app/src/main/java/com/github/irmin/practica3/
        ├── MainActivity.kt      ← Jetpack Compose UI
        ├── MainViewModel.kt     ← State + command logic
        └── BluetoothController.kt ← Bluetooth SPP socket
```

---

## Bluetooth communication protocol

All commands are plain ASCII, terminated with a newline (`\n`). The HC-06 is configured for **9600 baud, 8N1** (factory default).

### Phone → PIC (commands)

| Command | Example | Effect |
|---|---|---|
| `F<0-100>` | `F75` | Set fan speed to 75 % |
| `M<+/-><N>` | `M+200` | Move stepper 200 steps clockwise |
| `M<+/-><N>` | `M-50` | Move stepper 50 steps counter-clockwise |
| `R` | `R` | Request all four ADC readings |

### PIC → Phone (response)

| Message | Example | Meaning |
|---|---|---|
| `V0:<mV>,V1:<mV>,V2:<mV>,V3:<mV>` | `V0:3300,V1:1650,V2:0,V3:4995` | ADC voltages in millivolts |
| `OK` | `OK` | Sent once at startup |

---

## How `main.c` works

### 1. Configuration bits (`#pragma config`)

These are burned into the PIC's config memory at programming time and control fundamental device behaviour before `main()` even runs:

- `FOSC = XT` — selects the XT oscillator for a 4 MHz crystal.
- `PWRTE = ON` — adds a 72 ms delay after power-on to let the oscillator stabilise.
- `WDTE = OFF` — watchdog timer disabled (the main loop never stalls intentionally).
- `LVP = OFF` — frees RB3 (pin 24) for use as a stepper output.
- `BOREN = ON` — resets the chip automatically if VDD drops too low.

---

### 2. Initialisation sequence (`main()` startup)

`main()` calls five init functions before enabling interrupts:

```
init_ports()  →  init_adc()  →  init_pwm()  →  init_uart()  →  init_timer0()
```

#### `init_ports()`
Sets the direction of every I/O pin via the TRIS registers:
- `TRISA = 0x3F` — all PORTA pins are inputs (they feed the ADC).
- `TRISB = 0x00` — all PORTB pins are outputs (stepper coils).
- `TRISC = 0x80` — RC7 (RX) is input; RC6 (TX) and RC2 (CCP1/PWM) are outputs.

#### `init_adc()`
Configures the 10-bit ADC:
- `ADCON1 = 0x80` — result is **right-justified** (full 10-bit value split across ADRESH:ADRESL), and pins AN0–AN4 are analogue with VDD as the reference voltage.
- `ADCON0 = 0x41` — ADC clock = Fosc/8 (gives TAD = 2 µs, which meets the minimum 1.6 µs requirement), channel 0 selected, ADC powered on.

#### `init_pwm()`
Configures **CCP1** in PWM mode using **Timer2**:
- `PR2 = 249` — sets the PWM period. With Timer2 prescaler = 4:  
  `Period = (249+1) × 4 × (1/4 MHz) × 4 = 1 ms → ~1 kHz`
- `CCP1CON = 0x0C` — enables PWM mode on RC2 (pin 13).
- `CCPR1L = 0x00` — fan starts at 0 % duty (off).

#### `init_uart()`
Configures the USART for 9600 baud, 8N1:
- `SPBRG = 25` — baud rate divisor. Formula: `Fosc / (16 × baud) − 1 = 4 000 000 / (16 × 9600) − 1 = 25`.
- `TXSTA = 0x24` — BRGH=1 (high-speed mode), TXEN=1 (transmitter enabled), asynchronous.
- `RCSTA = 0x90` — SPEN=1 (serial port on), CREN=1 (continuous receive).

#### `init_timer0()`
Configures **Timer0** to overflow approximately every **5 ms** to drive the stepper:
- `OPTION_REG` is set for internal clock (Fosc/4 = 1 MHz) and prescaler 1:32.
- `TMR0 = 100` — pre-loads the counter so it overflows after 156 counts × 32 µs/count ≈ 4.99 ms.
- `T0IE = 1` — enables the Timer0 overflow interrupt.

---

### 3. Interrupt service routine (`isr`)

There is a **single ISR** for all interrupt sources (standard on PIC16 devices). It checks two flags on every entry:

#### Timer0 overflow (`T0IF`)
Fires every ~5 ms. The ISR:
1. Clears the flag and reloads `TMR0 = 100` to restart the countdown.
2. Sets `step_tick = 1` — a flag that the main loop reads to advance the stepper by one step.

The stepper logic itself runs in the main loop, not inside the ISR, to keep interrupt latency short.

#### USART receive (`RCIF`)
Fires on every received byte. The ISR:
1. Checks for an overrun error (`OERR`) and clears it by toggling `CREN` if necessary.
2. Reads `RCREG` (this automatically clears `RCIF`).
3. If the byte is `\n` or `\r` **and** the buffer is not empty, sets `cmd_ready = 1` to signal the main loop.
4. Otherwise, appends the byte to `cmd_buf[]` up to the buffer limit.

---

### 4. ADC reading (`adc_read`)

Called from the command processor when the phone sends `R`. For each of the four channels:

1. Writes `ADCON0` to select the channel, keeping ADCS=01 (Fosc/8) and ADON=1.
2. Waits **30 µs** for the sample-and-hold capacitor to charge (acquisition time).
3. Sets `GO_nDONE = 1` to start the conversion.
4. Polls `GO_nDONE` until it clears (hardware clears it when done, ~10 TAD = 20 µs).
5. Returns the 10-bit result from `ADRESH:ADRESL`.

The raw value (0–1023) is then converted to millivolts:

```
mV = raw × 5000 / 1023
```

The multiplication is done in `uint32_t` to avoid a 16-bit overflow before the division.

---

### 5. PWM / fan control (`fan_set_pct`)

The CCP1 PWM module uses a **10-bit duty cycle** register split across:
- `CCPR1L` — top 8 bits
- `CCP1CON<5:4>` — bottom 2 bits

The function converts a percentage (0–100) to the 10-bit duty word and writes both fields:

```c
duty = pct * 250 / 100        // 250 = PR2+1
CCPR1L  = duty >> 2           // bits 9:2
CCP1CON = (CCP1CON & 0xCF) | ((duty & 0x03) << 4)  // bits 1:0
```

The hardware updates the PWM output at the end of the current period, so there is no glitch.

---

### 6. Stepper motor

The stepper uses **4-phase full-step** drive. Two coils are always energised simultaneously for maximum torque:

| Step index | RB3 (IN4) | RB2 (IN3) | RB1 (IN2) | RB0 (IN1) | Hex |
|:---:|:---:|:---:|:---:|:---:|:---:|
| 0 | 0 | 0 | 1 | 1 | `0x03` |
| 1 | 0 | 1 | 1 | 0 | `0x06` |
| 2 | 1 | 1 | 0 | 0 | `0x0C` |
| 3 | 1 | 0 | 0 | 1 | `0x09` |

Advancing the index **forward** (0→1→2→3→0) = **clockwise**.  
Advancing the index **backward** (0→3→2→1→0, i.e. `+3 mod 4`) = **counter-clockwise**.

In the main loop, every time `step_tick` is set by the Timer0 ISR:
- If `step_count > 0`: advance forward, decrement `step_count`.
- If `step_count < 0`: advance backward, increment `step_count`.
- If `step_count == 0`: do nothing — coils remain energised at the last position to provide **holding torque**.

The step rate is 1 step per ~5 ms = **200 steps/second**. For a standard 200-step/revolution motor this equals 60 RPM.

---

### 7. Command processor (`process_cmd`)

Called from the main loop when `cmd_ready == 1`.

**Thread-safety step:** Interrupts are disabled briefly (`GIE = 0`) to copy `cmd_buf` into a local `local[]` array and reset `cmd_len = 0`. Interrupts are immediately re-enabled. This prevents the UART ISR from corrupting the buffer while it is being read.

The first character of the command selects the action (case-insensitive):

| First char | Action |
|---|---|
| `R` / `r` | Reads all 4 ADC channels, converts to mV, sends the response string |
| `F` / `f` | Calls `atoi()` on the remainder, clamps to 0–100, calls `fan_set_pct()` |
| `M` / `m` | Calls `atoi()` on the remainder (handles `+`/`-` sign), stores in `step_count` |

---

### 8. Main loop

```
for (;;) {
    if (cmd_ready)  → process_cmd()
    if (step_tick)  → advance stepper one step
}
```

The loop is non-blocking. Both flags (`cmd_ready`, `step_tick`) are set by the ISR and consumed here. If a command arrives while the stepper is running, neither is blocked — the stepper simply advances on the next tick after the command has been processed.

---

## Android app

### Architecture

The app uses the **MVVM** pattern with Jetpack Compose for the UI:

| File | Responsibility |
|---|---|
| `BluetoothController.kt` | Opens an RFCOMM socket (SPP UUID `00001101-…`), sends commands as ASCII+`\n`, reads response lines via a `BufferedReader` on the IO dispatcher |
| `MainViewModel.kt` | Owns the `BluetoothController`, exposes `UiState` as a `StateFlow`, parses ADC responses with `parseAdcResponse()`, and provides action functions (`connect`, `disconnect`, `requestAdc`, `setFanSpeed`, `moveStepper`) |
| `MainActivity.kt` | Composable UI split into four cards: Connection, ADC Readings, Fan Control, Stepper Motor |

### UI overview

- **Connection card** — dropdown of paired Bluetooth devices, Connect/Disconnect button, colour-coded status badge.
- **ADC card** — "Read" button triggers an `R` command; each channel shows its value in mV with a proportional progress bar.
- **Fan card** — slider (0–100 %) sends an `F<value>` command when released.
- **Stepper card** — numeric field for step count, two buttons for CW (`M+<N>`) and CCW (`M-<N>`).

### Permissions required (Android 12+)

```xml
BLUETOOTH_CONNECT   <!-- to list paired devices and open sockets -->
```

The app requests this at runtime when the user first opens the device list.

---

## Building and flashing

### PIC firmware

1. Open MPLAB X and import the project from `PIC/Practica3.X/`.
2. Select the **XC8** compiler and your programmer (PICkit 3/4, etc.).
3. Build → Program. The config bits are embedded in the source via `#pragma config`.

### Android app

1. Open `android-app/` in Android Studio.
2. Connect a device (or use an emulator with Bluetooth passthrough).
3. Run → select your device.

> Pair the HC-06 with your phone **before** launching the app (default PIN: `1234`).
