#pragma config OSC = HS
#pragma config WDT = OFF
#pragma config PBADEN = OFF
#pragma config MCLRE = ON
#pragma config LVP = OFF

#include <xc.h>
#include <stdio.h>

// Calibrated software delay
void delay_ms(unsigned int ms) {
    for (unsigned int i = 0; i < ms; i++) {
        for (int j = 0; j < 165; j++);
    }
}

void delay_short() {
    for (int i = 0; i < 500; i++);
}

// --- 4-BIT LCD DRIVER ---
void lcd_write_nibble(unsigned char nibble) {
    LATB = (LATB & 0x0F) | (nibble & 0xF0);
    PORTAbits.RA1 = 1; // EN = 1
    delay_short();
    PORTAbits.RA1 = 0; // EN = 0
}

void command(unsigned char cmd) {
    PORTAbits.RA0 = 0; // RS = 0
    lcd_write_nibble(cmd & 0xF0);
    lcd_write_nibble(cmd << 4);
    delay_ms(2);
}

void data(unsigned char val) {
    PORTAbits.RA0 = 1; // RS = 1
    lcd_write_nibble(val & 0xF0);
    lcd_write_nibble(val << 4);
    delay_ms(2);
}

void lcd_print(const char* s) {
    while(*s) {
        data(*s++);
    }
}

void lcd_init() {
    delay_ms(15);
    lcd_write_nibble(0x30);
    delay_ms(5);
    lcd_write_nibble(0x30);
    delay_ms(1);
    lcd_write_nibble(0x30);
    delay_ms(1);
    lcd_write_nibble(0x20); // 4-bit select
    delay_ms(1);
    
    command(0x28); // 4-bit, 2-line
    command(0x0C); // Display ON, Cursor OFF
    command(0x06); // Auto-increment
    command(0x01); // Clear screen
    delay_ms(2);
}

// --- USART SERIAL TELEMETRY ---
void usart_init() {
    TRISCbits.TRISC7 = 1; // RX as input
    TRISCbits.TRISC6 = 0; // TX as output
    TXSTA = 0x24;         // Transmit Enabled, High speed
    RCSTA = 0x90;         // Continuous receive enabled, Serial Port Enabled
    SPBRG = 129;          // 9600 Baud at 20MHz crystal oscillator
}

char usart_read() {
    if (OERR) {
        CREN = 0;
        CREN = 1;
    }
    while (RCIF == 0);
    return RCREG;
}

void usart_write(char ch) {
    while (!TXIF); // Wait for buffer empty
    TXREG = ch;
}

void usart_print(const char* s) {
    while (*s) {
        usart_write(*s++);
    }
}

// --- MAIN CONTROLLER SYSTEM ---
void main() {
    ADCON1 = 0x0F; // Digital mode configurations
    
    TRISD = 0x00;  // PORTD as outputs (Motor & LEDs)
    TRISB = 0x07;  // RB0-RB2 inputs (buttons), RB4-RB7 outputs (LCD)
    TRISA = 0xFC;  // RA0-RA1 outputs (LCD RS, EN)
    
    // Set all outputs low initially
    LATDbits.LATD0 = 0; // Motor IN1
    LATDbits.LATD1 = 0; // Motor IN2
    LATDbits.LATD4 = 0; // Green LED
    LATDbits.LATD5 = 0; // Red LED
    LATDbits.LATD6 = 0; // Yellow LED
    LATDbits.LATD7 = 0; // Emergency Warning LED
    LATB = 0x00;
    
    lcd_init();
    usart_init();
    
    unsigned char charge_state = 0; // 0 = Standby/Ready, 1 = Locking Connector, 2 = Charging, 3 = Completed, 4 = Fault
    int battery_level = 0; // State of Charge percentage
    int charge_rate = 1; // 1 = Normal Rate (1% per tick), 2 = Fast Rate (3% per tick)
    char buffer[32];
    
    command(0x80);
    lcd_print("EV CHARGING STN ");
    command(0xC0);
    lcd_print("STATUS: READY   ");
    
    usart_print("\r\n==========================================\r\n");
    usart_print("  Smart EV Charging Station Online\r\n");
    usart_print("==========================================\r\n");
    usart_print("Keyboard Commands (Type in Virtual Terminal):\r\n");
    usart_print("  [S] - Plug Cable & Start Charging\r\n");
    usart_print("  [F] - Toggle Normal/Fast Rate\r\n");
    usart_print("  [K] - Kill/Abort Charging Cycle\r\n");
    usart_print("  [E] - Trigger Emergency Stop\r\n");
    usart_print("------------------------------------------\r\n");
    
    while(1) {
        // --- 1. EMERGENCY STOP CHECK (RB2 Active LOW) ---
        if (PORTBbits.RB2 == 0) {
            charge_state = 4; // Force Fault state
        }
        
        // --- 2. SCAN SERIAL RX FOR KEYBOARD TYPING COMMANDS ---
        if (RCIF) {
            char cmd = usart_read();
            
            // Type 'S' or 's' to plug-in and start charging
            if ((cmd == 'S' || cmd == 's') && charge_state == 0) {
                charge_state = 1; // Transition to locking and charging
            }
            // Type 'F' or 'f' to toggle fast/normal charge rate
            else if ((cmd == 'F' || cmd == 'f') && charge_state == 2) {
                if (charge_rate == 1) {
                    charge_rate = 2;
                    usart_print("UART Command: FAST Charge Activated\r\n");
                } else {
                    charge_rate = 1;
                    usart_print("UART Command: NORMAL Charge Activated\r\n");
                }
            }
            // Type 'K' or 'k' to Kill/Abort charging (Stop and unlock plug)
            else if ((cmd == 'K' || cmd == 'k') && charge_state == 2) {
                usart_print("UART Command: Aborting Charge Cycle...\r\n");
                charge_state = 3; // Transition directly to completion (unlock and exit)
            }
            // Type 'E' or 'e' for immediate Emergency stop
            else if (cmd == 'E' || cmd == 'e') {
                charge_state = 4; // Force Fault state
            }
        }
        
        // --- 3. FAULT / EMERGENCY SYSTEM DISCONNECT ---
        if (charge_state == 4) {
            LATDbits.LATD6 = 0; // Stop charging (Yellow OFF)
            
            // Unlock Plug Latch Actuator (Motor Reverse RD0=0, RD1=1)
            command(0x01);
            lcd_print("! EMERGENCY !");
            command(0xC0);
            lcd_print("UNLOCKING PLUG  ");
            usart_print("ALERT: Emergency stop triggered! Releasing connector plug...\r\n");
            
            LATDbits.LATD0 = 0;
            LATDbits.LATD1 = 1;
            delay_ms(2000); // Reverse motor for 2s
            
            // Stop Motor, Turn ON Red LED
            LATDbits.LATD0 = 0;
            LATDbits.LATD1 = 0;
            LATDbits.LATD5 = 1; // Red LED solid ON (Fault indicator)
            
            command(0x01);
            lcd_print("SAFE TO UNPLUG  ");
            command(0xC0);
            lcd_print("RE-HOME REQUIRED");
            
            while(1) {
                // Flash Red LED hazard strobe
                LATDbits.LATD5 = !LATDbits.LATD5;
                delay_ms(250);
            }
        }
        
        // --- 4. CHARGER STANDBY STATE ---
        if (charge_state == 0) {
            LATDbits.LATD4 = 1; // Green LED ON (Ready/Available)
            LATDbits.LATD5 = 0; // Red LED OFF
            LATDbits.LATD6 = 0; // Yellow LED OFF
            
            // Look for Plug Insertion sensor (RB0 pressed/LOW)
            if (PORTBbits.RB0 == 0) {
                delay_ms(50); // Debounce
                if (PORTBbits.RB0 == 0) {
                    charge_state = 1; // Jump to locking phase
                }
            }
        }
        
        // --- 5. CONNECTOR PLUG LOCKING ACTUATOR ACTION ---
        if (charge_state == 1) {
            usart_print("Plug inserted. Locking connector pin...\r\n");
            command(0x01);
            lcd_print("CABLE CONNECTED ");
            command(0xC0);
            lcd_print("LOCKING PLUG... ");
            
            // Lock Latch Motor Forward (RD0=1, RD1=0)
            LATDbits.LATD0 = 1;
            LATDbits.LATD1 = 0;
            delay_ms(2000); // Lock for 2s
            
            // Stop motor, start charging process
            LATDbits.LATD0 = 0;
            LATDbits.LATD1 = 0;
            
            battery_level = 0; // Reset battery level
            charge_state = 2; // Move to charging phase
        }
        
        // --- 6. ACTIVE VEHICLE CHARGING LOOP ---
        if (charge_state == 2) {
            LATDbits.LATD4 = 0; // Green LED OFF
            LATDbits.LATD6 = !LATDbits.LATD6; // Blink Yellow LED
            
            // Read manual speed selector switch (RB1)
            if (PORTBbits.RB1 == 0) {
                delay_ms(50); // Debounce
                if (PORTBbits.RB1 == 0) {
                    if (charge_rate == 1) {
                        charge_rate = 2; // Shift to Fast charging
                        usart_print("User Input: Fast Charge Activated\r\n");
                    } else {
                        charge_rate = 1; // Shift to Normal charging
                        usart_print("User Input: Normal Charge Activated\r\n");
                    }
                }
            }
            
            // Accumulate charge percentage
            if (charge_rate == 1) {
                battery_level = battery_level + 2; // Slow charging increment
            } else {
                battery_level = battery_level + 5; // Fast charging increment
            }
            
            if (battery_level >= 100) {
                battery_level = 100;
                charge_state = 3; // Move to completed phase
            }
            
            // Print status on LCD
            command(0x80);
            if (charge_rate == 1) {
                lcd_print("RATE: NORMAL AC ");
            } else {
                lcd_print("RATE: FAST DC   ");
            }
            
            command(0xC0);
            sprintf(buffer, "BATTERY: %d%%    ", battery_level);
            lcd_print(buffer);
            
            // Send serial logging telemetry data
            if (charge_rate == 1) {
                sprintf(buffer, "TX_DATA: SOC=%d%%, V=230V, I=32A\r\n", battery_level);
            } else {
                sprintf(buffer, "TX_DATA: SOC=%d%%, V=400V, I=120A\r\n", battery_level);
            }
            usart_print(buffer);
            
            delay_ms(800); // Delay between charging simulation steps
        }
        
        // --- 7. CHARGING CYCLE COMPLETED ---
        if (charge_state == 3) {
            LATDbits.LATD6 = 0; // Yellow LED OFF
            LATDbits.LATD4 = 1; // Green LED ON
            
            command(0x01);
            lcd_print("CHARGE COMPLETE!");
            command(0xC0);
            lcd_print("UNLOCKING PLUG  ");
            usart_print("Vehicle battery fully charged! Releasing connector plug...\r\n");
            
            // Reverse motor to unlock connector (RD0=0, RD1=1)
            LATDbits.LATD0 = 0;
            LATDbits.LATD1 = 1;
            delay_ms(2000); // Spin for 2s
            
            LATDbits.LATD0 = 0;
            LATDbits.LATD1 = 0;
            
            command(0x01);
            lcd_print("SAFE TO REMOVE  ");
            command(0xC0);
            lcd_print("THANK YOU       ");
            delay_ms(4000);
            
            charge_state = 0; // Return to standby
        }
        delay_ms(100);
    }
}

