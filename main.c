#define F_CPU 7372800UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/sfr_defs.h>
#include <stdio.h>
#include <string.h>
#include "myLCD_new.h"

/* ================= PIN DEFINE ================= */
#define LED_RED_DDR   DDRC
#define LED_RED_PORT  PORTC
#define LED_RED_PIN   PC0

#define LED_BLUE_DDR  DDRC
#define LED_BLUE_PORT PORTC
#define LED_BLUE_PIN  PC1

#define SERVO_DDR     DDRC
#define SERVO_PORT    PORTC
#define SERVO_PIN     PC2

#define PUMP_DDR      DDRC
#define PUMP_PORT     PORTC
#define PUMP_PIN      PC3

#define LIGHT_DDR     DDRC
#define LIGHT_PORT    PORTC
#define LIGHT_PIN     PC4

#define DHT_DDR       DDRF
#define DHT_PORT      PORTF
#define DHT_PIN       PINF
#define DHT_BIT       PF2

#define MODE_AUTO     0
#define MODE_MANUAL   1

/* ================= ADC CHANNEL ================= */
#define SOIL_ADC_CH    0   /* PF0 / ADC0 */
#define LM35_ADC_CH    1   /* PF1 / ADC1 */

/* ================= LM35 STABLE FILTER =================
   LM35 ra tín hiệu analog nhỏ: 10mV / 1°C.
   Với Vref 5V, ADC dễ dao động nên LCD dễ nhảy nhiệt độ.

   Cách xử lý:
   - Đọc nhiều mẫu hơn.
   - Bỏ mẫu nhỏ nhất và lớn nhất.
   - Lọc EMA để nhiệt độ mượt.
   - Chỉ đổi số LCD khi lệch đủ 1°C.
*/
#define LM35_SAMPLE_COUNT      32
#define LM35_EMA_SHIFT         3
#define LM35_DISPLAY_HYS_X10   10
#define LM35_STABLE_COUNT      7


/* ================= SOIL CALIBRATION =================
   Cần đo thực tế lại:
   - Đất/nước rất ẩm  -> ADC thấp hơn -> soil gần 100%
   - Đất khô/ngoài không khí -> ADC cao hơn -> soil gần 0%

   Nếu soil bị ngược, báo mình để đảo công thức.
*/
#define SOIL_ADC_WET  300
#define SOIL_ADC_DRY  850

/* ================= BINARY PROTOCOL ================= */
#define CMD_START     0xAA
#define RESP_START    0x55

#define CMD_MODE      0x01
#define CMD_SERVO     0x02
#define CMD_PUMP      0x03
#define CMD_LIGHT     0x04
#define CMD_SET_SUN   0x10
#define CMD_SET_RAIN  0x11

#define RESP_SENSOR   0x80
#define RESP_ACK      0x81

#define ACK_OK             0
#define ACK_IGNORED_AUTO   1
#define ACK_RANGE_ERROR    2
#define ACK_UNKNOWN        3
#define ACK_QUEUE_FULL     4

#define STATUS_NORMAL  0
#define STATUS_HOT_DRY 1
#define STATUS_RAIN    2

/* ================= COMMAND QUEUE ================= */
#define CMD_QUEUE_SIZE 8

volatile uint8_t q_cmd[CMD_QUEUE_SIZE];
volatile uint8_t q_p1[CMD_QUEUE_SIZE];
volatile uint8_t q_p2[CMD_QUEUE_SIZE];
volatile uint8_t q_p3[CMD_QUEUE_SIZE];

volatile uint8_t q_head = 0;
volatile uint8_t q_tail = 0;
volatile uint8_t q_count = 0;
volatile uint8_t q_overflow = 0;
volatile uint8_t q_overflow_cmd = 0;

/* ================= GLOBAL VARIABLES ================= */
volatile char mode = MODE_AUTO;

// Servo hold, same motion style as filemauservo.c.
// Reversed for the current door linkage: close = 2000 us, open = 1000 us.
volatile char servo_state = 0;

#define SERVO_CLOSE_US             2000
#define SERVO_OPEN_US              1000

volatile uint32_t sys_ms = 0;

/* UART binary receive state */
volatile uint8_t rx_state = 0;
volatile uint8_t temp_cmd = 0;
volatile uint8_t temp_p1 = 0;
volatile uint8_t temp_p2 = 0;
volatile uint8_t temp_p3 = 0;

/* ================= TIMER0 MILLIS ================= */
void Timer0_Init(void)
{
	/*
	   F_CPU = 7.3728 MHz
	   Prescaler 64 => 115200 Hz
	   OCR0 = 114 => 115 counts ~ 1 ms
	*/
	TCCR0 = (1 << WGM01) | (1 << CS01) | (1 << CS00);
	OCR0 = 114;
	TIMSK |= (1 << OCIE0);
}

ISR(TIMER0_COMP_vect)
{
	sys_ms++;
}

uint32_t millis_atmega(void)
{
	uint32_t m;
	uint8_t sreg = SREG;

	cli();
	m = sys_ms;
	SREG = sreg;

	return m;
}

void Servo_SetTarget(uint8_t open)
{
	uint8_t new_state = open ? 1 : 0;

	uint8_t sreg = SREG;

	cli();

	servo_state = new_state;

	SREG = sreg;
}

void Servo_Hold(void)
{
	if(servo_state == 1)
	{
		SERVO_PORT |= (1 << SERVO_PIN);
		_delay_us(SERVO_OPEN_US);
		SERVO_PORT &= ~(1 << SERVO_PIN);
		_delay_ms(20 - (SERVO_OPEN_US / 1000));
	}
	else
	{
		SERVO_PORT |= (1 << SERVO_PIN);
		_delay_us(SERVO_CLOSE_US);
		SERVO_PORT &= ~(1 << SERVO_PIN);
		_delay_ms(20 - (SERVO_CLOSE_US / 1000));
	}
}

/* ================= ADC ================= */
void ADC_Init(void)
{
	/*
	   AVCC làm điện áp tham chiếu.
	   Nhớ cấp AVCC/AREF đúng cho ATmega128.
	*/
	ADMUX = (1 << REFS0);

	/*
	   Prescaler 64:
	   7.3728 MHz / 64 = 115.2 kHz
	   ADC ổn định hơn.
	*/
	ADCSRA = (1 << ADEN) | (1 << ADPS2) | (1 << ADPS1);
}

uint16_t read_adc_once(uint8_t channel)
{
	ADMUX = (ADMUX & 0xF0) | (channel & 0x0F);
	_delay_us(20);

	ADCSRA |= (1 << ADSC);
	while(bit_is_clear(ADCSRA, ADIF));
	ADCSRA |= (1 << ADIF);

	return ADCW;
}

uint16_t read_adc_avg(uint8_t channel)
{
	uint32_t sum = 0;

	/* Bỏ lần đọc đầu sau khi đổi kênh cho ổn định */
	read_adc_once(channel);

	for(uint8_t i = 0; i < 8; i++)
	{
		sum += read_adc_once(channel);
		_delay_us(100);
	}

	return (uint16_t)(sum / 8);
}
uint16_t read_adc_lm35_stable(uint8_t channel)
{
	uint32_t sum = 0;
	uint16_t value;
	uint16_t min_value = 1023;
	uint16_t max_value = 0;

	/* Bỏ lần đọc đầu sau khi đổi sang kênh LM35 */
	read_adc_once(channel);
	_delay_us(200);

	for(uint8_t i = 0; i < LM35_SAMPLE_COUNT; i++)
	{
		value = read_adc_once(channel);

		if(value < min_value) min_value = value;
		if(value > max_value) max_value = value;

		sum += value;
		_delay_us(200);
	}

	/* Bỏ mẫu thấp nhất và cao nhất để giảm nhiễu đột biến */
	sum -= min_value;
	sum -= max_value;

	return (uint16_t)(sum / (LM35_SAMPLE_COUNT - 2));
}
uint16_t lm35_temp_x10_from_adc(uint16_t adc_value)
{
    /*
       LM35: 10mV / 1°C
       ADC Vref = 5V:
       Điện áp mV = ADC * 5000 / 1023

       Vì LM35 = 10mV/°C
       nên nhiệt độ x10 = điện áp mV.
       Ví dụ: 298 nghĩa là 29.8°C
    */
    uint32_t temp_x10;

    temp_x10 = (uint32_t)adc_value * 5000UL / 1023UL;

    if(temp_x10 > 990)
    {
        temp_x10 = 990;
    }

    return (uint16_t)temp_x10;
}
uint16_t lm35_temp_x10_filtered(uint16_t adc_value)
{
	static uint8_t first_run = 1;
	static uint16_t filtered_x10 = 300;
	uint16_t raw_x10;
	uint8_t weight_old;

	raw_x10 = lm35_temp_x10_from_adc(adc_value);

	if(first_run)
	{
		filtered_x10 = raw_x10;
		first_run = 0;
	}
	else
	{
		weight_old = (1 << LM35_EMA_SHIFT) - 1;
		filtered_x10 = (uint16_t)((filtered_x10 * (uint32_t)weight_old + raw_x10 + (1 << (LM35_EMA_SHIFT - 1))) >> LM35_EMA_SHIFT);
	}

	return filtered_x10;
}
uint8_t lm35_temp_display_from_x10(uint16_t temp_x10)
{
	static uint8_t first_run = 1;
	static uint8_t display_temp = 30;
	static uint8_t candidate_temp = 30;
	static uint8_t stable_count = 0;

	uint8_t rounded_temp;
	uint8_t target_temp;
	uint16_t display_x10;

	rounded_temp = (uint8_t)((temp_x10 + 5) / 10);

	if(rounded_temp > 99)
	{
		rounded_temp = 99;
	}

	if(first_run)
	{
		display_temp = rounded_temp;
		candidate_temp = rounded_temp;
		stable_count = 0;
		first_run = 0;
		return display_temp;
	}

	display_x10 = (uint16_t)display_temp * 10;
	target_temp = display_temp;

	/*
	   Neu nhiet do loc van nam quanh gia tri dang hien thi,
	   giu nguyen LCD va reset bo dem ung vien.
	*/
	if(temp_x10 >= display_x10 + LM35_DISPLAY_HYS_X10)
	{
		if(display_temp < 99)
		{
			target_temp = display_temp + 1;
		}
	}
	else if(temp_x10 + LM35_DISPLAY_HYS_X10 <= display_x10)
	{
		if(display_temp > 0)
		{
			target_temp = display_temp - 1;
		}
	}
	else
	{
		candidate_temp = display_temp;
		stable_count = 0;
		return display_temp;
	}

	/*
	   Nhiet do moi phai lap lai cung huong nhieu lan lien tiep
	   thi moi cap nhat len LCD/Firebase.
	*/
	if(target_temp == candidate_temp)
	{
		if(stable_count < 255)
		{
			stable_count++;
		}
	}
	else
	{
		candidate_temp = target_temp;
		stable_count = 1;
	}

	if(stable_count >= LM35_STABLE_COUNT)
	{
		display_temp = candidate_temp;
		stable_count = 0;
	}

	return display_temp;
}

int soil_percent_from_adc(uint16_t adc_value)
{
	int soil;

	if(adc_value <= SOIL_ADC_WET)
	{
		soil = 100;
	}
	else if(adc_value >= SOIL_ADC_DRY)
	{
		soil = 0;
	}
	else
	{
		soil = (long)(SOIL_ADC_DRY - adc_value) * 100 / (SOIL_ADC_DRY - SOIL_ADC_WET);
	}

	if(soil > 100) soil = 100;
	if(soil < 0) soil = 0;

	return soil;
}

uint8_t lm35_temp_from_adc(uint16_t adc_value)
{
	/*
	   LM35: 10mV / 1°C
	   ADC Vref = 5V:
	   Temp = ADC * 500 / 1023

	   Ví dụ:
	   ADC 61 -> khoảng 30°C
	*/
	uint32_t temp_x10;
	uint16_t temp_c;

	temp_x10 = (uint32_t)adc_value * 5000UL / 1023UL;  /* nhiệt độ x10 */
	temp_c = (uint16_t)((temp_x10 + 5) / 10);          /* làm tròn */

	if(temp_c > 99)
	{
		temp_c = 99;
	}

	return (uint8_t)temp_c;
}

/* ================= DHT11: CHỈ LẤY ĐỘ ẨM ================= */
uint8_t read_dht11_humidity(uint8_t *humidity)
{
	uint8_t data[5] = {0};
	uint16_t timeout;

	DHT_DDR |= (1 << DHT_BIT);
	DHT_PORT &= ~(1 << DHT_BIT);
	_delay_ms(20);

	DHT_PORT |= (1 << DHT_BIT);
	_delay_us(30);
	DHT_DDR &= ~(1 << DHT_BIT);

	timeout = 0;
	while(DHT_PIN & (1 << DHT_BIT))
	{
		if(++timeout > 10000) return 0;
	}

	timeout = 0;
	while(!(DHT_PIN & (1 << DHT_BIT)))
	{
		if(++timeout > 10000) return 0;
	}

	timeout = 0;
	while(DHT_PIN & (1 << DHT_BIT))
	{
		if(++timeout > 10000) return 0;
	}

	for(uint8_t i = 0; i < 40; i++)
	{
		timeout = 0;
		while(!(DHT_PIN & (1 << DHT_BIT)))
		{
			if(++timeout > 10000) return 0;
		}

		_delay_us(30);

		if(DHT_PIN & (1 << DHT_BIT))
		{
			data[i / 8] |= (1 << (7 - (i % 8)));
		}

		timeout = 0;
		while(DHT_PIN & (1 << DHT_BIT))
		{
			if(++timeout > 10000) return 0;
		}
	}

	if((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4])
	{
		return 0;
	}

	*humidity = data[0];

	return 1;
}

uint8_t read_dht11_humidity_safe(uint8_t *humidity)
{
	uint8_t ok;
	uint8_t old_timsk;
	uint8_t old_ucsr0b;
	uint8_t dummy;
	uint8_t sreg;

	sreg = SREG;
	cli();

	old_timsk = TIMSK;
	old_ucsr0b = UCSR0B;

	/* Tat Timer0 va UART RX interrupt trong luc doc DHT11 de timing on hon. */
	TIMSK &= ~(1 << OCIE0);
	UCSR0B &= ~(1 << RXCIE0);

	SERVO_PORT &= ~(1 << SERVO_PIN);

	SREG = sreg;

	ok = read_dht11_humidity(humidity);

	sreg = SREG;
	cli();

	SERVO_PORT &= ~(1 << SERVO_PIN);

	while(UCSR0A & (1 << RXC0))
	{
		dummy = UDR0;
		(void)dummy;
	}

	rx_state = 0;
	TIMSK = old_timsk;
	UCSR0B = old_ucsr0b;

	SREG = sreg;

	return ok;
}

/* ================= UART ================= */
void UART_Init(void)
{
	UBRR0H = 0;
	UBRR0L = 3;      /* 115200 baud @ 7.3728 MHz */

	UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
	UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void UART_TxByte(uint8_t data)
{
	while(!(UCSR0A & (1 << UDRE0)));
	UDR0 = data;
}

uint8_t cmd_checksum(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3)
{
	return (uint8_t)(CMD_START ^ cmd ^ p1 ^ p2 ^ p3);
}

uint8_t resp_checksum(uint8_t type,
uint8_t d1,
uint8_t d2,
uint8_t d3,
uint8_t d4,
uint8_t d5,
uint8_t d6,
uint8_t d7,
uint8_t d8)
{
	return (uint8_t)(RESP_START ^ type ^ d1 ^ d2 ^ d3 ^ d4 ^ d5 ^ d6 ^ d7 ^ d8);
}

void UART_SendResponse(uint8_t type,
uint8_t d1,
uint8_t d2,
uint8_t d3,
uint8_t d4,
uint8_t d5,
uint8_t d6,
uint8_t d7,
uint8_t d8)
{
	uint8_t cs = resp_checksum(type, d1, d2, d3, d4, d5, d6, d7, d8);

	UART_TxByte(RESP_START);
	UART_TxByte(type);
	UART_TxByte(d1);
	UART_TxByte(d2);
	UART_TxByte(d3);
	UART_TxByte(d4);
	UART_TxByte(d5);
	UART_TxByte(d6);
	UART_TxByte(d7);
	UART_TxByte(d8);
	UART_TxByte(cs);
}

void UART_SendAck(uint8_t cmd, uint8_t ackCode)
{
	UART_SendResponse(RESP_ACK, cmd, ackCode, 0, 0, 0, 0, 0, 0);
}

/* ================= UART COMMAND QUEUE ================= */
uint8_t queue_push_from_isr(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3)
{
	if(q_count >= CMD_QUEUE_SIZE)
	{
		q_overflow = 1;
		q_overflow_cmd = cmd;
		return 0;
	}

	q_cmd[q_head] = cmd;
	q_p1[q_head]  = p1;
	q_p2[q_head]  = p2;
	q_p3[q_head]  = p3;

	q_head++;

	if(q_head >= CMD_QUEUE_SIZE)
	{
		q_head = 0;
	}

	q_count++;

	return 1;
}

uint8_t queue_pop(uint8_t *cmd, uint8_t *p1, uint8_t *p2, uint8_t *p3)
{
	uint8_t ok = 0;
	uint8_t sreg = SREG;

	cli();

	if(q_count > 0)
	{
		*cmd = q_cmd[q_tail];
		*p1  = q_p1[q_tail];
		*p2  = q_p2[q_tail];
		*p3  = q_p3[q_tail];

		q_tail++;

		if(q_tail >= CMD_QUEUE_SIZE)
		{
			q_tail = 0;
		}

		q_count--;
		ok = 1;
	}

	SREG = sreg;

	return ok;
}

uint8_t queue_take_overflow(uint8_t *cmd)
{
	uint8_t ok = 0;
	uint8_t sreg = SREG;

	cli();

	if(q_overflow)
	{
		*cmd = q_overflow_cmd;
		q_overflow = 0;
		ok = 1;
	}

	SREG = sreg;

	return ok;
}

ISR(USART0_RX_vect)
{
	uint8_t c = UDR0;

	switch(rx_state)
	{
		case 0:
			if(c == CMD_START)
			{
				rx_state = 1;
			}
			break;

		case 1:
			temp_cmd = c;
			rx_state = 2;
			break;

		case 2:
			temp_p1 = c;
			rx_state = 3;
			break;

		case 3:
			temp_p2 = c;
			rx_state = 4;
			break;

		case 4:
			temp_p3 = c;
			rx_state = 5;
			break;

		case 5:
			if(c == cmd_checksum(temp_cmd, temp_p1, temp_p2, temp_p3))
			{
				queue_push_from_isr(temp_cmd, temp_p1, temp_p2, temp_p3);
			}

			rx_state = 0;
			break;

		default:
			rx_state = 0;
			break;
	}
}

/* ================= DEVICE CONTROL ================= */
void pump_on(void)
{
	PUMP_PORT |= (1 << PUMP_PIN);
}

void pump_off(void)
{
	PUMP_PORT &= ~(1 << PUMP_PIN);
}

void light_on(void)
{
	LIGHT_PORT |= (1 << LIGHT_PIN);
}

void light_off(void)
{
	LIGHT_PORT &= ~(1 << LIGHT_PIN);
}

void red_on(void)
{
	LED_RED_PORT |= (1 << LED_RED_PIN);
}

void red_off(void)
{
	LED_RED_PORT &= ~(1 << LED_RED_PIN);
}

void blue_on(void)
{
	LED_BLUE_PORT |= (1 << LED_BLUE_PIN);
}

void blue_off(void)
{
	LED_BLUE_PORT &= ~(1 << LED_BLUE_PIN);
}

/* ================= COMMAND PROCESS ================= */
void process_binary_command(uint8_t cmd,
uint8_t p1,
uint8_t p2,
uint8_t p3,
int *sunTemp,
int *sunHum,
int *sunSoil,
int *rainTemp,
int *rainHum,
int *rainSoil)
{
	switch(cmd)
	{
		case CMD_MODE:
			if(p1 == MODE_MANUAL)
			{
				mode = MODE_MANUAL;
			}
			else
			{
				mode = MODE_AUTO;
			}

			UART_SendAck(cmd, ACK_OK);
			break;

		case CMD_SERVO:
		if(mode == MODE_MANUAL)
		{
			Servo_SetTarget((p1 != 0) ? 1 : 0);
			UART_SendAck(cmd, ACK_OK);
		}
		else
		{
			UART_SendAck(cmd, ACK_IGNORED_AUTO);
		}
		break;

		case CMD_PUMP:
			if(mode == MODE_MANUAL)
			{
				if(p1)
				{
					pump_on();
				}
				else
				{
					pump_off();
				}

				UART_SendAck(cmd, ACK_OK);
			}
			else
			{
				UART_SendAck(cmd, ACK_IGNORED_AUTO);
			}
			break;

		case CMD_LIGHT:
			if(p1)
			{
				light_on();
			}
			else
			{
				light_off();
			}

			UART_SendAck(cmd, ACK_OK);
			break;

		case CMD_SET_SUN:
			if(p1 <= 80 && p2 <= 100 && p3 <= 100)
			{
				*sunTemp = p1;
				*sunHum  = p2;
				*sunSoil = p3;
				UART_SendAck(cmd, ACK_OK);
			}
			else
			{
				UART_SendAck(cmd, ACK_RANGE_ERROR);
			}
			break;

		case CMD_SET_RAIN:
			if(p1 <= 80 && p2 <= 100 && p3 <= 100)
			{
				*rainTemp = p1;
				*rainHum  = p2;
				*rainSoil = p3;
				UART_SendAck(cmd, ACK_OK);
			}
			else
			{
				UART_SendAck(cmd, ACK_RANGE_ERROR);
			}
			break;

		default:
			UART_SendAck(cmd, ACK_UNKNOWN);
			break;
	}
}

/* ================= MAIN ================= */
int main(void)
{
	uint32_t now = 0;
	uint32_t last_adc_ms = 0;
	uint32_t last_dht_ms = 0;
	uint32_t last_lcd_ms = 0;
	uint32_t last_uart_ms = 0;

	uint16_t adc_soil = 0;
	uint16_t adc_lm35 = 0;
	uint16_t temp_x10 = 300; /* 300 nghĩa là 30.0°C */
	uint8_t temp = 30;       /* nhiệt độ từ LM35 */
	uint8_t humidity = 50;   /* độ ẩm từ DHT11 */

	uint8_t new_humidity = 0;

	int soil = 0;

	char statusText[10] = "NORMAL";
	uint8_t statusCode = STATUS_NORMAL;

	uint8_t cmd_copy;
	uint8_t p1_copy;
	uint8_t p2_copy;
	uint8_t p3_copy;
	uint8_t overflow_cmd;

	int sunTemp = 32;
	int sunHum  = 60;
	int sunSoil = 20;

	int rainTemp = 30;
	int rainHum  = 70;
	int rainSoil = 70;

	init_LCD();
	clr_LCD();

	UART_Init();
	ADC_Init();
	Timer0_Init();

	DDRF = 0x00;
	PORTF = 0x00;

	LED_RED_DDR  |= (1 << LED_RED_PIN);
	LED_BLUE_DDR |= (1 << LED_BLUE_PIN);
	SERVO_DDR    |= (1 << SERVO_PIN);
	PUMP_DDR     |= (1 << PUMP_PIN);
	LIGHT_DDR    |= (1 << LIGHT_PIN);

	red_off();
	blue_off();
	pump_off();
	light_off();

	servo_state = 0;
	mode = MODE_AUTO;

	sei();

	UART_SendResponse(RESP_ACK, 0xFE, ACK_OK, 0, 0, 0, 0, 0, 0);

	while(1)
	{
		now = millis_atmega();

		/* ===== Nhận và xử lý toàn bộ lệnh đang chờ từ ESP32 ===== */
		while(queue_pop(&cmd_copy, &p1_copy, &p2_copy, &p3_copy))
		{
			process_binary_command(
				cmd_copy,
				p1_copy,
				p2_copy,
				p3_copy,
				&sunTemp,
				&sunHum,
				&sunSoil,
				&rainTemp,
				&rainHum,
				&rainSoil
			);
		}

		if(queue_take_overflow(&overflow_cmd))
		{
			UART_SendAck(overflow_cmd, ACK_QUEUE_FULL);
		}

		/* ===== Đọc soil + LM35 mỗi 300 ms ===== */
		if(now - last_adc_ms >= 300)
		{
			last_adc_ms = now;

			/* Soil ở ADC0 / PF0 */
			adc_soil = read_adc_avg(SOIL_ADC_CH);
			soil = soil_percent_from_adc(adc_soil);

			/* LM35 ở ADC1 / PF1: đọc nhiều mẫu + lọc EMA + chống nhảy LCD */
			adc_lm35 = read_adc_lm35_stable(LM35_ADC_CH);
			temp_x10 = lm35_temp_x10_filtered(adc_lm35);
			temp = lm35_temp_display_from_x10(temp_x10);
		}

		/* ===== Đọc DHT11 chỉ lấy độ ẩm mỗi 2000 ms ===== */
		if(now - last_dht_ms >= 2000)
		{
			last_dht_ms = now;

			if(read_dht11_humidity_safe(&new_humidity))
			{
				if(new_humidity <= 100)
				{
					humidity = new_humidity;
				}
			}
		}

		/* ===== Tính trạng thái ===== */
		if(temp >= sunTemp && humidity <= sunHum && soil <= sunSoil)
		{
			statusCode = STATUS_HOT_DRY;
			strcpy(statusText, "HOT_DRY");
		}
		else if(temp <= rainTemp && humidity >= rainHum && soil >= rainSoil)
		{
			statusCode = STATUS_RAIN;
			strcpy(statusText, "RAIN");
		}
		else
		{
			statusCode = STATUS_NORMAL;
			strcpy(statusText, "NORMAL");
		}

		/* ===== AUTO / MANUAL ===== */
		if(mode == MODE_AUTO)
		{
			if(statusCode == STATUS_HOT_DRY)
			{
				red_on();
				blue_off();
				Servo_SetTarget(0);
				pump_on();
			}
			else if(statusCode == STATUS_RAIN)
			{
				blue_on();
				red_off();
				Servo_SetTarget(1);
				pump_off();
			}
			else
			{
				red_off();
				blue_off();
				Servo_SetTarget(0);
				pump_off();
			}
		}
		else
		{
			red_off();
			blue_off();
		}

		/* ===== LCD mỗi 500 ms ===== */
		if(now - last_lcd_ms >= 500)
		{
			last_lcd_ms = now;

			move_LCD(1,1);
			printf_LCD("S:%3d%% H:%3d%% ", soil, humidity);

			move_LCD(2,1);
			printf_LCD("T:%2d %-8s %c", temp, statusText, (mode == MODE_AUTO) ? 'A' : 'M');
		}

		/* ===== Gui sensor frame moi 500 ms ===== */
		if(now - last_uart_ms >= 500)
		{
			last_uart_ms = now;

			UART_SendResponse(
				RESP_SENSOR,
				(uint8_t)soil,
				temp,
				humidity,
				statusCode,
				(mode == MODE_AUTO) ? MODE_AUTO : MODE_MANUAL,
				(PUMP_PORT & (1 << PUMP_PIN)) ? 1 : 0,
				servo_state ? 1 : 0,
				(LIGHT_PORT & (1 << LIGHT_PIN)) ? 1 : 0
			);
		}

		/* ===== Giu servo lien tuc giong filemauservo.c ===== */
		Servo_Hold();
	}
}
