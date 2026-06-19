/*
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

// USART  9600 baud @ 4 MHz
#define UART_SPBRG      25u

// PWM ~1 kHz
#define PWM_PR2         249u

// Timer0 ~5 ms
#define T0_RELOAD       100u

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
// PWM 
// ---------------------------------------------------------------------------

static void fan_set_pct(uint8_t pct)
{
    // 10-bit duty word (0..1000 == 0..100%): pct * (PR2+1) * 4 / 100
    // The *4 accounts for the two Tosc/4 sub-bits of CCP PWM resolution.
    // uint32 cast avoids the 16-bit overflow of 100 * 1000.
    uint16_t duty = (uint16_t)(((uint32_t)pct * (PWM_PR2 + 1u) * 4u) / 100u);
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
                cmd_ready = 1u;
            }
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
    TRISA = 0x3Fu;
    TRISB = 0x00u;
    TRISC = 0x80u;
    PORTA = 0x00u;
    PORTB = 0x00u;
    PORTC = 0x00u;
}

static void init_adc(void)
{
    ADCON1 = 0x80u;
    ADCON0 = 0x41u;
}

static void init_pwm(void)
{
    PR2     = PWM_PR2;
    CCPR1L  = 0x00u; 
    CCP1CON = 0x0Cu;  
    T2CON   = 0x05u;
}

static void init_uart(void)
{
    SPBRG = UART_SPBRG;
    TXSTA = 0x24u;  
    RCSTA = 0x90u; 
}

static void init_timer0(void)
{
    OPTION_REG = (OPTION_REG & 0xC0u) | 0x04u;
    TMR0 = T0_RELOAD;
    INTCONbits.T0IE = 1u;
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

    PIE1bits.RCIE   = 1u;
    INTCONbits.PEIE = 1u;
    INTCONbits.GIE  = 1u;

    uart_puts("OK\n");

    for (;;)
    {
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
        }
    }
}
