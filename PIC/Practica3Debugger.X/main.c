/*
 * File:   main.c
 * Author: irmin
 *
 * PIC16F873A @ 4 MHz crystal (XT mode) — Debugger for Practica3.X
 *
 * Pinout:
 *  Pin  2  RA0  - Button STEP_CW    (active low, 10 kΩ pull-up to VDD)
 *  Pin  3  RA1  - Button STEP_CCW   (active low, 10 kΩ pull-up to VDD)
 *  Pin  4  RA2  - Button FAN_TOGGLE (active low, 10 kΩ pull-up to VDD)
 *  Pin  5  RA3  - Button ADC_CH     (active low, cycle display channel; 10 kΩ pull-up)
 *  Pin 17  RC6/TX  → Target PIC RC7/RX  (9600 8N1)
 *  Pin 18  RC7/RX  ← Target PIC RC6/TX  (9600 8N1)
 *  Pin 21-28 RB0-RB7 - LED bar graph (RB0 = LSB)
 *
 * Commands sent to target:
 *   "M+1\n"  — one CW step  (STEP_CW button)
 *   "M-1\n"  — one CCW step (STEP_CCW button)
 *   "F100\n" — fan on       (FAN_TOGGLE, toggles)
 *   "F0\n"   — fan off
 *   "R\n"    — request ADC readings (sent automatically every ~500 ms)
 *
 * LED bar graph:
 *   Displays the currently selected ADC channel (V0–V3) as a bar
 *   graph on RB0-RB7.  Each LED represents 625 mV (5000 mV / 8 LEDs).
 *   ADC_CH button cycles the displayed channel V0 → V1 → V2 → V3 → V0…
 *
 * LVP must be OFF (same fuse as target) so RB3 is free GPIO.
 */

// ---------------------------------------------------------------------------
// Configuration bits
// ---------------------------------------------------------------------------
#pragma config FOSC  = XT
#pragma config WDTE  = OFF
#pragma config PWRTE = ON
#pragma config CP    = OFF
#pragma config BOREN = ON
#pragma config LVP   = OFF
#pragma config CPD   = OFF
#pragma config WRT   = OFF

#include <xc.h>
#include <stdint.h>
#include <stdlib.h>

#define _XTAL_FREQ  4000000UL

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define UART_SPBRG      25u     // 9600 baud @ 4 MHz, BRGH=1

// Timer0 ~5 ms tick (identical to target)
#define T0_RELOAD       100u

// Automatic ADC request every 100 ticks (~500 ms)
#define ADC_INTERVAL    100u

// Require 3 consecutive ticks (~15 ms) of stable press before firing
#define DEBOUNCE_CNT    3u

#define BTN_STEP_CW     0u
#define BTN_STEP_CCW    1u
#define BTN_FAN         2u
#define BTN_ADC_CH      3u
#define NUM_BUTTONS     4u

#define RX_BUF_SIZE     48u

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static volatile uint8_t tick = 0u;

// Button debounce: cnt counts stable-pressed ticks; armed allows one fire per press
static uint8_t btn_cnt[NUM_BUTTONS]   = {0u, 0u, 0u, 0u};
static uint8_t btn_armed[NUM_BUTTONS] = {1u, 1u, 1u, 1u};

static uint8_t  fan_on    = 0u;
static uint8_t  adc_ch    = 0u;
static uint16_t adc_mv[4] = {0u, 0u, 0u, 0u};
static uint8_t  adc_timer = 0u;

static volatile char    rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_len   = 0u;
static volatile uint8_t rx_ready = 0u;

// ---------------------------------------------------------------------------
// UART helpers
// ---------------------------------------------------------------------------

static void uart_putc(char c)
{
    while (!PIR1bits.TXIF);
    TXREG = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

// ---------------------------------------------------------------------------
// ISR
// ---------------------------------------------------------------------------

void __interrupt() isr(void)
{
    if (INTCONbits.T0IF)
    {
        INTCONbits.T0IF = 0u;
        TMR0 = T0_RELOAD;
        tick = 1u;
    }

    if (PIR1bits.RCIF)
    {
        if (RCSTAbits.OERR)
        {
            RCSTAbits.CREN = 0u;
            RCSTAbits.CREN = 1u;
        }
        char c = (char)RCREG;
        if (c == '\n' || c == '\r')
        {
            if (rx_len > 0u && !rx_ready)
                rx_ready = 1u;
        }
        else if (!rx_ready && rx_len < RX_BUF_SIZE - 1u)
        {
            rx_buf[rx_len++] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Parse "V0:<mV>,V1:<mV>,V2:<mV>,V3:<mV>" from target
// ---------------------------------------------------------------------------

static void parse_response(void)
{
    char    local[RX_BUF_SIZE];
    uint8_t len;

    INTCONbits.GIE = 0u;
    len = rx_len;
    for (uint8_t i = 0u; i < len; i++) local[i] = rx_buf[i];
    rx_len = 0u;
    INTCONbits.GIE = 1u;

    local[len] = '\0';

    if (local[0] != 'V') return;   // ignore "OK" or unexpected responses

    char *p = local;
    for (uint8_t ch = 0u; ch < 4u; ch++)
    {
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;
        adc_mv[ch] = (uint16_t)atoi(p);
        while (*p && *p != ',') p++;
        if (*p) p++;
    }
}

// ---------------------------------------------------------------------------
// Refresh PORTB LED bar graph from adc_mv[adc_ch]
// ---------------------------------------------------------------------------

static void update_leds(void)
{
    uint16_t mv   = adc_mv[adc_ch];
    // Each of 8 LEDs represents 625 mV (5000 / 8)
    uint8_t  bars = (uint8_t)(mv / 625u);
    if (bars > 8u) bars = 8u;

    uint8_t pattern = 0u;
    for (uint8_t i = 0u; i < bars; i++)
        pattern |= (uint8_t)(1u << i);
    PORTB = pattern;
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

static void init(void)
{
    TRISA = 0x0Fu;   // RA[3:0] inputs (buttons), RA[5:4] outputs (unused)
    TRISB = 0x00u;   // RB all outputs (LED bar graph)
    TRISC = 0x80u;   // RC7 input (RX), RC6 output (TX)
    PORTA = 0x00u;
    PORTB = 0x00u;
    PORTC = 0x00u;

    ADCON1 = 0x07u;  // PCFG=0111: all port pins digital, ADC disabled

    SPBRG = UART_SPBRG;
    TXSTA = 0x24u;   // TXEN=1, BRGH=1, asynchronous
    RCSTA = 0x90u;   // SPEN=1, CREN=1

    // T0CS=0 internal, PSA=0 to TMR0, PS=100 (1:32) — same as target
    OPTION_REG = (OPTION_REG & 0xC0u) | 0x04u;
    TMR0 = T0_RELOAD;
    INTCONbits.T0IE = 1u;

    PIE1bits.RCIE   = 1u;
    INTCONbits.PEIE = 1u;
    INTCONbits.GIE  = 1u;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void main(void)
{
    init();

    for (;;)
    {
        // Handle incoming ADC response immediately, independent of tick rate
        if (rx_ready)
        {
            rx_ready = 0u;
            parse_response();
            update_leds();
        }

        if (!tick) continue;
        tick = 0u;

        // --- Button debounce and edge detection ---
        // PORTA bits 3:0, active low → invert so 1 = pressed
        uint8_t raw = (uint8_t)(~PORTA) & 0x0Fu;

        for (uint8_t i = 0u; i < NUM_BUTTONS; i++)
        {
            uint8_t bit = (uint8_t)(1u << i);

            if (raw & bit)                          // button currently pressed
            {
                if (btn_cnt[i] < DEBOUNCE_CNT)
                    btn_cnt[i]++;
            }
            else                                    // button released
            {
                btn_cnt[i]   = 0u;
                btn_armed[i] = 1u;                  // re-arm for next press
            }

            if (btn_cnt[i] == DEBOUNCE_CNT && btn_armed[i])
            {
                btn_armed[i] = 0u;                  // disarm until released

                switch (i)
                {
                    case BTN_STEP_CW:
                        uart_puts("M+1\n");
                        break;

                    case BTN_STEP_CCW:
                        uart_puts("M-1\n");
                        break;

                    case BTN_FAN:
                        fan_on = !fan_on;
                        uart_puts(fan_on ? "F100\n" : "F0\n");
                        break;

                    case BTN_ADC_CH:
                        adc_ch = (adc_ch + 1u) & 0x03u;
                        update_leds();
                        break;

                    default: break;
                }
            }
        }

        // --- Periodic ADC request ---
        if (++adc_timer >= ADC_INTERVAL)
        {
            adc_timer = 0u;
            uart_puts("R\n");
        }
    }
}
