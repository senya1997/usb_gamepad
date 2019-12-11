#include "defines.h"

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

#include "usbdrv/usbdrv.h"
#include "descriptor.h"

#ifdef DEBUG
	#warning "DEBUG is enabled"
#endif

uchar report_buf[REPORT_SIZE] = {0x7F, 0x7F, 0x7F, 0x7F, 0x00, 0x00};

uchar delay_idle = INIT_IDLE_TIME; // step - 4ms
uchar cnt_idle = 0;

// SEGA var and protocol:
	uchar state = 0; // 0..7 states
	
	uchar flag_ch_gp = 1; // shows that required save gamepad state
	uchar flag_sega = 0; // to enable SEGA controllers, hold spec comb on 2nd SEGA controller before and after connect device
						 // to PC on 2-3 sec (see "defines.h")
	
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
	uchar temp_report_buf[REPORT_SIZE];

	uchar cnt_data = 0;
	uchar cnt_edge = 0;
	
	uchar flag_ps = 0; // analog "flag_sega"
		
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

uchar flag_idle = 0; // shows that idle time is over and can send report
uchar flag_report = 0; // shows that information from gamepad is ready to form like in descriptor

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
	
	report_buf[4 + (pl_offset >> 1)] = (~(*(gp_state_ptr + 5 + offset))) & ZYX_MD_MASK;
	report_buf[4 + (pl_offset >> 1)] |= (~(*(gp_state_ptr + 3 + offset))) & C_B_MASK;
	report_buf[4 + (pl_offset >> 1)] |= ((~(*(gp_state_ptr + 2 + offset))) & ST_A_MASK) << 2;
}

void hardware_SEGA_Init()
{
	DDR_LED = (1 << LED0) | (1 << LED1);
		
// gamepads:
	DDR_SEGA_AUX |= (1 << SEGA_SEL);
	PORT_SEGA_AUX &= ~(1 << SEGA_SEL); // necessarily down to zero SEL signal on start
		
	PORT_SEGA1 = (1 << SEGA_LF_X) | (1 << SEGA_RG_MD) | (1 << SEGA_UP_Z) | (1 << SEGA_DW_Y) | // add pullup (mb not required)
				 (1 << SEGA_A_B) | (1 << SEGA_ST_C);
	PORT_SEGA2 = (1 << SEGA_LF_X) | (1 << SEGA_RG_MD) | (1 << SEGA_UP_Z) | (1 << SEGA_DW_Y) |
				 (1 << SEGA_A_B) | (1 << SEGA_ST_C);
	
// timers, DO NOT forget to approve with CPU freq:
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
// this func call after "hardware_SEGA_init" when PS mode is activating
	DDR_PS |= (1 << PS_CS) | (1 << PS_MOSI) | (1 << PS_CLK); // outputs
	DDR_PS &= ~(1 << PS_MISO) & ~(1 << PS_ACK); // inputs

// add pullup on inputs and issue one on outputs:
	PORT_PS = (1 << PS_MISO) | (1 << PS_ACK) | (1 << PS_CS) | (1 << PS_MOSI) | (1 << PS_CLK);
	
// for PS CLK ~ 7 kHz, DO NOT forget to approve with CPU freq:
	TCCR1B = (1 << WGM12) | (1 << CS10); // CTC mode with OCR1A, presc = 1 => half period CLK 71.4285 us <=> 1142.856 cnt
	OCR1A = CLK_HALF_PER;
	TIMSK1 = (1 << OCIE1A);
	
// full reset timer:
	//TCNT2 = 0;
	//TIFR2 |= (1 << OCF2A);
	//GTCCR |= (1 << PSRASY); // reset presc timers (include "idle timer" presc)
}

ISR(TIMER0_COMPA_vect)
{
	if(flag_sega) TIMSK2 &= ~(1 << OCIE2A); // "cli" for avoid nested interrupts
	if(flag_ps) TIMSK1 &= ~(1 << OCIE1A);
	
		sei();
		
		if(cnt_idle < delay_idle) cnt_idle++;
		else
		{
			TIMSK0 &= ~(1 << OCIE0A);
			flag_idle = 1;
		}
		
	if(flag_sega) TIMSK2 |= (1 << OCIE2A); // "sei"
	if(flag_ps) TIMSK1 |= (1 << OCIE1A);
}

ISR(TIMER1_COMPA_vect)
{
	TIMSK0 &= ~(1 << OCIE0A); // "cli"
	sei();
	
	// CLK:
	if((cnt_data > 0) & (cnt_edge <= 15)) PORT_PS ^= (1 << PS_CLK); // mb "cnt" var go to reg for faster using
	
	// MISO:
	if((cnt_data > 2) & (cnt_edge <= 15) & ((cnt_edge & 0x01) == 1)) // odd "cnt_edge"
		temp_report_buf[cnt_data - 4] |= (PIN_PS & (1 << PS_MISO)) << ((cnt_edge - 1) >> 1);
	
	// MOSI:
	if(cnt_data == 2) 
	{
		if((cnt_edge == 18) | (cnt_edge == 3) | (cnt_edge == 4) | (cnt_edge == 13) | (cnt_edge == 14)) 
			PORT_PS |= (1 << PS_MOSI);
		else PORT_PS &= ~(1 << PS_MOSI);
	}
	else if((cnt_data == 1) & (cnt_edge == 2)) PORT_PS &= ~(1 << PS_MOSI);
	
	// CS:
	if((cnt_data == 9) & (cnt_edge == 18)) PORT_PS |= (1 << PS_CS);
	else if((cnt_data == 0) & (cnt_edge == 2)) PORT_PS &= ~(1 << PS_CS);

		if(cnt_data == 0)
		{
			if(cnt_edge == 2)
			{
				cnt_edge = 0;
				cnt_data++;
			}
			else cnt_edge++;
		}
		else if((cnt_data == 1) | (cnt_data == 2))
		{
			if(cnt_edge == 18)
			{
				cnt_edge = 0;
				cnt_data++;
			}
			else if(cnt_edge > 15) cnt_edge++;
			else cnt_edge++;
		}
		else
		{
			if(cnt_edge == 18)
			{
				cnt_edge = 0;
				
				if(cnt_data == 9)
				{
					flag_report = 1;
					cnt_data = 0;
				}
				else cnt_data++;
			}
			else if(cnt_edge > 15) cnt_edge++;
			else cnt_edge++;
		}

	TIMSK0 |= (1 << OCIE0A); // "sei"
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

void main(void)
{
	uchar flag_first_run = 1;
	
	#ifdef DEBUG
		#ifdef DEBUG_SEGA
			uchar gp_state_buf[2][8] = {0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00,
										0x00, 0x00, 0xFF, 0xFF, 0x00, ~SEGA_ON, 0x00, 0x00};
			#warning "SEGA MODE DEBUG is enabled"
		#else
			#ifdef DEBUG_PS
				uchar gp_state_buf[2][8] = {0x00, 0x00, 0xFF, 0xFF, 0x00, ~PS_ON, 0x00, 0x00,
											0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00};
				#warning "PS MODE DEBUG is enabled"
			#else
				uchar gp_state_buf[2][8] = {0x00, 0x00, 0x10, 0x15, 0x00, 0x0A, 0x00, 0x00,
											0x00, 0x00, 0x20, 0x2A, 0x00, 0x05, 0x00, 0x00};
			#endif
		#endif		
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
					usbSetInterrupt(report_buf, REPORT_SIZE);  // ~ 31.5 us
				#endif
				
				cnt_idle = 0;
				flag_idle = 0;
				
			// full reset idle timer then enable interrupt:
				TCNT0 = 0;
				TIFR0 |= (1 << OCF0A); 
				TIMSK0 |= (1 << OCIE0A);
				
				if(flag_sega) PORT_LED |= (1 << LED1);
				else if(flag_ps) PORT_LED |= (1 << LED0);
			}
		}
		
		if(flag_report) // build report:
		{ 
			if(flag_sega | flag_first_run)
			{
				upd_SEGA_ReportBuf(0, (uchar *)gp_state_buf); // 1st player
				upd_SEGA_ReportBuf(8, (uchar *)gp_state_buf); // 2nd player
			}
			else if(flag_ps)
			{
				report_buf[0] = temp_report_buf[5]; // mb required "turn over" descriptor
				report_buf[1] = temp_report_buf[4];
				report_buf[2] = temp_report_buf[3];
				report_buf[3] = temp_report_buf[2];
				report_buf[4] = temp_report_buf[1];
				report_buf[5] = temp_report_buf[0];
			}
			
			if(flag_first_run) // 2nd pl SEGA <=> PS joy
			{	//			2nd pl						1st pl
				if((report_buf[5] == SEGA_ON) & (report_buf[4] != PS_ON)) flag_sega = 1;
				else if((report_buf[5] != SEGA_ON) & (report_buf[4] == PS_ON))
				{
					TCCR2B &= ~(1 << CS20) & ~(1 << CS21) & ~(1 << CS22);
					TIMSK2 &= ~(1 << OCIE2A); // disable SEGA interrupt (enable by default)
					
					flag_ps = 1;
					flag_ch_gp = 0;
					
					hardware_PS_Init();
				}
				else 
				{
					TCCR2B &= ~(1 << CS20) & ~(1 << CS21) & ~(1 << CS22);
					TIMSK2 &= ~(1 << OCIE2A);
				}
				
				flag_first_run = 0;
			} 
			
			flag_report = 0;
		}
		
		if(flag_ch_gp & (TCNT2 >= DELAY_BEF_POLL)) // upd gamepad status buffer:
		{
			gp_state_buf[0][state] = PIN_SEGA1 & SEGA_PIN_MASK;
			gp_state_buf[1][state] = PIN_SEGA2 & SEGA_PIN_MASK;
			flag_ch_gp = 0;
		}
		
		// LEDs:
		if((report_buf[4] != 0x00) | (report_buf[5] != 0x00)) // if buttons not pressed
		{
			if(flag_sega) PORT_LED ^= (1 << LED0);
			else if(flag_ps) PORT_LED ^= (1 << LED1);
		}
    }
}