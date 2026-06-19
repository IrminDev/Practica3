# Guía Completa del Proyecto PIC16F873A — Practica3

## Índice

1. [Descripción general](#descripción-general)
2. [Configuración del microcontrolador](#configuración-del-microcontrolador)
3. [Mapa de pines](#mapa-de-pines)
4. [Cómo funciona el ADC](#cómo-funciona-el-adc)
   - [Registros del ADC](#registros-del-adc)
5. [Cómo funciona el PWM (control del ventilador)](#cómo-funciona-el-pwm)
   - [Registros del PWM](#registros-del-pwm)
6. [Cómo funciona la comunicación UART / Bluetooth](#cómo-funciona-la-comunicación-uart--bluetooth)
   - [Registros de la UART](#registros-de-la-uart)
7. [Cómo funciona el motor a pasos](#cómo-funciona-el-motor-a-pasos)
8. [Circuitos de potencia (drivers de hardware)](#circuitos-de-potencia-drivers-de-hardware)
   - [Ventilador — 2N2222 + diodo](#ventilador--transistor-2n2222--diodo-conmutador-de-lado-bajo)
   - [Motor a pasos — 28BYJ-48 + ULN2003](#motor-a-pasos--28byj-48--tarjeta-uln2003)
   - [Requisitos a nivel de placa](#requisitos-a-nivel-de-placa-imprescindibles)
9. [Rutina de interrupción (ISR)](#rutina-de-interrupción-isr)
10. [Función por función](#función-por-función)
11. [Flujo general del programa](#flujo-general-del-programa)

---

## Descripción general

El firmware controla un **PIC16F873A** corriendo a **4 MHz** (cristal XT). El sistema realiza cuatro tareas simultáneas coordinadas mediante interrupciones y un lazo principal (`main`):

| Tarea | Periférico | Descripción |
|---|---|---|
| Leer tensiones analógicas | ADC (AN0–AN3) | 4 canales, 10 bits, 0–5 V |
| Controlar velocidad de ventilador | CCP1/PWM + Timer2 | Señal PWM ~1 kHz en RC2 |
| Comunicación Bluetooth | USART (RC6/RC7) | 9600 baud, comandos por texto |
| Mover motor a pasos | GPIO RB0–RB3 + Timer0 | 4 fases, paso completo, 200 pasos/s máx |

---

## Configuración del microcontrolador

```c
#pragma config FOSC  = XT      // Oscilador de cristal XT a 4 MHz
#pragma config WDTE  = OFF     // Watchdog desactivado
#pragma config PWRTE = ON      // Temporizador de encendido (72 ms de estabilización)
#pragma config CP    = OFF     // Sin protección de código
#pragma config BOREN = ON      // Reset por caída de tensión activado
#pragma config LVP   = OFF     // Programación de bajo voltaje DESACTIVADA
#pragma config CPD   = OFF     // Sin protección de datos EEPROM
#pragma config WRT   = OFF     // Sin protección de escritura Flash
```

**Por qué `LVP = OFF`:** El pin RB3 comparte función con PGM (programación de bajo voltaje). Si LVP estuviera activo, RB3 no podría usarse como salida GPIO para el motor a pasos. Al desactivarlo, los cuatro pines RB0–RB3 quedan disponibles para las bobinas del motor.

---

## Mapa de pines

```
PIC16F873A (DIP-28)
┌──────────┬──────┬────────────────────────────────────────┐
│ Pin físico│ Nombre│ Función en este proyecto               │
├──────────┼──────┼────────────────────────────────────────┤
│  2       │ RA0  │ ADC canal 0 — entrada analógica 0–5 V  │
│  3       │ RA1  │ ADC canal 1 — entrada analógica 0–5 V  │
│  4       │ RA2  │ ADC canal 2 — entrada analógica 0–5 V  │
│  5       │ RA3  │ ADC canal 3 — entrada analógica 0–5 V  │
│ 13       │ RC2  │ Salida PWM (CCP1) → driver ventilador  │
│ 17       │ RC6  │ USART TX → RX del módulo HC-06         │
│ 18       │ RC7  │ USART RX ← TX del módulo HC-06         │
│ 21       │ RB0  │ IN1 motor a pasos (bobina A+)          │
│ 22       │ RB1  │ IN2 motor a pasos (bobina A-)          │
│ 23       │ RB2  │ IN3 motor a pasos (bobina B+)          │
│ 24       │ RB3  │ IN4 motor a pasos (bobina B-)          │
└──────────┴──────┴────────────────────────────────────────┘
```

---

## Cómo funciona el ADC

### Principio básico

El convertidor analógico-digital (ADC) del PIC16F873A mide una tensión continua (0 a 5 V) y la convierte en un número entero de **10 bits** (0 a 1023), donde:
- `0`    →   0 mV  (GND)
- `1023` →  5000 mV (VDD)

### Configuración de registros

```c
static void init_adc(void)
{
    ADCON1 = 0x80u;  // ADFM=1: resultado justificado a la derecha
                     // PCFG=0000: AN0–AN4 son analógicos, Vref = VDD (5 V)
    ADCON0 = 0x41u;  // ADCS=01 → reloj ADC = Fosc/8 (TAD = 2 µs)
                     // Canal 0, ADC encendido
}
```

**¿Por qué Fosc/8?**
La hoja de datos del PIC16F873A exige que el período de conversión TAD sea de al menos **1.6 µs**. Con Fosc = 4 MHz:
- Fosc/8 → TAD = 2 µs ✔ (cumple el mínimo)

### Registros del ADC

#### ADCON0 — A/D Control Register 0 (dirección: 0x1F)

| Bit | Nombre | Función |
|---|---|---|
| 7:6 | **ADCS1:ADCS0** | Selección del reloj de conversión (TAD) |
| 5:3 | **CHS2:CHS0** | Canal analógico seleccionado |
| 2 | **GO/DONE** | Inicia/indica estado de conversión |
| 1 | — | No implementado |
| 0 | **ADON** | Enciende/apaga el módulo ADC |

**ADCS1:ADCS0 — Reloj de conversión:**

| ADCS1 | ADCS0 | Reloj ADC | TAD @ 4 MHz | Uso |
|---|---|---|---|---|
| 0 | 0 | Fosc/2 | 0.5 µs | Demasiado rápido (< mín 1.6 µs) |
| 0 | 1 | **Fosc/8** | **2 µs** | **← usado aquí** |
| 1 | 0 | Fosc/32 | 8 µs | Válido, pero lento |
| 1 | 1 | FRC (RC interno) | ~4 µs | Sin dependencia de Fosc |

**CHS2:CHS0 — Selección de canal:**

| CHS2:0 | Canal | Pin |
|---|---|---|
| 000 | AN0 | RA0 |
| 001 | AN1 | RA1 |
| 010 | AN2 | RA2 |
| 011 | AN3 | RA3 |
| 100 | AN4 | RA5 |

En `adc_read(ch)` se calcula `ch << 3` para posicionar el número de canal en los bits 5:3.

**GO/DONE:**
- Escribir `1` dispara la conversión. El hardware lo borra automáticamente al terminar (~12 TAD). El lazo `while (ADCON0bits.GO_nDONE)` es una espera activa sobre este bit.

**Valor utilizado:** `ADCON0 = 0x41` = `0b01000001` → ADCS=01, CHS=000, ADON=1

---

#### ADCON1 — A/D Control Register 1 (dirección: 0x9F)

| Bit | Nombre | Función |
|---|---|---|
| 7 | **ADFM** | Formato de justificación del resultado |
| 6:4 | — | No implementados |
| 3:0 | **PCFG3:PCFG0** | Configuración de pines analógicos y tensión de referencia |

**ADFM — Justificación del resultado:**

| ADFM | Formato | ADRESH | ADRESL |
|---|---|---|---|
| 1 (derecha) | `000000b9b8` \| `b7b6b5b4b3b2b1b0` | bits 9:8 en posición 1:0 | bits 7:0 |
| 0 (izquierda) | `b9b8b7b6b5b4b3b2` \| `b1b0000000` | bits 9:2 | bits 1:0 en posición 7:6 |

Con ADFM=1 (usado aquí), el código `(ADRESH << 8) | ADRESL` entrega directamente el entero de 10 bits.

**PCFG3:PCFG0 — Configuración analógica:**

Con `PCFG = 0000` (resultado de `ADCON1 = 0x80`), los pines AN0–AN4 son entradas analógicas y la referencia de tensión es VDD/VSS (0–5 V). Los demás valores de PCFG permiten dedicar AN2 y AN3 como entradas de Vref externo.

**Valor utilizado:** `ADCON1 = 0x80` = `0b10000000` → ADFM=1, PCFG=0000

---

#### ADRESH / ADRESL — Registros de resultado A/D

Con ADFM=1 (justificado a la derecha):

```
  ADRESH [7:0]              ADRESL [7:0]
┌──┬──┬──┬──┬──┬──┬──┬──┐ ┌──┬──┬──┬──┬──┬──┬──┬──┐
│0 │0 │0 │0 │0 │0 │b9│b8│ │b7│b6│b5│b4│b3│b2│b1│b0│
└──┴──┴──┴──┴──┴──┴──┴──┘ └──┴──┴──┴──┴──┴──┴──┴──┘
             bits altos ↑   ↑ bits bajos
```

El resultado de 10 bits se obtiene con:
```c
(uint16_t)(((uint16_t)ADRESH << 8u) | ADRESL)
```

### Lectura canal por canal — `adc_read(ch)`

```c
static uint16_t adc_read(uint8_t ch)
{
    ADCON0 = 0x41u | (uint8_t)(ch << 3u);  // seleccionar canal (bits CHS2:CHS0)
    __delay_us(30);                          // tiempo de adquisición (carga del condensador de muestreo)
    ADCON0bits.GO_nDONE = 1u;               // iniciar conversión
    while (ADCON0bits.GO_nDONE);            // esperar fin de conversión (~12 TAD = 24 µs)
    return (uint16_t)(((uint16_t)ADRESH << 8u) | ADRESL);  // resultado de 10 bits
}
```

**Pasos internos del ADC:**
1. Se selecciona el canal deseado escribiendo en los bits `CHS2:CHS0` de `ADCON0`.
2. Se espera **30 µs** para que el condensador interno de muestreo se cargue completamente a la tensión de entrada.
3. Se activa el bit `GO_nDONE` para que el hardware inicie la conversión.
4. El ADC realiza una **aproximación sucesiva de 10 bits** (~12 TAD ≈ 24 µs).
5. El resultado queda en el par de registros `ADRESH:ADRESL` justificado a la derecha (los 10 bits bajos son válidos).

### Conversión a milivoltios

Dentro del comando `R` (en `process_cmd`):

```c
mv[i] = (uint16_t)(((uint32_t)adc_read(i) * 5000uL) / 1023uL);
```

La fórmula es:

```
Tensión (mV) = valor_raw × 5000 / 1023
```

Se usa `uint32_t` para el producto intermedio porque `1023 × 5000 = 5 115 000`, que no cabe en 16 bits.

---

## Cómo funciona el PWM

### Principio básico — ¿Qué es el ciclo de trabajo?

La modulación por ancho de pulso (PWM) genera una señal digital que alterna entre alto (5 V) y bajo (0 V) a una **frecuencia fija**. Lo que varía no es la frecuencia, sino cuánto tiempo por ciclo la señal permanece en alto. Ese tiempo se expresa como porcentaje del período total y se llama **ciclo de trabajo** (*duty cycle*).

```
Ciclo de trabajo 0 % — señal siempre en bajo:
________________________________________________________________________________
0V

Ciclo de trabajo 25 %:
 ___             ___             ___             ___
|   |___________|   |___________|   |___________|   |___________
←T/4→←  3T/4  →

Ciclo de trabajo 50 %:
 _______         _______         _______         _______
|       |_______|       |_______|       |_______|       |_______
←  T/2 →← T/2 →

Ciclo de trabajo 75 %:
 ___________     ___________     ___________     ___________
|           |___|           |___|           |___|           |___
←   3T/4   →T/4→

Ciclo de trabajo 100 % — señal siempre en alto:
‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
5V
```

#### ¿Por qué PWM controla la velocidad de un ventilador?

Un ventilador de corriente continua tiene un transistor de potencia (MOSFET en este circuito, o un driver L298N) entre el motor y la alimentación. La señal PWM activa y desactiva ese transistor a alta frecuencia (1 kHz aquí). El motor tiene **inercia mecánica y eléctrica**: no puede arrancar y parar 1000 veces por segundo; en cambio, integra la energía media que recibe. El resultado es que gira a una velocidad proporcional al porcentaje de tiempo que el transistor conduce:

```
Potencia media ≈ VDD × I × (duty cycle / 100)
```

A 0 % el ventilador está apagado; a 100 % gira a máxima velocidad; a 50 % recibe la mitad de la potencia disponible. La frecuencia de 1 kHz es lo suficientemente alta para que el motor no vibre audiblemente por el switching.

### Configuración Timer2 + CCP1

El módulo CCP1 del PIC en modo PWM usa **Timer2** para generar el período:

```c
#define PWM_PR2   249u   // registro de período del Timer2

static void init_pwm(void)
{
    PR2     = PWM_PR2;  // período = (PR2+1) × 4 × Tosc × prescaler
    CCPR1L  = 0x00u;    // duty inicial = 0 % (ventilador apagado)
    CCP1CON = 0x0Cu;    // CCP1 en modo PWM
    T2CON   = 0x05u;    // Timer2 ON, prescaler 1:4
}
```

**Cálculo del período:**

```
Tosc = 1/Fosc = 1/4 MHz = 0.25 µs
Período = (PR2 + 1) × 4 × Tosc × prescaler
        = (249 + 1) × 4 × 0.25 µs × 4
        = 250 × 4 × 0.25 × 4
        = 1000 µs  →  frecuencia = 1 kHz
```

### Registros del PWM

#### PR2 — Timer2 Period Register (dirección: 0x92)

Registro de 8 bits que define el **tope de conteo** del Timer2. Cuando el contador interno TMR2 alcanza el valor de PR2, se resetea a 0 y se emite una señal de "período completado" al módulo CCP1.

```
Período PWM = (PR2 + 1) × 4 × Tosc × prescaler_T2
            = (249  + 1) × 4 × 0.25 µs × 4
            = 1000 µs  →  1 kHz
```

El valor máximo es 255 (contador de 8 bits); la resolución del duty cycle en bits viene dada por `log2(PR2 + 1)`.

---

#### T2CON — Timer2 Control Register (dirección: 0x12)

| Bit | Nombre | Función |
|---|---|---|
| 7 | — | No implementado |
| 6:3 | **TOUTPS3:TOUTPS0** | Postscaler de la señal de desbordamiento Timer2 |
| 2 | **TMR2ON** | Encendido del Timer2 |
| 1:0 | **T2CKPS1:T2CKPS0** | Prescaler de entrada del Timer2 |

**T2CKPS — Prescaler:**

| T2CKPS1 | T2CKPS0 | Prescaler | Efecto |
|---|---|---|---|
| 0 | 0 | 1:1 | Timer2 incrementa cada Tosc×4 = 1 µs |
| 0 | 1 | **1:4** | Timer2 incrementa cada 4 µs **← usado aquí** |
| 1 | x | 1:16 | Timer2 incrementa cada 16 µs |

El prescaler divide la frecuencia base `Fosc/4` antes de alimentar al contador TMR2.

**TOUTPS — Postscaler:**
El postscaler divide la señal de desbordamiento de Timer2 para generar interrupciones de Timer2 menos frecuentes. Para el PWM, el postscaler no interviene: el módulo CCP1 lee directamente TMR2 sin postscaler. En este proyecto el postscaler se deja en 1:1 (`TOUTPS=0000`).

**Valor utilizado:** `T2CON = 0x05` = `0b00000101` → TOUTPS=0000, TMR2ON=1, T2CKPS=01

---

#### CCPR1L — CCP Register 1 Low (dirección: 0x15)

Contiene los **8 bits más significativos** del valor de duty cycle de 10 bits. En el modo PWM, el hardware compara continuamente `{CCPR1L, DC1B1, DC1B0}` con el contador TMR2 para determinar cuándo poner el pin en bajo dentro del período:

```
Ciclo PWM (simplificado):
  TMR2 = 0        →  pin CCP1 se pone en ALTO
  TMR2 = duty[9:0] →  pin CCP1 se pone en BAJO
  TMR2 = PR2       →  TMR2 se resetea, comienza nuevo ciclo
```

---

#### CCP1CON — CCP1 Control Register (dirección: 0x17)

| Bit | Nombre | Función |
|---|---|---|
| 7:6 | — | No implementados |
| 5:4 | **DC1B1:DC1B0** | Bits 1 y 0 (LSBs) del duty cycle de 10 bits |
| 3:0 | **CCP1M3:CCP1M0** | Selección de modo de operación del módulo CCP1 |

**DC1B1:DC1B0** son los 2 bits de menor peso del word de duty. Combinados con CCPR1L forman el valor completo:

```
Duty de 10 bits:  [ CCPR1L[7:0] | DC1B1 | DC1B0 ]
                    ↑ 8 bits altos   ↑ 2 bits bajos
```

**CCP1M — Modos de operación:**

| CCP1M3:0 | Modo |
|---|---|
| `0000` | Módulo deshabilitado |
| `0100` | Captura, flanco de bajada |
| `0101` | Captura, flanco de subida |
| `1000`–`1011` | Comparación |
| **`1100`** | **PWM ← usado aquí** |
| `1101`–`1111` | PWM (variantes de polaridad) |

`CCP1M = 1100` activa el modo PWM estándar: el pin RC2/CCP1 se controla automáticamente por hardware sin intervención de la CPU.

**Valor utilizado:** `CCP1CON = 0x0C` = `0b00001100` → DC1B=00, CCP1M=1100 (PWM)

---

#### Relación entre registros — generación del PWM

```
Fosc/4 (1 MHz)
    │
    ▼
[Prescaler T2CKPS=1:4]  → TMR2 incrementa cada 4 µs
    │
    ▼
[TMR2 contador 8 bits] ←── se resetea cuando TMR2 = PR2 (249)
    │
    ├─→ comparado con {CCPR1L, DC1B1, DC1B0}
    │         Si TMR2 < duty  → CCP1 = 1 (alto)
    │         Si TMR2 ≥ duty  → CCP1 = 0 (bajo)
    │
    └─→ pin RC2/CCP1 ──→ MOSFET/L298N ──→ ventilador
```

### Resolución del ciclo de trabajo — por qué es de 10 bits y por qué el máximo es 1000, no 1023

#### El problema de resolución con un temporizador de 8 bits

Si el ciclo de trabajo se controlara directamente con TMR2 (8 bits, valores 0–249), solo habría 250 niveles posibles. Eso representa una resolución de `1/250 = 0.4 %` por paso, lo cual puede ser aceptable, pero el hardware del PIC ofrece **4 veces más resolución** sin coste adicional de silicio.

#### La solución: el contador de fase interno (bits Q)

Internamente, el PIC tiene un contador de 2 bits (llamado "Q counter" en la hoja de datos) que corre a la velocidad del **reloj de instrucciones** (Fosc/4 = 1 MHz en este proyecto, un pulso cada 1 µs). Este contador Q vale 0, 1, 2, 3, 0, 1, 2, 3… continuamente.

El prescaler de Timer2 (1:4) hace que TMR2 avance **una unidad cada 4 ciclos** del reloj de instrucciones. Dentro de esos 4 ciclos, el contador Q recorre los valores 0→1→2→3. Esto significa que entre dos avances consecutivos de TMR2 hay 4 "momentos" distinguibles.

El comparador de hardware del módulo CCP1 usa una cantidad de **10 bits** formada por:

```
Contador interno de 10 bits = { TMR2[7:0] , Q[1:0] }
                                  8 bits      2 bits
```

Este contador de 10 bits avanza cada microsegundo (cada ciclo de instrucción):

```
Ciclo de instrucción:   0    1    2    3    4    5    6    7    8   ...
TMR2:                   0    0    0    0    1    1    1    1    2   ...
Q counter:              0    1    2    3    0    1    2    3    0   ...
Valor de 10 bits:       0    1    2    3    4    5    6    7    8   ...
```

Cuando TMR2 llega a PR2 (249) y Q a 3, el contador de 10 bits ha alcanzado el valor:
```
249 × 4 + 3 = 999
```
En el siguiente ciclo, TMR2 se resetea a 0 (período completo). Por lo tanto:

```
Rango útil del contador de 10 bits = 0 a 999
Número de niveles = 1000  (= (PR2 + 1) × 4 = 250 × 4)
```

#### Por qué 10 bits y no exactamente 2^10 = 1024 niveles

Los 10 bits del campo de duty cycle en hardware pueden representar valores de 0 a 1023. Sin embargo, el contador interno nunca supera 999 (porque TMR2 se resetea al llegar a PR2=249). Esto significa:

| Valor del campo duty (10 bits) | Comportamiento |
|---|---|
| 0 | Salida siempre en bajo (0 % práctico) |
| 1 – 999 | Ciclo parcial; duty real = valor / 1000 |
| ≥ 1000 | Comparador nunca dispara → salida siempre en alto (100 %) |
| 1001–1023 | También dan 100 % (rango no útil) |

El nombre "10 bits" describe el **ancho del registro** (CCPR1L + DC1B1:DC1B0 = 8+2 bits), no el número exacto de niveles válidos. Con PR2=249, los niveles válidos son 1000 (de 0 a 999), que es una resolución de `1/1000 = 0.1 %` por paso.

#### Relación entre el valor de 10 bits, el tiempo y el porcentaje

Según la hoja de datos del PIC16F873A, la fórmula del tiempo en alto es:

```
Tiempo de duty (µs) = valor_10bits × Tosc × prescaler_T2
                    = valor_10bits × 0.25 µs × 4
                    = valor_10bits × 1 µs
```

Para calcular el valor de 10 bits a partir de un porcentaje deseado:

```
valor_10bits = (porcentaje / 100) × período_en_µs
             = (porcentaje / 100) × 1000
             = porcentaje × 10
```

| Duty deseado | valor_10bits correcto | Tiempo en alto |
|---|---|---|
| 0 % | 0 | 0 µs |
| 25 % | 250 | 250 µs |
| 50 % | 500 | 500 µs |
| 75 % | 750 | 750 µs |
| 100 % | 1000 | 1000 µs (100 % del período) |

#### La fórmula en el código (corregida)

La función `fan_set_pct` debe convertir el porcentaje (0–100) al word de duty de 10 bits (0–1000). Para ello multiplica por el factor ×4 que representa la subdivisión Q (los dos sub-bits `Tosc/4` de resolución del CCP):

```c
uint16_t duty = (uint16_t)(((uint32_t)pct * (PWM_PR2 + 1u) * 4u) / 100u);
//            = pct × 1000 / 100  =  pct × 10
```

> **Importante — uso de `uint32_t`:** El producto intermedio puede llegar a `100 × 250 × 4 = 100 000`, que **no cabe en 16 bits** (máx. 65 535). Por eso el cálculo se hace en `uint32_t` y solo el resultado final (0–1000) se reduce a `uint16_t`.

> **Nota histórica (bug corregido):** Una versión previa usaba `pct * (PWM_PR2 + 1) / 100`, sin el factor ×4. Eso producía un máximo de **250** (25 % de duty real) en lugar de 1000, por lo que el ventilador nunca pasaba del 25 % de su potencia. La fórmula de arriba corrige ese error y restaura el rango completo 0–100 %.

### Función `fan_set_pct(pct)`

```c
static void fan_set_pct(uint8_t pct)
{
    // Convertir porcentaje (0–100) al word de duty de 10 bits (0–1000)
    // El ×4 cubre los dos sub-bits Tosc/4 de resolución del CCP.
    // El cast a uint32 evita el desbordamiento de 16 bits de 100 × 1000.
    uint16_t duty = (uint16_t)(((uint32_t)pct * (PWM_PR2 + 1u) * 4u) / 100u);

    CCPR1L  = (uint8_t)(duty >> 2u);              // 8 bits altos del duty
    CCP1CON = (CCP1CON & 0xCFu)                   // borrar bits DC1B1:DC1B0
            | (uint8_t)((duty & 0x03u) << 4u);    // escribir los 2 bits bajos
}
```

**Ejemplo al 50 %:**
```
duty = 50 × 1000 / 100 = 500  (0b0111110100)
CCPR1L  = 500 >> 2 = 125  (0b01111101)
DC1B    = 500 & 3  = 0    → bits [5:4] de CCP1CON = 00
```

El hardware compara continuamente el Timer2 con el valor de duty y controla el pin RC2 automáticamente, **sin intervención de la CPU durante el ciclo**.

---

## Cómo funciona la comunicación UART / Bluetooth

### Configuración física

El módulo HC-06 es un adaptador Bluetooth-UART. Se conecta directamente a los pines TX (RC6) y RX (RC7) del PIC. Desde la perspectiva del firmware, es simplemente un puerto serie asíncrono.

```c
#define UART_SPBRG   25u   // SPBRG = Fosc / (16 × baud) − 1
                           //       = 4 000 000 / (16 × 9600) − 1
                           //       = 26.04... − 1 ≈ 25  →  9615 baud (error < 0.2 %)

static void init_uart(void)
{
    SPBRG = UART_SPBRG;
    TXSTA = 0x24u;   // TXEN=1 (TX habilitado), BRGH=1 (alta velocidad), modo asíncrono
    RCSTA = 0x90u;   // SPEN=1 (serial habilitado), CREN=1 (recepción continua)
}
```

**¿Qué es BRGH?**
El bit BRGH (High Baud Rate) selecciona la fórmula del divisor de baud rate. Con BRGH=1 se usa:
```
SPBRG = Fosc / (16 × baud) − 1
```
Con BRGH=0 sería `/64` en lugar de `/16`, generando mayor error a altas velocidades.

### Registros de la UART

#### SPBRG — Serial Port Baud Rate Generator (dirección: 0x99)

Registro de 8 bits que actúa como divisor del reloj del sistema para producir la frecuencia de bits (baud rate). Es el único parámetro que el usuario configura para ajustar la velocidad de comunicación.

| BRGH | Fórmula | SPBRG para 9600 baud @ 4 MHz | Baud real | Error |
|---|---|---|---|---|
| 1 | `Fosc / (16 × (SPBRG+1))` | 25 | 9615 | 0.16 % |
| 0 | `Fosc / (64 × (SPBRG+1))` | 6 | 8929 | 7.0 % |

Con BRGH=1 el error es menor de 2 % (límite recomendado para UART asíncrono), mientras que con BRGH=0 el error superaría el umbral y causaría errores de trama a velocidades mayores.

---

#### TXSTA — Transmit Status and Control Register (dirección: 0x98)

| Bit | Nombre | Función |
|---|---|---|
| 7 | **CSRC** | Fuente de reloj (solo modo síncrono, irrelevante aquí) |
| 6 | **TX9** | Habilita transmisión de 9 bits |
| 5 | **TXEN** | Habilita el transmisor |
| 4 | **SYNC** | Modo síncrono/asíncrono |
| 3 | — | No implementado |
| 2 | **BRGH** | Selector de alta velocidad de baud rate |
| 1 | **TRMT** | Estado del registro de desplazamiento TX (solo lectura) |
| 0 | **TX9D** | Noveno bit de datos transmitidos |

**TX9:** Con `TX9=0` se transmiten tramas de 8 bits de datos (formato estándar 8N1 junto con SYNC=0). Con `TX9=1` se añade un noveno bit de datos (utilizado para detección de dirección en redes multidrop).

**TXEN:** Al activar este bit, el pin RC6 pasa de GPIO a función TX de la USART. Desactivarlo aborta cualquier transmisión en curso.

**SYNC:** `0` = modo asíncrono (UART convencional, sin pin de reloj separado). Los datos se envían con bit de inicio, datos y bit de parada. `1` = modo síncrono (tipo SPI, requiere pin RC6 como reloj).

**BRGH:** Selecciona el divisor del baud rate generator. Afecta la fórmula aplicada a SPBRG.

**TRMT:** Indica si el Transmit Shift Register (TSR) está libre. Difiere de TXIF: TXIF indica que TXREG puede aceptar un nuevo byte; TRMT indica que el TSR terminó de serializar el último byte. En este código se usa TXIF para la espera (no TRMT).

**Valor utilizado:** `TXSTA = 0x24` = `0b00100100` → TX9=0, TXEN=1, SYNC=0, BRGH=1

---

#### RCSTA — Receive Status and Control Register (dirección: 0x18)

| Bit | Nombre | Función |
|---|---|---|
| 7 | **SPEN** | Habilita el puerto serie completo |
| 6 | **RX9** | Habilita recepción de 9 bits |
| 5 | **SREN** | Recepción de un solo byte (solo modo síncrono) |
| 4 | **CREN** | Habilita recepción continua |
| 3 | **ADDEN** | Detección de dirección (solo modo 9 bits) |
| 2 | **FERR** | Flag de error de trama (solo lectura) |
| 1 | **OERR** | Flag de error de desbordamiento (solo lectura) |
| 0 | **RX9D** | Noveno bit del dato recibido |

**SPEN:** Al activar este bit, RC6 (TX) y RC7 (RX) pasan automáticamente a función USART, independientemente del valor de TRISC. Si SPEN=0, ambos pines son GPIO ordinarios.

**CREN:** Con `CREN=1` el receptor acepta bytes de forma continua: en cuanto termina de recibir un byte, el hardware está listo para el siguiente sin intervención del software. Con `CREN=0` la recepción se detiene. Esta es la forma estándar de **limpiar el flag OERR**: hacer `CREN=0` luego `CREN=1`. Si se limpia solo el flag sin este toggle, el módulo de recepción no se recupera.

**FERR — Error de trama:** Se activa cuando el bit de parada (stop bit) recibido es 0 en lugar de 1. Indica que el PIC y el emisor no están sincronizados en el mismo baud rate, o que hay ruido en la línea. Se lee junto con el dato de RCREG y se actualiza con el siguiente byte.

**OERR — Error de desbordamiento:** El buffer de recepción RCREG es un FIFO de 2 posiciones. Si llegan 3 bytes sin que el software haya leído RCREG, el tercer byte se pierde y OERR se activa. El módulo de recepción queda bloqueado hasta el toggle de CREN. En la ISR del proyecto se detecta y corrige automáticamente:
```c
if (RCSTAbits.OERR) {
    RCSTAbits.CREN = 0u;
    RCSTAbits.CREN = 1u;
}
```

**Valor utilizado:** `RCSTA = 0x90` = `0b10010000` → SPEN=1, CREN=1

---

#### TXREG — Transmit Register (dirección: 0x19)

Buffer de escritura del transmisor. Cuando el software escribe un byte aquí, el hardware lo transfiere automáticamente al **TSR** (Transmit Shift Register), que lo serializa bit a bit a la frecuencia definida por SPBRG.

TXREG y TSR forman un **pipeline de 2 etapas**: se puede escribir el siguiente byte en TXREG mientras el TSR transmite el byte anterior, maximizando el uso del canal sin intervención de la CPU byte a byte.

```
CPU escribe → [TXREG] → (auto-transfer) → [TSR] → bit a bit → pin RC6/TX → HC-06
                ↑                                                       
           TXIF=1 cuando                                                
           TXREG está libre                                             
```

---

#### RCREG — Receive Register (dirección: 0x1A)

Buffer de lectura del receptor. Implementado internamente como un **FIFO de 2 posiciones**: puede almacenar 2 bytes completos antes de señalizar desbordamiento. Si el software no lee RCREG a tiempo y llega un tercer byte, se activa OERR.

```
pin RC7/RX → bit a bit → [RSR (Receive Shift Register)] → [RCREG FIFO 2 pos.] → CPU lee
                                                                    ↑
                                                               RCIF=1 cuando
                                                               hay al menos 1 byte
```

Leer RCREG limpia RCIF automáticamente **cuando el FIFO queda vacío** (si quedan bytes pendientes en el FIFO, RCIF permanece en 1 para indicar que hay más datos disponibles).

---

#### PIR1 — Peripheral Interrupt Request Register (dirección: 0x0C)

Contiene los flags de estado de los periféricos. Los dos relevantes para la UART son:

| Bit | Nombre | Condición de activación | Cómo se limpia |
|---|---|---|---|
| 5 | **RCIF** | RCREG tiene al menos 1 byte | Al leer RCREG (cuando el FIFO queda vacío) |
| 4 | **TXIF** | TXREG está vacío y listo | Al escribir en TXREG |

Ambos flags son de **solo lectura**: el hardware los gestiona automáticamente.

- **RCIF** desencadena la ISR (cuando RCIE=1 y PEIE=1 y GIE=1). En el código se lee RCREG dentro de la ISR, lo que limpia RCIF.
- **TXIF** se sondea en `uart_putc()` para esperar a que el transmisor esté libre antes de escribir el siguiente byte.

---

#### PIE1 — Peripheral Interrupt Enable Register (dirección: 0x8C)

Máscara que habilita o deshabilita cada fuente de interrupción periférica individualmente. Solo el bit RCIE es relevante aquí:

| Bit | Nombre | Función |
|---|---|---|
| 5 | **RCIE** | Habilita interrupción por RCIF (recepción UART) |

Con `PIE1bits.RCIE = 1` cada byte recibido dispara la ISR. Requiere también que `PEIE=1` y `GIE=1` en INTCON.

---

#### INTCON — Interrupt Control Register (dirección: 0x0B)

Registro central de control de interrupciones. Los bits usados en este proyecto:

| Bit | Nombre | Función |
|---|---|---|
| 7 | **GIE** | Habilitación global de todas las interrupciones |
| 6 | **PEIE** | Habilitación de interrupciones de periféricos (PIR1/PIR2) |
| 5 | **T0IE** | Habilitación de interrupción por desbordamiento Timer0 |
| 2 | **T0IF** | Flag de desbordamiento Timer0 (debe limpiarse manualmente en ISR) |

**GIE** es el interruptor maestro. Desactivarlo (como hace `process_cmd()` durante la copia del buffer) bloquea todas las interrupciones, garantizando una sección crítica sin condiciones de carrera.

**PEIE** es necesario además de GIE para que las interrupciones de periféricos (como RCIF) puedan llegar a la CPU. Sin PEIE=1, la interrupción de recepción UART no se dispararía aunque GIE=1 y RCIE=1.

```
Cadena de habilitación para que RCIF genere interrupción:
  GIE=1  AND  PEIE=1  AND  RCIE=1  AND  RCIF=1  →  ISR se ejecuta
```

### Protocolo de comandos

La aplicación Android envía comandos de texto terminados con `'\n'`:

| Comando | Dirección | Descripción |
|---|---|---|
| `F<0–100>` | Android → PIC | Ajusta velocidad del ventilador en porcentaje |
| `M<+/-><N>` | Android → PIC | Mueve el motor a pasos N pasos (+ = CW, − = CCW) |
| `R` | Android → PIC | Solicita lectura de los 4 canales ADC |
| `V0:<mV>,V1:<mV>,V2:<mV>,V3:<mV>\n` | PIC → Android | Respuesta con las 4 tensiones en milivoltios |

### Recepción — buffer de comandos en la ISR

La ISR acumula caracteres recibidos en `cmd_buf[]`:

```c
if (PIR1bits.RCIF)                // hay un byte en RCREG
{
    if (RCSTAbits.OERR)           // error de desbordamiento → resetear CREN
    {
        RCSTAbits.CREN = 0u;
        RCSTAbits.CREN = 1u;
    }

    char c = (char)RCREG;         // leer byte (limpia RCIF automáticamente)

    if (c == '\n' || c == '\r')
    {
        if (cmd_len > 0u && !cmd_ready)
            cmd_ready = 1u;       // señal al lazo principal: comando completo
    }
    else if (!cmd_ready && cmd_len < CMD_SIZE - 1u)
    {
        cmd_buf[cmd_len++] = c;   // acumular carácter
    }
}
```

El flag `cmd_ready` desacopla la ISR del procesamiento: la ISR solo llena el buffer; el lazo principal lo procesa cuando puede.

### Procesamiento — `process_cmd()`

```c
static void process_cmd(void)
{
    char    local[CMD_SIZE];
    uint8_t len;

    // Sección crítica: copiar buffer con interrupciones desactivadas
    INTCONbits.GIE = 0u;
    len = cmd_len;
    for (uint8_t i = 0u; i < len; i++) local[i] = cmd_buf[i];
    cmd_len = 0u;           // liberar buffer para nuevos comandos
    INTCONbits.GIE = 1u;   // reactivar interrupciones

    local[len] = '\0';      // terminar cadena

    switch (local[0])       // el primer carácter determina el tipo de comando
    {
        case 'R': /* leer ADC y transmitir */
        case 'F': /* ajustar PWM */
        case 'M': /* mover motor */
    }
}
```

**¿Por qué copiar con GIE=0?**
Sin la sección crítica, la ISR podría modificar `cmd_buf[]` o `cmd_len` mientras `process_cmd` los está leyendo, corrompiendo el comando. Deshabilitar GIE durante la copia (apenas unos ciclos) elimina esta condición de carrera.

### Transmisión — `uart_putc` / `uart_puts` / `uart_putu`

```c
static void uart_putc(char c)
{
    while (!PIR1bits.TXIF);   // esperar a que el registro TXREG esté libre
    TXREG = (uint8_t)c;       // escribir byte; el hardware lo serializa automáticamente
}
```

`uart_puts` llama a `uart_putc` para cada carácter de una cadena. `uart_putu` convierte un `uint16_t` a dígitos ASCII y los envía de mayor a menor peso (sin usar `printf` para ahorrar ROM).

---

## Cómo funciona el motor a pasos

### Principio básico

Un motor a pasos bifásico tiene **dos bobinas** (A y B). Energizando las bobinas en una secuencia determinada, el rotor avanza un ángulo fijo (el "paso") en cada conmutación. En **paso completo** se energizan ambas bobinas a la vez, maximizando el torque.

### Secuencia de pasos (full-step)

El firmware usa la secuencia de 4 estados almacenada en `STEP_SEQ`:

```c
static const uint8_t STEP_SEQ[4] = {0x03u, 0x06u, 0x0Cu, 0x09u};
//  Estado 0: 0b0011  → IN1=1 IN2=1 IN3=0 IN4=0   (bobinas A+ y A- activas)
//  Estado 1: 0b0110  → IN1=0 IN2=1 IN3=1 IN4=0   (bobinas A- y B+ activas)
//  Estado 2: 0b1100  → IN1=0 IN2=0 IN3=1 IN4=1   (bobinas B+ y B- activas)
//  Estado 3: 0b1001  → IN1=1 IN2=0 IN3=0 IN4=1   (bobinas B- y A+ activas)
```

Estos 4 bits se aplican directamente sobre `RB[3:0]`:

```
RB3 = IN4 (bobina B-)
RB2 = IN3 (bobina B+)
RB1 = IN2 (bobina A-)
RB0 = IN1 (bobina A+)
```

La tabla de conmutación es:

```
Paso │ IN1 │ IN2 │ IN3 │ IN4 │ Hex
─────┼─────┼─────┼─────┼─────┼─────
  0  │  1  │  1  │  0  │  0  │ 0x03
  1  │  0  │  1  │  1  │  0  │ 0x06
  2  │  0  │  0  │  1  │  1  │ 0x0C
  3  │  1  │  0  │  0  │  1  │ 0x09
```

### Dirección de giro

- **Sentido horario (CW):** índice avanza `+1` en la secuencia (0→1→2→3→0→…)
- **Sentido antihorario (CCW):** índice retrocede `−1`, equivalente a sumar `+3` módulo 4 (0→3→2→1→0→…)

```c
if (step_count > 0)       // CW
{
    step_idx = (step_idx + 1u) & 0x03u;
    PORTB = (PORTB & 0xF0u) | STEP_SEQ[step_idx];
    step_count--;
}
else if (step_count < 0)  // CCW
{
    step_idx = (step_idx + 3u) & 0x03u;   // -1 mod 4 = +3 mod 4
    PORTB = (PORTB & 0xF0u) | STEP_SEQ[step_idx];
    step_count++;
}
```

`(PORTB & 0xF0u)` preserva los bits RB4–RB7 (usados para otro propósito o como spare) y solo escribe RB0–RB3.

### Temporización — Timer0

El motor avanza **un paso cada tick del Timer0**, configurado para ~5 ms:

```c
#define T0_RELOAD   100u   // recarga del Timer0

static void init_timer0(void)
{
    // T0CS=0 reloj interno Fosc/4=1MHz, PSA=0 prescaler a TMR0, PS=100 → 1:32
    OPTION_REG = (OPTION_REG & 0xC0u) | 0x04u;
    TMR0 = T0_RELOAD;
    INTCONbits.T0IE = 1u;
}
```

**Cálculo del intervalo:**

```
Fosc/4 = 1 MHz  →  T_base = 1 µs por conteo
Prescaler 1:32  →  TMR0 incrementa cada 32 µs
Cuentas hasta desbordamiento = 256 − 100 = 156
Tiempo = 156 × 32 µs ≈ 4.992 ms  (~5 ms)
```

Velocidad resultante: **1 paso cada ~5 ms = 200 pasos/segundo máximo**.

La ISR pone `step_tick = 1` en cada desbordamiento; el lazo principal lo lee y avanza el motor:

```c
if (step_tick)
{
    step_tick = 0u;
    // ... avanzar un paso si step_count ≠ 0
}
```

### Comando de movimiento

El comando Bluetooth `M<+/-><N>` asigna directamente `step_count`:

```c
case 'M': case 'm':
    step_count = (int16_t)atoi(&local[1]);
    break;
```

`atoi` acepta signo opcional, por lo que `M+200` mueve 200 pasos CW y `M-100` mueve 100 pasos CCW.

**Las bobinas permanecen energizadas** al terminar el movimiento (torque de retención), porque el código no aplica un patrón de cero al detenerse.

---

## Circuitos de potencia (drivers de hardware)

El PIC nunca alimenta directamente cargas de potencia. Sus pines entregan como máximo ~25 mA por pin, mientras que un ventilador o un motor consumen cientos de miliamperios. Por eso entre el microcontrolador y cada carga hay una etapa de potencia.

### Ventilador — transistor 2N2222 + diodo (conmutador de lado bajo)

La salida PWM (RC2/CCP1) controla un único transistor NPN 2N2222 que conmuta el ventilador. El PWM "trocea" la alimentación a 1 kHz y la inercia del motor promedia la energía, fijando la velocidad.

```
            +Vfan (5 V o 12 V)
              │
            [ventilador]
              │
              ●────►│──┐  diodo flyback (cátodo a +Vfan, ánodo al colector)
              │ C       │  (1N4001 / 1N4148)
  RC2 ──[R]──■ B  2N2222 (NPN)
              │ E
             GND  ← masa común con el PIC
```

**Puntos clave:**

- **Resistencia de base R ≈ 330 Ω – 1 kΩ** desde RC2 a la base. Garantiza que el transistor entre en saturación con la corriente de base que el pin del PIC puede entregar (~10–15 mA para un ventilador de 100–200 mA).
- **El diodo flyback es obligatorio.** El ventilador es una carga inductiva; al cortar la corriente 1000 veces por segundo, sin el diodo de descarga los picos de tensión superarían el límite VCEO (~40 V) del 2N2222 y lo destruirían.
- **1 kHz es adecuado para un ventilador DC simple de 2 hilos** (con escobillas). ⚠️ Si fuera un ventilador *brushless* de 3 o 4 hilos (tipo PC), trocear su alimentación a 1 kHz lo haría zumbar o comportarse de forma errática: esos ventiladores esperan alimentación constante y un hilo de control PWM separado.
- **Masa común** entre el PIC y la fuente del ventilador, o el transistor no conmuta de forma fiable.
- **Límite de corriente:** el 2N2222A admite hasta ~800 mA de colector; mantén la corriente del ventilador con margen por debajo.

### Motor a pasos — 28BYJ-48 + tarjeta ULN2003

El motor es un **28BYJ-48 unipolar de 5 V** controlado por la tarjeta driver **ULN2003** que lo acompaña. Los pines RB0–RB3 se conectan a IN1–IN4 de la tarjeta, en ese orden.

```
PIC                ULN2003 (array Darlington, sumidero)        Motor 28BYJ-48
RB0 ──────────────► IN1 ──► OUT1 ─────────────────────────────► fase 1
RB1 ──────────────► IN2 ──► OUT2 ─────────────────────────────► fase 2
RB2 ──────────────► IN3 ──► OUT3 ─────────────────────────────► fase 3
RB3 ──────────────► IN4 ──► OUT4 ─────────────────────────────► fase 4
                    COM ◄────────────────────────── +5 V ──────► común (center-tap)
                    GND ◄────────────────────────── masa común con el PIC
```

**Cómo funciona la tarjeta:** El ULN2003 es un array de transistores Darlington que actúan como **sumideros** (drenan corriente a masa). Una entrada en ALTO activa su salida, que conecta esa fase a tierra; el común del motor va a +5 V. Por eso un `1` lógico en RBx energiza la fase correspondiente, exactamente como espera la tabla `STEP_SEQ`.

**La secuencia del código es correcta para este motor:** `STEP_SEQ = {0x03, 0x06, 0x0C, 0x09}` es la secuencia canónica de paso completo (dos fases activas) del 28BYJ-48 con IN1–IN4 conectados en orden.

**Consideraciones prácticas del 28BYJ-48:**

- **El engranaje cambia el significado de "N pasos".** El 28BYJ-48 tiene una reducción interna (~64:1), de modo que requiere **~2048 pasos completos por vuelta** del eje de salida. Por lo tanto:
  - `M200` ≈ **35°** de giro del eje.
  - `M2048` ≈ una vuelta completa.
- **La velocidad es segura.** 5 ms/paso = 200 pasos/s ≈ **5.9 RPM** en el eje de salida, muy dentro del rango del motor, así que no se atasca pese a no tener rampa de aceleración.
- **El flyback ya está resuelto.** El ULN2003 incluye diodos de descarga internos (el pin COM se conecta a +5 V). No hay que añadir diodos externos.
- **Aliméntalo con una fuente de 5 V real**, no con el regulador del PIC: el motor consume ~200–300 mA. Comparte la masa con el PIC.
- **Calor en reposo (opcional):** las bobinas quedan energizadas al terminar el movimiento (torque de retención), por lo que el motor y el ULN2003 se calientan en reposo. Si no necesitas par de mantenimiento, añade `PORTB &= 0xF0u;` cuando `step_count` llegue a 0 para desenergizar.
- **Si solo vibra en lugar de girar:** intercambia los hilos IN2↔IN3 (o reordena la tabla). Es el típico problema de orden de fases del 28BYJ-48; con el cableado IN1–IN4 directo esta secuencia es la estándar y debería funcionar.

### Requisitos a nivel de placa (imprescindibles)

Aunque el firmware sea correcto, estos elementos de hardware suelen ser la causa de que "compile pero no funcione":

| Elemento | Detalle | Por qué |
|---|---|---|
| **Divisor en RX del HC-06** | El TX del PIC (RC6) entrega 5 V; bájalo a ~3.3 V (divisor 1 kΩ / 2 kΩ) hacia el RX del HC-06 | El RX del HC-06 normalmente **no tolera 5 V** y se daña. El sentido contrario (TX del HC-06 a 3.3 V → RX del PIC) sí funciona |
| **Pull-up en MCLR** | Resistencia ~10 kΩ desde el pin 1 (MCLR) a VDD | En el 873A el reset es siempre externo; si MCLR queda flotante, el chip no arranca |
| **Capacitores del cristal** | Dos condensadores ~22 pF del cristal de 4 MHz a masa | Sin ellos el oscilador puede no arrancar de forma fiable |
| **Desacoplo de alimentación** | 100 nF entre VDD y VSS, cerca del chip | Estabiliza la alimentación y evita resets espurios |

---

## Rutina de interrupción (ISR)

```c
void __interrupt() isr(void)
{
    // ── Fuente 1: Timer0 overflow (~cada 5 ms) ──────────────────────────────
    if (INTCONbits.T0IF)
    {
        INTCONbits.T0IF = 0u;   // limpiar flag
        TMR0 = T0_RELOAD;       // recargar para mantener el período
        step_tick = 1u;         // señal al lazo principal
    }

    // ── Fuente 2: USART receive ──────────────────────────────────────────────
    if (PIR1bits.RCIF)
    {
        if (RCSTAbits.OERR)     // overrun: el buffer de HW se llenó → resetear
        {
            RCSTAbits.CREN = 0u;
            RCSTAbits.CREN = 1u;
        }
        char c = (char)RCREG;   // leer byte (limpia RCIF)
        // ... acumular en cmd_buf[]
    }
}
```

El PIC16F873A tiene una sola interrupción vectorizada; la ISR revisa manualmente cada flag. Las dos fuentes activas son:
- `T0IF` — desbordamiento del Timer0 (tick del motor)
- `RCIF`  — byte recibido por la USART

---

## Función por función

### Inicialización

| Función | Qué hace |
|---|---|
| `init_ports()` | TRISA=0x3F (RA0–RA5 entradas analógicas), TRISB=0x00 (RB todo salidas), TRISC=0x80 (RC7 entrada, resto salidas) |
| `init_adc()` | ADCON1=0x80 (resultado justificado derecha, Vref=VDD, AN0–AN4 analógicos), ADCON0=0x41 (Fosc/8, canal 0, ADC ON) |
| `init_pwm()` | PR2=249, CCPR1L=0 (0%), CCP1CON=0x0C (modo PWM), T2CON=0x05 (Timer2 ON, prescaler 1:4) |
| `init_uart()` | SPBRG=25 (9600 baud), TXSTA=0x24 (TX ON, BRGH=1), RCSTA=0x90 (serial ON, RX continuo) |
| `init_timer0()` | OPTION_REG prescaler 1:32, TMR0=100, habilita interrupción T0IE |

### Helpers UART

| Función | Qué hace |
|---|---|
| `uart_putc(c)` | Espera que TXREG esté libre (flag TXIF) y escribe un byte |
| `uart_puts(s)` | Llama a `uart_putc` para cada carácter de la cadena hasta el `'\0'` |
| `uart_putu(v)` | Convierte `uint16_t` a cadena decimal ASCII y la transmite (sin `printf`) |

### ADC

| Función | Qué hace |
|---|---|
| `adc_read(ch)` | Selecciona canal, espera 30 µs de adquisición, dispara conversión, devuelve valor de 10 bits |

### PWM

| Función | Qué hace |
|---|---|
| `fan_set_pct(pct)` | Calcula word de duty de 10 bits, escribe los 8 bits altos en CCPR1L y los 2 bajos en CCP1CON |

### Control

| Función | Qué hace |
|---|---|
| `isr()` | Atiende Timer0 (tick del motor) y UART RX (acumula caracteres, señaliza `cmd_ready`) |
| `process_cmd()` | Copia buffer con GIE=0 (sección crítica), interpreta el primer carácter y ejecuta la acción correspondiente |
| `main()` | Inicializa todo, habilita interrupciones, envía "OK\n" y entra en el lazo principal que atiende `cmd_ready` y `step_tick` |

---

## Flujo general del programa

```
Arranque
   │
   ├─ init_ports()
   ├─ init_adc()
   ├─ init_pwm()
   ├─ init_uart()
   ├─ init_timer0()
   ├─ Habilitar interrupciones (RCIE, PEIE, GIE)
   └─ uart_puts("OK\n")
        │
        ▼
┌──────────────────────────────────────┐
│           Lazo principal (for ;;)    │
│                                      │
│  ┌─ ¿cmd_ready? ──────────────────┐ │
│  │   Sí → process_cmd()           │ │
│  │        ├─ 'R' → adc × 4       │ │
│  │        │        uart_puts(...)  │ │
│  │        ├─ 'F' → fan_set_pct()  │ │
│  │        └─ 'M' → step_count=N  │ │
│  └────────────────────────────────┘ │
│                                      │
│  ┌─ ¿step_tick? ──────────────────┐ │
│  │   Sí → avanzar/retroceder 1   │ │
│  │        paso si step_count ≠ 0  │ │
│  └────────────────────────────────┘ │
└──────────────────────────────────────┘
         ▲               ▲
         │               │
  ┌──────┴──────┐  ┌──────┴──────┐
  │  ISR T0IF   │  │  ISR RCIF   │
  │ step_tick=1 │  │ acumula buf │
  │ recarga TMR0│  │ cmd_ready=1 │
  └─────────────┘  └─────────────┘
```

El diseño es **completamente no bloqueante** en el lazo principal: las interrupciones gestionan el tiempo real (Timer0 y recepción UART), mientras que el lazo principal consume los eventos cuando están listos sin introducir esperas activas que congelen el sistema.
