/*
 * File	: uart.c
 * This file is part of Espressif's AT+ command set program.
 * Copyright (C) 2013 - 2016, Espressif Systems
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 * ----------------------------------------------------------------------------
 * Heavily modified and enhanced by Thorsten von Eicken in 2015
 */
#include "espmissingincludes.h"
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
#include "uart.h"

#define recvTaskPrio        0
#define recvTaskQueueLen    64

// UartDev is defined and initialized in rom code.
extern UartDevice    UartDev;

os_event_t		recvTaskQueue[recvTaskQueueLen];

#define MAX_CB 4
static UartRecv_cb uart_recv_cb[4];

static void uart0_rx_intr_handler(void *para);

/******************************************************************************
 * FunctionName : uart_config
 * Description  : Internal used function
 *                UART0 used for data TX/RX, RX buffer size is 0x100, interrupt enabled
 *                UART1 just used for debug output
 * Parameters   : uart_no, use UART0 or UART1 defined ahead
 * Returns      : NONE
*******************************************************************************/
static void ICACHE_FLASH_ATTR
uart_config(uint8 uart_no)
{
  if (uart_no == UART1) {
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_U1TXD_BK);
		//PIN_PULLDWN_DIS(PERIPHS_IO_MUX_GPIO2_U);
		PIN_PULLUP_DIS(PERIPHS_IO_MUX_GPIO2_U);
  } else {
    /* rcv_buff size is 0x100 */
    ETS_UART_INTR_ATTACH(uart0_rx_intr_handler,  &(UartDev.rcv_buff));
    PIN_PULLUP_DIS (PERIPHS_IO_MUX_U0TXD_U);
    //PIN_PULLDWN_DIS(PERIPHS_IO_MUX_U0TXD_U);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_U0TXD);
    PIN_PULLUP_DIS (PERIPHS_IO_MUX_U0RXD_U);
    //PIN_PULLDWN_DIS(PERIPHS_IO_MUX_U0RXD_U);
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, 0); // FUNC_U0RXD==0
  }

  uart_div_modify(uart_no, UART_CLK_FREQ / (UartDev.baut_rate));

  if (uart_no == UART1)  //UART 1 always 8 N 1
    WRITE_PERI_REG(UART_CONF0(uart_no),
        CALC_UARTMODE(EIGHT_BITS, NONE_BITS, ONE_STOP_BIT));
  else
    WRITE_PERI_REG(UART_CONF0(uart_no),
        CALC_UARTMODE(UartDev.data_bits, UartDev.parity, UartDev.stop_bits));

  //clear rx and tx fifo,not ready
  SET_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);
  CLEAR_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST | UART_TXFIFO_RST);

  if (uart_no == UART0) {
    // Configure RX interrupt conditions as follows: trigger rx-full when there are 80 characters
    // in the buffer (allows for 64-byte HEX records), trigger rx-timeout when the fifo is
    // non-empty and nothing further has been received for 4 character period. Also set the
    // hardware flow-control to trigger when the FIFO holds 100 characters, although we don't
    // really expect the signals to actually be wired up to anything. It doesn't hurt to set
    // the threshold here...
    WRITE_PERI_REG(UART_CONF1(uart_no),
                   ((80 & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S) |
                   ((100 & UART_RX_FLOW_THRHD) << UART_RX_FLOW_THRHD_S) |
                   UART_RX_FLOW_EN |
                   (4 & UART_RX_TOUT_THRHD) << UART_RX_TOUT_THRHD_S |
                   UART_RX_TOUT_EN);
    SET_PERI_REG_MASK(UART_INT_ENA(uart_no), UART_RXFIFO_FULL_INT_ENA |
                   UART_RXFIFO_TOUT_INT_ENA | UART_FRM_ERR_INT_ENA);
  } else {
    WRITE_PERI_REG(UART_CONF1(uart_no),
                   ((UartDev.rcv_buff.TrigLvl & UART_RXFIFO_FULL_THRHD) << UART_RXFIFO_FULL_THRHD_S));
  }

  //clear all interrupt
  WRITE_PERI_REG(UART_INT_CLR(uart_no), 0xffff);
}

/******************************************************************************
 * FunctionName : uart1_tx_one_char
 * Description  : Internal used function
 *                Use uart1 interface to transfer one char
 * Parameters   : uint8 TxChar - character to tx
 * Returns      : OK
*******************************************************************************/
STATUS
uart_tx_one_char(uint8 uart, uint8 c)
{
	//Wait until there is room in the FIFO
	while (((READ_PERI_REG(UART_STATUS(uart))>>UART_TXFIFO_CNT_S)&UART_TXFIFO_CNT)>=100) ;
	//Send the character
	WRITE_PERI_REG(UART_FIFO(uart), c);
	return OK;
}

/******************************************************************************
 * FunctionName : uart1_write_char
 * Description  : Internal used function
 *                Do some special deal while tx char is '\r' or '\n'
 * Parameters   : char c - character to tx
 * Returns      : NONE
*******************************************************************************/
void ICACHE_FLASH_ATTR
uart1_write_char(char c)
{
	//if (c == '\n') uart_tx_one_char(UART1, '\r');
	uart_tx_one_char(UART1, c);
}
void ICACHE_FLASH_ATTR
uart0_write_char(char c)
{
	//if (c == '\n') uart_tx_one_char(UART0, '\r');
	uart_tx_one_char(UART0, c);
}
/******************************************************************************
 * FunctionName : uart0_tx_buffer
 * Description  : use uart0 to transfer buffer
 * Parameters   : uint8 *buf - point to send buffer
 *                uint16 len - buffer len
 * Returns      :
*******************************************************************************/
void ICACHE_FLASH_ATTR
uart0_tx_buffer(char *buf, uint16 len)
{
  uint16 i;

  for (i = 0; i < len; i++)
  {
    uart_tx_one_char(UART0, buf[i]);
  }
}

/******************************************************************************
 * FunctionName : uart0_sendStr
 * Description  : use uart0 to transfer buffer
 * Parameters   : uint8 *buf - point to send buffer
 *                uint16 len - buffer len
 * Returns      :
*******************************************************************************/
void ICACHE_FLASH_ATTR
uart0_sendStr(const char *str)
{
	while(*str)
	{
		uart_tx_one_char(UART0, *str++);
	}
}

static bool rx_bad; // set to true on framing error to avoid printing errors continuously

/******************************************************************************
 * FunctionName : uart0_rx_intr_handler
 * Description  : Internal used function
 *                UART0 interrupt handler, add self handle code inside
 * Parameters   : void *para - point to ETS_UART_INTR_ATTACH's arg
 * Returns      : NONE
*******************************************************************************/
static void // must not use ICACHE_FLASH_ATTR !
uart0_rx_intr_handler(void *para)
{
  /* uart0 and uart1 intr combine togther, when interrupt occur, see reg 0x3ff20020, bit2,
   * bit0 represents uart1 and uart0 respectively */
  uint8 uart_no = UART0;//UartDev.buff_uart_no;

  if(UART_FRM_ERR_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_FRM_ERR_INT_ST))
  {
    if (!rx_bad) os_printf("FRM_ERR\n");
    rx_bad = true;
    //clear rx and tx fifo
    SET_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST);
    CLEAR_PERI_REG_MASK(UART_CONF0(uart_no), UART_RXFIFO_RST);
    // reset interrupt
    WRITE_PERI_REG(UART_INT_CLR(uart_no), UART_FRM_ERR_INT_CLR);
  }

  if(UART_RXFIFO_FULL_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_RXFIFO_FULL_INT_ST))
  {
    //os_printf("fifo fullr\n");
    ETS_UART_INTR_DISABLE();

    system_os_post(recvTaskPrio, 0, 0);
  }
  else if(UART_RXFIFO_TOUT_INT_ST == (READ_PERI_REG(UART_INT_ST(uart_no)) & UART_RXFIFO_TOUT_INT_ST))
  {
    rx_bad = false;
    ETS_UART_INTR_DISABLE();
    //os_printf("stat:%02X",*(uint8 *)UART_INT_ENA(uart_no));
    system_os_post(recvTaskPrio, 0, 0);
  }
}

/******************************************************************************
 * FunctionName : uart_recvTask
 * Description  : system task triggered on receive interrupt, empties FIFO and calls callbacks
*******************************************************************************/
static void ICACHE_FLASH_ATTR
uart_recvTask(os_event_t *events)
{
	while (READ_PERI_REG(UART_STATUS(UART0)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S)) {
		//WRITE_PERI_REG(0X60000914, 0x73); //WTD // commented out by TvE

		// read a buffer-full from the uart
		uint16 length = 0;
		char buf[128];
		while ((READ_PERI_REG(UART_STATUS(UART0)) & (UART_RXFIFO_CNT << UART_RXFIFO_CNT_S)) &&
					 (length < 128)) {
			buf[length++] = READ_PERI_REG(UART_FIFO(UART0)) & 0xFF;
		}
		//os_printf("%d ix %d\n", system_get_time(), length);

		for (int i=0; i<MAX_CB; i++) {
			if (uart_recv_cb[i] != NULL) (uart_recv_cb[i])(buf, length);
		}
	}
	WRITE_PERI_REG(UART_INT_CLR(UART0), UART_RXFIFO_FULL_INT_CLR|UART_RXFIFO_TOUT_INT_CLR);
	ETS_UART_INTR_ENABLE();
}

void ICACHE_FLASH_ATTR
uart0_baud(int rate) {
  uart_div_modify(UART0, UART_CLK_FREQ / rate);
}

/******************************************************************************
 * FunctionName : uart_init
 * Description  : user interface for init uart
 * Parameters   : UartBautRate uart0_br - uart0 bautrate
 *                UartBautRate uart1_br - uart1 bautrate
 * Returns      : NONE
*******************************************************************************/
void ICACHE_FLASH_ATTR
uart_init(UartBautRate uart0_br, UartBautRate uart1_br)
{
  // rom use 74880 baut_rate, here reinitialize
  UartDev.baut_rate = uart0_br;
  uart_config(UART0);
  UartDev.baut_rate = uart1_br;
  uart_config(UART1);
  for (int i=0; i<4; i++) uart_tx_one_char(UART1, '\n');
  for (int i=0; i<4; i++) uart_tx_one_char(UART0, '\n');
  ETS_UART_INTR_ENABLE();

  // install uart1 putc callback
  os_install_putc1((void *)uart0_write_char);

	system_os_task(uart_recvTask, recvTaskPrio, recvTaskQueue, recvTaskQueueLen);
}

void ICACHE_FLASH_ATTR
uart_add_recv_cb(UartRecv_cb cb) {
	for (int i=0; i<MAX_CB; i++) {
		if (uart_recv_cb[i] == NULL) {
			uart_recv_cb[i] = cb;
			return;
		}
	}
	os_printf("UART: max cb count exceeded\n");
}

void ICACHE_FLASH_ATTR
uart_reattach()
{
	uart_init(BIT_RATE_74880, BIT_RATE_74880);
//  ETS_UART_INTR_ATTACH(uart_rx_intr_handler_ssc,  &(UartDev.rcv_buff));
//  ETS_UART_INTR_ENABLE();
}
