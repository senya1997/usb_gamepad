#include "defines.h"

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include "usbdrv/usbdrv.h"
#include "descriptor.h"

#ifdef DEBUG
	#warning "DEBUG is enabled"
#endif

#ifdef PROTEUS
	#warning "PROTEUS SIM is enabled"
#endif

uchar report_buf[REPORT_SIZE] = {0x7F, 0x7F, 0x7F, 0x7F, 0x00, 0x00};  // 1st pl axis, 2nd pl axis, 1st pl buttons, 2nd pl buttons
	
uchar delay_idle = INIT_IDLE_TIME; // step - 4ms
uchar cnt_idle = 0;

uchar flag_idle = 0; // shows that idle time is over and can send report
uchar flag_report = 0;

// SEGA var and protocol:
	uchar state = 0; // 0..7 states
	uchar flag_ch_gp = 1; // shows that required save buttons state
	
/*  _____________________________
	|Sel |D0 |D1 |D2 |D3 |D4 |D5 |
	+----+---+---+---+---+---+---+
0:	| L  |UP |DW |LO |LO |A  |ST |
1:	| H  |UP |DW |LF |RG |B  |C  |
2:	| L  |UP |DW |LO |LO |A  |ST |
3:	| H  |UP |DW |LF |RG |B  |C  |
4:	| L  |LO |LO |LO |LO |A  |ST |
5:	| H  |Z  |Y  |X  |MD |HI |HI |
6:	| L  |HI |HI |HI |HI |A  |ST |
7:	| H  |UP |DW |LF |RG |B  |C  |
*/

/************************************************************************/
/* approx timing:  |    500us     |2ms|  500us  :  half of period time  */
/*		   state:	0 1 2 3 ... 7 | 8 | 0 1 2 3 ...						*/
/*					  _   _	    _	      _   _							*/
/*		SEL:	 ____/ \_/ \_... \_______/ \_/ \_...					*/
/************************************************************************/

// PS var and protocol:
	uchar shift_report_buf[REPORT_SIZE];

	uchar cnt_byte = 0;
	uchar cnt_edge = 0;
	uchar cnt_rep_buf = 5;

	uchar offset;

/*********************************************************************************/
/* CLK ~ 7 kHz, issue data LSB on MISO and MOSI on falling edge, read on front   */
/* seq from MC:  0x01 | 0x42 | 0xFF | 0xFF | 0xFF | 0xFF | 0xFF | 0xFF | 0xFF    */
/* seq from JOY: 0xFF | 0x73 | 0x5A | DAT1 | DAT2 | RJX  | RJY  | LJX  | LJY     */
/*		       ___														   _____ */
/*		   CS:	  |_______________________________________________________|	     */
/*             _____   _   _   _   _   _   _   _   _____   _  		 _	 _______ */
/*		  CLK:      |_| |_| |_| |_| |_| |_| |_| |_|     |_| |_ ... _| |_|		 */
/*					  r   r   r   r   r   r   r   r   r  						 */
/* MOSI, MISO: -----.000.111.222.333.444.555.666.777.---.000.1 ... 666.777.----- */
/*			   _____________________________________1CLK______ ... _____________ */
/*		  ACK:									    |__|						 */
/*********************************************************************************/

USB_PUBLIC uchar usbFunctionDescriptor(usbRequest_t * rq)
{
	if (rq->bRequest == USBRQ_GET_DESCRIPTOR)
	{
		switch(rq -> wValue.bytes[1])
		{
			case USBDESCR_DEVICE:
				usbMsgPtr = (usbMsgPtr_t)desc_dev;
				return sizeof(desc_dev);
			case USBDESCR_CONFIG:
				usbMsgPtr = (usbMsgPtr_t)desc_conf;
				return sizeof(desc_conf);
			case USBDESCR_STRING:
				if(rq -> wValue.bytes[0] == 2) // device name
				{
					usbMsgPtr = (usbMsgPtr_t)desc_prod_str;
					return sizeof(desc_prod_str);
				}
		}
	}
	
	return 0x00;
}

USB_PUBLIC uchar usbFunctionSetup(uchar data[8])
{
	usbRequest_t *rq = (usbRequest_t*)data;
	
	if((rq -> bmRequestType & USBRQ_TYPE_MASK) == USBRQ_TYPE_CLASS) // class request type
	{    
		switch (rq -> bRequest)
		{
			case USBRQ_HID_GET_REPORT:
				usbMsgPtr = (usbMsgPtr_t)report_buf;
				return REPORT_SIZE;
			case USBRQ_HID_GET_IDLE:
				if (rq -> wValue.bytes[0] > 0)
				{
					usbMsgPtr = (usbMsgPtr_t)&delay_idle;
					return 1;
				}
				break;
			case USBRQ_HID_SET_IDLE: return USB_NO_MSG; // call "usbFunctionWrite" ("OUT" token)
			/*
			case USBRQ_HID_SET_IDLE:
				// mb required reset "cnt_idle" and enable interrupt timer 0, cause 
				// "flag_idle" can be set during "delay_idle" change in large way
				if(rq -> wValue.bytes[1] != 0) delay_idle = rq -> wValue.bytes[1];
				else delay_idle = INIT_IDLE_TIME; // when the upper byte of "wValue" = 0, the duration is indefinite
				*/
		}
	}
	
	return 0; // ignore data from host ("OUT" token)
}

USB_PUBLIC uchar usbFunctionWrite(uchar *data, uchar len)
{
	usbRequest_t *rq = (usbRequest_t*)data;
	
	if(rq -> wValue.bytes[1] != 0) delay_idle = rq -> wValue.bytes[1];
	else delay_idle = INIT_IDLE_TIME; // when the upper byte of "wValue" = 0, the duration is indefinite
	
	return 1;
}

void hardware_SEGA_Init()
{
	DDR_LED = (1 << LED0) | (1 << LED1);
	
	// gamepads:
		DDR_SEGA_AUX = (1 << SEGA_SEL);
		PORT_SEGA_AUX &= ~(1 << SEGA_SEL); // necessarily down to zero SEL signal on start
	
		PORT_SEGA1 = (1 << SEGA_LF_X) | (1 << SEGA_RG_MD) | (1 << SEGA_UP_Z) | (1 << SEGA_DW_Y) | // add pullup (mb not required)
					 (1 << SEGA_A_B) | (1 << SEGA_ST_C);
					 
		PORT_SEGA2 = (1 << SEGA_LF_X) | (1 << SEGA_RG_MD) | (1 << SEGA_UP_Z) | (1 << SEGA_DW_Y) |
					 (1 << SEGA_A_B) | (1 << SEGA_ST_C);
	
	// timers:
		// for idle time:
			TCCR0A = (1 << WGM01); // CTC mode with OCRA
			TCCR0B = (1 << CS02); // presc = 256 => 4 ms <=> 250 cnt
			OCR0A = STEP_IDLE_CONF;
	
		// for SEGA SEL clock:
			TCCR2A = (1 << WGM21); // CTC mode with OCRA
			TCCR2B = (1 << CS20) | (1 << CS22); // presc = 128 => 2 ms <=> 250 cnt; 500 us <=> 62.5
			OCR2A = PER_POLL_GP;
	
	TIMSK0 = (1 << OCIE0A);
	TIMSK2 = (1 << OCIE2A);
}

void hardware_PS_Init()
{
	PORT_LED &= ~(1 << LED1); // PS mode
	
	// this func call after "hardware_SEGA_init"
		DDR_PS = (1 << PS_CS) | (1 << PS_MOSI) | (1 << PS_CLK); // outputs
		DDR_PS &= ~(1 << PS_MISO); // inputs

	// add pullup on inputs and issue one on outputs:
		PORT_PS = (1 << PS_MISO) | (1 << PS_CS) | (1 << PS_MOSI) | (1 << PS_CLK);
	
	// for PS CLK:
		TCCR2A = (1 << WGM21); // CTC mode with OCRA
		TCCR2B = (1 << CS20) | (1 << CS22); // presc = 128 => 2 ms <=> 250 cnt; 500 us <=> 62.5
		OCR2A = PER_POLL_GP;
	
	TIMSK2 = (1 << OCIE2A);
}

void upd_SEGA_ReportBuf(uchar offset, uchar *gp_state_ptr) // offset defines by player number: 1st - "0", 2nd - "8"
{
	uchar temp, pl_offset;
	
	if(offset == 0) pl_offset = 0;
	else pl_offset = 2;
	
	// 2,3,5 - SEL number at which data were polling in protocol (see "state" comment)
	temp = (~(*(gp_state_ptr + 3 + offset))) & LF_RG_MASK; // inv, because on SEGA 1 - not pressed; in descriptor 1 - pressed
	
		if(temp == LF_MASK) report_buf[1 + pl_offset] = 0xFF; // left
		else if(temp == RG_MASK) report_buf[1 + pl_offset] = 0x00; // right
		else report_buf[1 + pl_offset] = 0x7F;
	
	temp = (~(*(gp_state_ptr + 3 + offset))) & UP_DW_MASK;
	
		if(temp == UP_MASK) report_buf[0 + pl_offset] = 0xFF; // up
		else if(temp == DW_MASK) report_buf[0 + pl_offset] = 0x00; // down
		else report_buf[0 + pl_offset] = 0x7F;
	
	report_buf[4 + pl_offset/2] = (~(*(gp_state_ptr + 5 + offset))) & ZYX_MD_MASK;
	report_buf[4 + pl_offset/2] |= (~(*(gp_state_ptr + 3 + offset))) & C_B_MASK;
	report_buf[4 + pl_offset/2] |= ((~(*(gp_state_ptr + 2 + offset))) & ST_A_MASK) << 2;
}

inline void clearShiftBuf()
{
	if(!flag_report)
	{
		shift_report_buf[0] = 0;
		shift_report_buf[1] = 0;
		shift_report_buf[2] = 0;
		shift_report_buf[3] = 0;
		shift_report_buf[4] = 0;
		shift_report_buf[5] = 0;
	}
	
	cnt_rep_buf = 5;
	
	cnt_edge = 0;
	cnt_byte = 0;
	
	PORT_PS |= (1 << PS_CS) | (1 << PS_CLK);
	TIFR0 |= (1 << OCF0A);
}

ISR(TIMER0_COMPA_vect)
{
	TIMSK2 &= ~(1 << OCIE2A); // "cli" for avoid nested interrupts
		sei();
		
		if(cnt_idle < delay_idle) cnt_idle++;
		else
		{
			TIMSK0 &= ~(1 << OCIE0A);
			flag_idle = 1;
		}
	TIMSK2 |= (1 << OCIE2A); // "sei"
}

ISR(TIMER2_COMPA_vect)
{
	TIMSK0 &= ~(1 << OCIE0A); // "cli"
		sei();
	
		if(state < 7) 
		{
			PORT_SEGA_AUX ^= (1 << SEGA_SEL);
		
			flag_ch_gp = 1;
			state++;
		}
		else if(state == 8)
		{ // after delay between "packets":
			OCR2A = PER_POLL_GP;
			flag_ch_gp = 1;
			state = 0;
		}
		else
		{ // after "packet":
			PORT_SEGA_AUX &= ~(1 << SEGA_SEL);
			OCR2A = DELAY_BTW_POLL;
		
			flag_report = 1;
			state = 8;
		}
	TIMSK0 |= (1 << OCIE0A); // "sei"
}

ISR(TIMER1_OVF_vect)
{
	TIMSK0 &= ~(1 << OCIE0A);
	TIMSK2 &= ~(1 << OCIE2A);
	
	sei();
	
	if(cnt_idle < delay_idle) cnt_idle++;
	else
	{
		TIMSK1 &= ~(1 << TOIE1);
		flag_idle = 1;
	}
	
	TIMSK0 |= (1 << OCIE0A);
	TIMSK2 |= (1 << OCIE2A);
}

ISR(TIMER2_COMPA_vect)
{
	TCNT0 = 5; // reset duplicate timer
	TIMSK0 &= ~(1 << OCIE0A); // "cli"
	
	sei();
	
	if((TIFR0 & (1 << OCF0A)) == (1 << OCF0A)) clearShiftBuf();
	
	// CLK:
	if((cnt_byte > 0) & ACT_TRANS) PORT_PS ^= (1 << PS_CLK);
	
	// MISO:
	if((cnt_byte > 3) & ACT_TRANS & ((cnt_edge & 0x01) == 1)) // odd "cnt_edge"
	{
		offset = (cnt_edge - 1) >> 1;
		shift_report_buf[cnt_rep_buf] |= (PIN_PS & 0x01) << offset;
	}
	
	// MOSI:
	if(((cnt_byte == 1) & (cnt_edge < 2)) |
	((cnt_byte == 2) & ((cnt_edge == 2) | (cnt_edge == 3) | (cnt_edge == 12) | (cnt_edge == 13)))) PORT_PS |= (1 << PS_MOSI);
	else PORT_PS &= ~(1 << PS_MOSI);
	
	// CS:
	if((cnt_byte == 0) & IDLE_STATE) PORT_PS &= ~(1 << PS_CS);
	else if(LAST_BYTE & END_ONE_BYTE) PORT_PS |= (1 << PS_CS);
	
	// counters - required go to ASM:
	if(cnt_byte == 0) // between byte transmit
	{
		if(IDLE_STATE)
		{
			cnt_edge = 0;
			cnt_byte++;
		}
		else cnt_edge++;
	}
	else if((cnt_byte > 0) & (cnt_byte < 4))
	{
		if(END_ONE_BYTE)
		{
			cnt_edge = 0;
			cnt_byte++;
		}
		else cnt_edge++;
	}
	else
	{
		if(END_ONE_BYTE)
		{
			cnt_edge = 0;
			cnt_rep_buf--;
			
			if(LAST_BYTE)
			{
				flag_report = 1;
				cnt_byte = 0;
				
				//TIMSK1 &= ~(1 << OCIE1A);
			}
			else cnt_byte++;
		}
		else cnt_edge++;
	}
	
	TIMSK0 |= (1 << OCIE0A); // "sei"
}

void main_PS(void) // if activate PS mode by spec key combination on SEGA controller
{
	char flag_report_rdy = 0;
	
	cli();
	
	hardware_PS_Init();
	
	// full reset timers:
		TCNT0 = 0;
		TCNT1 = 0;
		TCNT2 = 0;
	
		TIFR0 |= (1 << OCF0A);
		TIFR1 |= (1 << TOV1);
		TIFR2 |= (1 << OCF2A);
	
	sei();
	while (1)
	{
		if(flag_idle & flag_report_rdy) // send report immediately after "idle" time has passed:
		{
			if(usbInterruptIsReady())
			{
				#ifndef DEBUG
					usbSetInterrupt(report_buf, REPORT_SIZE);  // ~ 31.5 us
				#endif
				
				#ifdef PROTEUS
					usbSetInterrupt(report_buf, REPORT_SIZE);
				#endif
				
				//clearShiftBuf();
				flag_report_rdy = 0;
				
				// full reset idle timer then enable interrupt:
				TCNT1 = 0;
				TIFR1 |= (1 << TOV1);
				TIMSK1 |= (1 << TOIE1);
			}
		}
		
		if(flag_report) // build report:
		{
			report_buf[0] = shift_report_buf[1]; // mb required "turn over" descriptor
			report_buf[1] = shift_report_buf[0];
			report_buf[2] = shift_report_buf[3];
			report_buf[3] = shift_report_buf[2];
			report_buf[4] = ~shift_report_buf[4];
			report_buf[5] = ~shift_report_buf[5];
			
			shift_report_buf[0] = 0;
			shift_report_buf[1] = 0;
			shift_report_buf[2] = 0;
			shift_report_buf[3] = 0;
			shift_report_buf[4] = 0;
			shift_report_buf[5] = 0;
			
			cnt_rep_buf = 5;
			
			PORT_PS |= (1 << PS_CS) | (1 << PS_CLK);
			
			flag_report = 0;
			flag_report_rdy = 1;
		}
		
		if((report_buf[4] != 0x00) | (report_buf[5] != 0x00)) PORT_LED ^= (1 << LED0);
		
		if((cnt_byte == 0) & (cnt_edge == 10))
		{
			usbPoll();
		}
	}
}

void main(void)
{
	// to enable PS controllers, hold START on SEGA 1st controller before and after connect device to PC on 2-3 sec
	// otherwise by default work SEGA controller mode
	uchar flag_first_run = 1;
	
	#ifdef DEBUG
		uchar gp_state_buf[2][8] = {0x00, 0x00, 0x10, 0x15, 0x00, 0x0A, 0x00, 0x00,
									0x00, 0x00, 0x20, 0x2A, 0x00, 0x05, 0x00, 0x00};
	#else
		uchar gp_state_buf[2][8];
	#endif
	
	hardware_SEGA_Init();
	
	#ifndef DEBUG
		usbDeviceConnect();
		usbInit();
	#endif
	
	// full reset timers:
		TCNT0 = 0;
		TCNT2 = 0;
	
	// reset interrupt timers flags:
		TIFR0 |= (1 << OCF0A);
		TIFR2 |= (1 << OCF2A);
	
		GTCCR |= (1 << PSRASY); // reset presc timers
	
	sei();
	while (1)
	{
		#ifndef DEBUG
			usbPoll(); // ~ 9.63 us (all timings write in 16 MHz CPU freq)
		#endif
		
		if(flag_idle) // send report immediately after "idle" time has passed:
		{
			if(usbInterruptIsReady())
			{
				#ifndef DEBUG
					usbSetInterrupt(report_buf, REPORT_SIZE);  // ~ 18.06 us ???
				#endif
				
				cnt_idle = 0;
				flag_idle = 0;
				
				// full reset timer 0 then enable interrupt:
				TCNT0 = 0;
				TIFR0 |= (1 << OCF0A);
				TIMSK0 |= (1 << OCIE0A);
			}
		}
		
		if(flag_report) // build report:
		{
			upd_SEGA_ReportBuf(0, (uchar *)gp_state_buf); // 1st player
			upd_SEGA_ReportBuf(8, (uchar *)gp_state_buf); // 2nd player
			
			if(flag_first_run)
			{
				flag_first_run = 0;
				
				if((report_buf[4] != PS_ON))
				{
					main_PS();
				}
			}
			
			flag_report = 0;
		}
		
		if(flag_ch_gp & (TCNT2 >= DELAY_BEF_POLL)) // upd gamepad status buffer:
		{
			gp_state_buf[0][state] = PIN_SEGA1 & SEGA_PIN_MASK;
			gp_state_buf[1][state] = PIN_SEGA2 & SEGA_PIN_MASK;
			flag_ch_gp = 0;
		}
		
		if((report_buf[4] != 0x00) | (report_buf[5] != 0x00)) PORT_LED ^= (1 << LED0);
	}
}