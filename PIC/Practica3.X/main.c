/*
 * File:   main.c
 * Author: irmin
 *
 * PIC16F873A @ 4 MHz crystal (XT mode)
 *
 * Pinout (DIP-28):
 *  Pin  2  RA0/AN0  - ADC ch0   (0-5 V analogue input)
 *  Pin  3  RA1/AN1  - ADC ch1   (0-5 V analogue input)
 *  Pin  4  RA2/AN2  - ADC ch2   (0-5 V analogue input)
 *  Pin  5  RA3/AN3  - ADC ch3   (0-5 V analogue input)
 *  Pin 13  RC2/CCP1 - PWM output -> fan driver (MOSFET / L298N ENA)
 *  Pin 17  RC6/TX   - USART TX  -> HC-06 RX
 *  Pin 18  RC7/RX   - USART RX  <- HC-06 TX
 *  Pin 21  RB0      - Stepper IN1 (coil A)
 *  Pin 22  RB1      - Stepper IN2 (coil A-)
 *  Pin 23  RB2      - Stepper IN3 (coil B)
 *  Pin 24  RB3      - Stepper IN4 (coil B-)
 *
 * Bluetooth protocol (9600 8N1, commands end with '\n'):
 *   RX  "F<0-100>"      set fan speed in %
 *       "M<+/-><N>"     move stepper N full steps, CW(+) or CCW(-)
 *       "R"             request all four ADC readings
 *   TX  "V0:<mV>,V1:<mV>,V2:<mV>,V3:<mV>\n"
 *
 * LVP must be OFF so that RB3/PGM is available as a stepper output.
 */

// ---------------------------------------------------------------------------
// Configuration bits
// ---------------------------------------------------------------------------
#pragma config FOSC  = XT       // 4 MHz XT crystal
#pragma config WDTE  = OFF      // watchdog disabled
#pragma config PWRTE = ON       // power-up timer enabled (72 ms stabilisation)
#pragma config CP    = OFF      // no code protection
#pragma config BOREN = ON       // brown-out reset enabled
#pragma config LVP   = OFF      // LVP off  ->  RB3 usable as GPIO
#pragma config CPD   = OFF
#pragma config WRT   = OFF

#include <xc.h>
#include <stdint.h>
#include <stdlib.h>             // atoi

#define _XTAL_FREQ  4000000UL

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// USART  9600 baud @ 4 MHz, BRGH=1:  SPBRG = Fosc/(16*baud) - 1 = 25
#define UART_SPBRG      25u

// PWM ~1 kHz:  Timer2 prescaler = 4, PR2 = 249
// Period = (PR2+1) * 4 * Tosc * prescaler = 250 * 4 * 0.25 us * 4 = 1 ms
#define PWM_PR2         249u

// Timer0 ~5 ms stepper tick:  Fosc/4 = 1 MHz, prescaler 1:32
// reload = 256 - floor(5000/32) = 256 - 156 = 100  ->  actual ~4.992 ms
#define T0_RELOAD       100u

// 4-phase full-step sequence on RB[3:0], defines CW rotation
// IN1=RB0 IN2=RB1 IN3=RB2 IN4=RB3
// Step:  AB  BC  CD  DA
static const uint8_t STEP_SEQ[4] = {0x03u, 0x06u, 0x0Cu, 0x09u};

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

static volatile uint8_t step_tick = 0u;    // raised by Timer0 ISR ~every 5 ms

static int16_t  step_count = 0;            // remaining steps (+CW, -CCW)
static uint8_t  step_idx   = 0u;           // current index into STEP_SEQ

#define CMD_SIZE  24u
static volatile uint8_t  cmd_ready = 0u;
static volatile uint8_t  cmd_len   = 0u;
static          char     cmd_buf[CMD_SIZE];

// ---------------------------------------------------------------------------
// UART helpers
// ---------------------------------------------------------------------------

static void uart_putc(char c)
{
    while (!PIR1bits.TXIF);         // wait until TXREG can accept a byte
    TXREG = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

static void uart_putu(uint16_t v)
{
    char    buf[6];
    uint8_t i = 0u;
    if (v == 0u) { uart_putc('0'); return; }
    while (v) { buf[i++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (i) uart_putc(buf[--i]);
}

// ---------------------------------------------------------------------------
// ADC
// ---------------------------------------------------------------------------

static uint16_t adc_read(uint8_t ch)
{
    // ADCS=01 (Fosc/8 -> TAD=2 us, meets >=1.6 us), select channel, ADON=1
    ADCON0 = 0x41u | (uint8_t)(ch << 3u);
    __delay_us(30);                 // acquisition / charge time
    ADCON0bits.GO_nDONE = 1u;
    while (ADCON0bits.GO_nDONE);
    return (uint16_t)(((uint16_t)ADRESH << 8u) | ADRESL);
}

// ---------------------------------------------------------------------------
// PWM  (CCP1 / Timer2)
// ---------------------------------------------------------------------------

static void fan_set_pct(uint8_t pct)
{
    // 10-bit duty word = pct * (PR2+1) / 100
    uint16_t duty = ((uint16_t)pct * (PWM_PR2 + 1u)) / 100u;
    CCPR1L  = (uint8_t)(duty >> 2u);
    CCP1CON = (CCP1CON & 0xCFu) | (uint8_t)((duty & 0x03u) << 4u);
}

// ---------------------------------------------------------------------------
// Interrupt service routine
// ---------------------------------------------------------------------------

void __interrupt() isr(void)
{
    if (INTCONbits.T0IF)
    {
        INTCONbits.T0IF = 0u;
        TMR0 = T0_RELOAD;
        step_tick = 1u;
    }

    if (PIR1bits.RCIF)              // RCIF is cleared automatically by reading RCREG
    {
        if (RCSTAbits.OERR)         // overrun error: toggle CREN to reset
        {
            RCSTAbits.CREN = 0u;
            RCSTAbits.CREN = 1u;
        }

        char c = (char)RCREG;

        if (c == '\n' || c == '\r')
        {
            if (cmd_len > 0u && !cmd_ready)
            {
                cmd_ready = 1u;     // signal complete command to main loop
                // cmd_len intentionally left set; process_cmd() reads and resets it
            }
            // if cmd_ready already set (previous cmd not yet consumed), discard this newline
        }
        else if (!cmd_ready && cmd_len < CMD_SIZE - 1u)
        {
            cmd_buf[cmd_len++] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Command processor
// ---------------------------------------------------------------------------

static void process_cmd(void)
{
    char    local[CMD_SIZE];
    uint8_t len;

    // Snapshot the receive buffer with interrupts disabled to avoid a race
    // with the UART ISR, then immediately re-open the buffer for new input.
    INTCONbits.GIE = 0u;
    len = cmd_len;
    for (uint8_t i = 0u; i < len; i++) local[i] = cmd_buf[i];
    cmd_len = 0u;
    INTCONbits.GIE = 1u;

    local[len] = '\0';

    switch (local[0])
    {
        // ------------------------------------------------------------------
        case 'R': case 'r':
        {
            uint16_t mv[4];
            for (uint8_t i = 0u; i < 4u; i++)
                // Convert 10-bit raw (0-1023) to millivolts (0-5000)
                mv[i] = (uint16_t)(((uint32_t)adc_read(i) * 5000uL) / 1023uL);

            uart_puts("V0:"); uart_putu(mv[0]);
            uart_puts(",V1:"); uart_putu(mv[1]);
            uart_puts(",V2:"); uart_putu(mv[2]);
            uart_puts(",V3:"); uart_putu(mv[3]);
            uart_putc('\n');
            break;
        }

        // ------------------------------------------------------------------
        case 'F': case 'f':
        {
            uint8_t pct = (uint8_t)atoi(&local[1]);
            if (pct > 100u) pct = 100u;
            fan_set_pct(pct);
            break;
        }

        // ------------------------------------------------------------------
        case 'M': case 'm':
        {
            // atoi handles optional leading '+' or '-'
            step_count = (int16_t)atoi(&local[1]);
            break;
        }

        default: break;
    }
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

static void init_ports(void)
{
    TRISA = 0x3Fu;  // RA[5:0] inputs  (ADC channels AN0-AN4)
    TRISB = 0x00u;  // RB all outputs  (stepper on RB[3:0], RB[7:4] spare)
    TRISC = 0x80u;  // RC7 input (RX); RC6 output (TX); RC2 output (CCP1/PWM)
    PORTA = 0x00u;
    PORTB = 0x00u;
    PORTC = 0x00u;
}

static void init_adc(void)
{
    // ADFM=1 right-justified 10-bit result in ADRESH:ADRESL
    // PCFG=0000 -> AN0-AN4 analogue inputs, Vref = VDD
    ADCON1 = 0x80u;
    // ADC clock Fosc/8 (TAD=2 us @ 4 MHz), channel 0, ADC on
    // Overwritten per channel inside adc_read()
    ADCON0 = 0x41u;
}

static void init_pwm(void)
{
    PR2     = PWM_PR2;  // set PWM period
    CCPR1L  = 0x00u;   // start at 0 % duty (fan off)
    CCP1CON = 0x0Cu;   // CCP1 in PWM mode
    T2CON   = 0x05u;   // Timer2 on, prescaler 1:4
}

static void init_uart(void)
{
    SPBRG = UART_SPBRG;
    TXSTA = 0x24u;      // TXEN=1, BRGH=1, asynchronous mode
    RCSTA = 0x90u;      // SPEN=1, CREN=1 (continuous receive enabled)
}

static void init_timer0(void)
{
    // T0CS=0 internal (Fosc/4), PSA=0 prescaler to TMR0, PS=100 -> 1:32
    OPTION_REG = (OPTION_REG & 0xC0u) | 0x04u;
    TMR0 = T0_RELOAD;
    INTCONbits.T0IE = 1u;   // enable TMR0 overflow interrupt
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void main(void)
{
    init_ports();
    init_adc();
    init_pwm();
    init_uart();
    init_timer0();

    PIE1bits.RCIE   = 1u;   // USART receive interrupt
    INTCONbits.PEIE = 1u;   // peripheral interrupt enable
    INTCONbits.GIE  = 1u;   // global interrupt enable

    uart_puts("OK\n");

    for (;;)
    {
        // Process complete command received over Bluetooth
        if (cmd_ready)
        {
            cmd_ready = 0u;
            process_cmd();
        }

        // Advance stepper one step per Timer0 tick (~5 ms)
        if (step_tick)
        {
            step_tick = 0u;

            if (step_count > 0)
            {
                step_idx = (step_idx + 1u) & 0x03u;                // CW
                PORTB = (PORTB & 0xF0u) | STEP_SEQ[step_idx];
                step_count--;
            }
            else if (step_count < 0)
            {
                step_idx = (step_idx + 3u) & 0x03u;                // CCW (-1 mod 4)
                PORTB = (PORTB & 0xF0u) | STEP_SEQ[step_idx];
                step_count++;
            }
            // Coils remain energised at the final step for holding torque.
        }
    }
}
