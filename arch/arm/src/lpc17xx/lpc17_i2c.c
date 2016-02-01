/****************************************************************************
 * arch/arm/src/lpc17xx/lpc17_i2c.c
 *
 *   Copyright (C) 2012, 2014-2016 Gregory Nutt. All rights reserved.
 *   Copyright (C) 2011 Li Zhuoyi. All rights reserved.
 *   Author: Li Zhuoyi <lzyy.cn@gmail.com> (Original author)
 *           Gregory Nutt <gnutt@nuttx.org>
 *
 * Derived from arch/arm/src/lpc31xx/lpc31_i2c.c
 *
 *   Author: David Hewson
 *
 *   Copyright (C) 2010-2011 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/wdog.h>
#include <nuttx/i2c/i2c_master.h>

#include <arch/irq.h>
#include <arch/board/board.h>

#include "chip.h"
#include "up_arch.h"
#include "up_internal.h"

#include "chip.h"
#include "chip/lpc17_syscon.h"
#include "lpc17_gpio.h"
#include "lpc17_i2c.h"

#if defined(CONFIG_LPC17_I2C0) || defined(CONFIG_LPC17_I2C1) || defined(CONFIG_LPC17_I2C2)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef GPIO_I2C1_SCL
#  define GPIO_I2C1_SCL GPIO_I2C1_SCL_1
#  define GPIO_I2C1_SDA GPIO_I2C1_SDA_1
#endif

#ifndef CONFIG_LPC17_I2C0_FREQUENCY
#  define CONFIG_LPC17_I2C0_FREQUENCY 100000
#endif

#ifndef CONFIG_LPC17_I2C1_FREQUENCY
#  define CONFIG_LPC17_I2C1_FREQUENCY 100000
#endif

#ifndef CONFIG_LPC17_I2C2_FREQUENCY
#  define CONFIG_LPC17_I2C2_FREQUENCY 100000
#endif

#define I2C_TIMEOUT  (20 * 1000/CONFIG_USEC_PER_TICK) /* 20 mS */
#define LPC17_I2C1_FREQUENCY 400000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct lpc17_i2cdev_s
{
  struct i2c_master_s dev;     /* Generic I2C device */
  struct i2c_msg_s msg;        /* a single message for legacy read/write */
  unsigned int     base;       /* Base address of registers */
  uint16_t         irqid;      /* IRQ for this device */

  sem_t            mutex;      /* Only one thread can access at a time */
  sem_t            wait;       /* Place to wait for state machine completion */
  volatile uint8_t state;      /* State of state machine */
  WDOG_ID          timeout;    /* watchdog to timeout when bus hung */

  struct i2c_msg_s *msgs;      /* remaining transfers - first one is in progress */
  unsigned int     nmsg;       /* number of transfer remaining */

  uint16_t         wrcnt;      /* number of bytes sent to tx fifo */
  uint16_t         rdcnt;      /* number of bytes read from rx fifo */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int      lpc17_i2c_start(struct lpc17_i2cdev_s *priv);
static void     lpc17_i2c_stop(struct lpc17_i2cdev_s *priv);
static int      lpc17_i2c_interrupt(int irq, FAR void *context);
static void     lpc17_i2c_timeout(int argc, uint32_t arg, ...);

/* I2C device operations */

static uint32_t lpc17_i2c_setfrequency(FAR struct i2c_master_s *dev,
                  uint32_t frequency);
static int      lpc17_i2c_setaddress(FAR struct i2c_master_s *dev, int addr,
                  int nbits);
static int      lpc17_i2c_transfer(FAR struct i2c_master_s *dev,
                  FAR struct i2c_msg_s *msgs, int count);
static void     lpc17_stopnext(struct lpc17_i2cdev_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_LPC17_I2C0
static struct lpc17_i2cdev_s g_i2c0dev;
#endif
#ifdef CONFIG_LPC17_I2C1
static struct lpc17_i2cdev_s g_i2c1dev;
#endif
#ifdef CONFIG_LPC17_I2C2
static struct lpc17_i2cdev_s g_i2c2dev;
#endif

struct i2c_ops_s lpc17_i2c_ops =
{
  .setfrequency = lpc17_i2c_setfrequency,
  .setaddress   = lpc17_i2c_setaddress,
  .transfer     = lpc17_i2c_transfer
};

/****************************************************************************
 * Name: lpc17_i2c_setfrequency
 *
 * Description:
 *   Set the frequence for the next transfer
 *
 ****************************************************************************/

static uint32_t lpc17_i2c_setfrequency(FAR struct i2c_master_s *dev,
                                       uint32_t frequency)
{
  struct lpc17_i2cdev_s *priv = (struct lpc17_i2cdev_s *) dev;

  if (frequency > 100000)
    {
      /* Asymetric per 400Khz I2C spec */

      putreg32(LPC17_CCLK / (83 + 47) * 47 / frequency,
               priv->base + LPC17_I2C_SCLH_OFFSET);
      putreg32(LPC17_CCLK / (83 + 47) * 83 / frequency,
               priv->base + LPC17_I2C_SCLL_OFFSET);
    }
  else
    {
      /* 50/50 mark space ratio */

      putreg32(LPC17_CCLK / 100 * 50 / frequency,
               priv->base + LPC17_I2C_SCLH_OFFSET);
      putreg32(LPC17_CCLK / 100 * 50 / frequency,
               priv->base + LPC17_I2C_SCLL_OFFSET);
    }

  /* FIXME: This function should return the actual selected frequency */

  return frequency;
}

/****************************************************************************
 * Name: lpc17_i2c_setaddress
 *
 * Description:
 *   Set the I2C slave address for a subsequent read/write
 *
 ****************************************************************************/

static int lpc17_i2c_setaddress(FAR struct i2c_master_s *dev, int addr,
                                int nbits)
{
  struct lpc17_i2cdev_s *priv = (struct lpc17_i2cdev_s *)dev;

  DEBUGASSERT(dev != NULL);
  DEBUGASSERT(nbits == 7);

  priv->msg.addr  = addr;

  return OK;
}

/****************************************************************************
 * Name: lpc17_i2c_start
 *
 * Description:
 *   Perform a I2C transfer start
 *
 ****************************************************************************/

static int lpc17_i2c_start(struct lpc17_i2cdev_s *priv)
{
  int ret = -1;

  sem_wait(&priv->mutex);

  putreg32(I2C_CONCLR_STAC | I2C_CONCLR_SIC,
           priv->base + LPC17_I2C_CONCLR_OFFSET);
  putreg32(I2C_CONSET_STA, priv->base + LPC17_I2C_CONSET_OFFSET);

  wd_start(priv->timeout, I2C_TIMEOUT, lpc17_i2c_timeout, 1, (uint32_t)priv);
  sem_wait(&priv->wait);

  wd_cancel(priv->timeout);

  ret = priv->nmsg;

  sem_post(&priv->mutex);

  return ret;
}

/****************************************************************************
 * Name: lpc17_i2c_stop
 *
 * Description:
 *   Perform a I2C transfer stop
 *
 ****************************************************************************/

static void lpc17_i2c_stop(struct lpc17_i2cdev_s *priv)
{
  if (priv->state != 0x38)
    {
      putreg32(I2C_CONSET_STO | I2C_CONSET_AA,
               priv->base + LPC17_I2C_CONSET_OFFSET);
    }

  sem_post(&priv->wait);
}

/****************************************************************************
 * Name: lpc17_i2c_timeout
 *
 * Description:
 *   Watchdog timer for timeout of I2C operation
 *
 ****************************************************************************/

static void lpc17_i2c_timeout(int argc, uint32_t arg, ...)
{
  struct lpc17_i2cdev_s *priv = (struct lpc17_i2cdev_s *)arg;

  irqstate_t flags = irqsave();
  priv->state = 0xff;
  sem_post(&priv->wait);
  irqrestore(flags);
}

/****************************************************************************
 * Name: lpc17_i2c_transfer
 *
 * Description:
 *   Perform a sequence of I2C transfers
 *
 ****************************************************************************/

static int lpc17_i2c_transfer(FAR struct i2c_master_s *dev,
                              FAR struct i2c_msg_s *msgs, int count)
{
  struct lpc17_i2cdev_s *priv = (struct lpc17_i2cdev_s *)dev;
  int ret;

  DEBUGASSERT(dev != NULL);

  priv->wrcnt = 0;
  priv->rdcnt = 0;
  priv->msgs  = msgs;
  priv->nmsg  = count;

  ret = lpc17_i2c_start(priv);

  return ret;
}

/****************************************************************************
 * Name: lpc17_i2c_interrupt
 *
 * Description:
 *   Check if we need to issue STOP at the next message
 *
 ****************************************************************************/

static void lpc17_stopnext(struct lpc17_i2cdev_s *priv)
{
  priv->nmsg--;

  if (priv->nmsg > 0)
    {
      priv->msgs++;
      putreg32(I2C_CONSET_STA, priv->base + LPC17_I2C_CONSET_OFFSET);
    }
  else
    {
      lpc17_i2c_stop(priv);
    }
}

/****************************************************************************
 * Name: lpc17_i2c_interrupt
 *
 * Description:
 *   The I2C Interrupt Handler
 *
 ****************************************************************************/

static int lpc17_i2c_interrupt(int irq, FAR void *context)
{
  struct lpc17_i2cdev_s *priv;
  struct i2c_msg_s *msg;
  uint32_t state;

#ifdef CONFIG_LPC17_I2C0
  if (irq == LPC17_IRQ_I2C0)
    {
      priv = &g_i2c0dev;
    }
  else
#endif
#ifdef CONFIG_LPC17_I2C1
  if (irq == LPC17_IRQ_I2C1)
    {
      priv = &g_i2c1dev;
    }
  else
#endif
#ifdef CONFIG_LPC17_I2C2
  if (irq == LPC17_IRQ_I2C2)
    {
      priv = &g_i2c2dev;
    }
  else
#endif
    {
      PANIC();
    }

  /* Reference UM10360 19.10.5 */

  state = getreg32(priv->base + LPC17_I2C_STAT_OFFSET);
  msg  = priv->msgs;

  priv->state = state;
  state &= 0xf8;  /* state mask, only 0xX8 is possible */
  switch (state)
    {

    case 0x08:     /* A START condition has been transmitted. */
    case 0x10:     /* A Repeated START condition has been transmitted. */
      /* Set address */

      putreg32(((I2C_M_READ & msg->flags) == I2C_M_READ) ?
        I2C_READADDR8(msg->addr) :
        I2C_WRITEADDR8(msg->addr), priv->base + LPC17_I2C_DAT_OFFSET);

      /* Clear start bit */

      putreg32(I2C_CONCLR_STAC, priv->base + LPC17_I2C_CONCLR_OFFSET);
      break;

    /* Write cases */

    case 0x18: /* SLA+W has been transmitted; ACK has been received  */
      priv->wrcnt = 0;
      putreg32(msg->buffer[0], priv->base + LPC17_I2C_DAT_OFFSET); /* put first byte */
      break;

    case 0x28: /* Data byte in DAT has been transmitted; ACK has been received. */
      priv->wrcnt++;

      if (priv->wrcnt < msg->length)
        {
          putreg32(msg->buffer[priv->wrcnt], priv->base + LPC17_I2C_DAT_OFFSET); /* Put next byte */
        }
      else
        {
          lpc17_stopnext(priv);
        }
      break;

    /* Read cases */

    case 0x40:  /* SLA+R has been transmitted; ACK has been received */
      priv->rdcnt = 0;
      if (msg->length > 1)
        {
          putreg32(I2C_CONSET_AA, priv->base + LPC17_I2C_CONSET_OFFSET); /* Set ACK next read */
        }
      else
        {
          putreg32(I2C_CONCLR_AAC, priv->base + LPC17_I2C_CONCLR_OFFSET);  /* Do not ACK because only one byte */
        }
      break;

    case 0x50:  /* Data byte has been received; ACK has been returned. */
      priv->rdcnt++;
      msg->buffer[priv->rdcnt - 1] = getreg32(priv->base + LPC17_I2C_BUFR_OFFSET);

      if (priv->rdcnt >= (msg->length - 1))
        {
          putreg32(I2C_CONCLR_AAC, priv->base + LPC17_I2C_CONCLR_OFFSET);  /* Do not ACK any more */
        }
      break;

    case 0x58:  /* Data byte has been received; NACK has been returned. */
      msg->buffer[priv->rdcnt] = getreg32(priv->base + LPC17_I2C_BUFR_OFFSET);
      lpc17_stopnext(priv);
      break;

    default:
      lpc17_i2c_stop(priv);
      break;
    }

  putreg32(I2C_CONCLR_SIC, priv->base + LPC17_I2C_CONCLR_OFFSET); /* clear interrupt */

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_i2cinitialize
 *
 * Description:
 *   Initialise an I2C device
 *
 ****************************************************************************/

struct i2c_master_s *up_i2cinitialize(int port)
{
  struct lpc17_i2cdev_s *priv;

  if (port > 1)
    {
      dbg("lpc I2C Only support 0,1\n");
      return NULL;
    }

  irqstate_t flags;
  uint32_t regval;

  flags = irqsave();

#ifdef CONFIG_LPC17_I2C0
  if (port == 0)
    {
      priv        = &g_i2c0dev;
      priv->base  = LPC17_I2C0_BASE;
      priv->irqid = LPC17_IRQ_I2C0;

      /* Enable clocking */

      regval  = getreg32(LPC17_SYSCON_PCONP);
      regval |= SYSCON_PCONP_PCI2C0;
      putreg32(regval, LPC17_SYSCON_PCONP);

      regval  = getreg32(LPC17_SYSCON_PCLKSEL0);
      regval &= ~SYSCON_PCLKSEL0_I2C0_MASK;
      regval |= (SYSCON_PCLKSEL_CCLK << SYSCON_PCLKSEL0_I2C0_SHIFT);
      putreg32(regval, LPC17_SYSCON_PCLKSEL0);

      /* Pin configuration */

      lpc17_configgpio(GPIO_I2C0_SCL);
      lpc17_configgpio(GPIO_I2C0_SDA);

      /* Set default frequency */

      lpc17_i2c_setfrequency((struct i2c_master_s *)priv,
                             CONFIG_LPC17_I2C0_FREQUENCY);
    }
  else
#endif
#ifdef CONFIG_LPC17_I2C1
  if (port == 1)
    {
      priv        = &g_i2c1dev;
      priv->base  = LPC17_I2C1_BASE;
      priv->irqid = LPC17_IRQ_I2C1;

      /* Enable clocking */

      regval  = getreg32(LPC17_SYSCON_PCONP);
      regval |= SYSCON_PCONP_PCI2C1;
      putreg32(regval, LPC17_SYSCON_PCONP);

      regval  = getreg32(LPC17_SYSCON_PCLKSEL1);
      regval &= ~SYSCON_PCLKSEL1_I2C1_MASK;
      regval |= (SYSCON_PCLKSEL_CCLK << SYSCON_PCLKSEL1_I2C1_SHIFT);
      putreg32(regval, LPC17_SYSCON_PCLKSEL1);

      /* Pin configuration */

      lpc17_configgpio(GPIO_I2C1_SCL);
      lpc17_configgpio(GPIO_I2C1_SDA);

      /* Set default frequency */

      lpc17_i2c_setfrequency((struct i2c_master_s *)priv,
                             CONFIG_LPC17_I2C1_FREQUENCY);
    }
  else
#endif
#ifdef CONFIG_LPC17_I2C2
  if (port == 2)
    {
      priv        = &g_i2c2dev;
      priv->base  = LPC17_I2C2_BASE;
      priv->irqid = LPC17_IRQ_I2C2;

      /* Enable clocking */

      regval  = getreg32(LPC17_SYSCON_PCONP);
      regval |= SYSCON_PCONP_PCI2C2;
      putreg32(regval, LPC17_SYSCON_PCONP);

      regval  = getreg32(LPC17_SYSCON_PCLKSEL1);
      regval &= ~SYSCON_PCLKSEL1_I2C2_MASK;
      regval |= (SYSCON_PCLKSEL_CCLK << SYSCON_PCLKSEL1_I2C2_SHIFT);
      putreg32(regval, LPC17_SYSCON_PCLKSEL1);

      /* Pin configuration */

      lpc17_configgpio(GPIO_I2C2_SCL);
      lpc17_configgpio(GPIO_I2C2_SDA);

      /* Set default frequency */

      lpc17_i2c_setfrequency((struct i2c_master_s *)priv,
                             CONFIG_LPC17_I2C2_FREQUENCY);
    }
  else
#endif
    {
      return NULL;
    }

  irqrestore(flags);

  putreg32(I2C_CONSET_I2EN, priv->base + LPC17_I2C_CONSET_OFFSET);

  sem_init(&priv->mutex, 0, 1);
  sem_init(&priv->wait, 0, 0);

  /* Allocate a watchdog timer */

  priv->timeout = wd_create();
  DEBUGASSERT(priv->timeout != 0);

  /* Attach Interrupt Handler */

  irq_attach(priv->irqid, lpc17_i2c_interrupt);

  /* Enable Interrupt Handler */

  up_enable_irq(priv->irqid);

  /* Install our operations */

  priv->dev.ops = &lpc17_i2c_ops;
  return &priv->dev;
}

/****************************************************************************
 * Name: up_i2cuninitalize
 *
 * Description:
 *   Uninitialise an I2C device
 *
 ****************************************************************************/

int up_i2cuninitialize(FAR struct i2c_master_s * dev)
{
  struct lpc17_i2cdev_s *priv = (struct lpc17_i2cdev_s *) dev;

  /* Disable I2C */

  putreg32(I2C_CONCLRT_I2ENC, priv->base + LPC17_I2C_CONCLR_OFFSET);

  /* Reset data structures */

  sem_destroy(&priv->mutex);
  sem_destroy(&priv->wait);

  /* Free the watchdog timer */

  wd_delete(priv->timeout);
  priv->timeout = NULL;

  /* Disable interrupts */

  up_disable_irq(priv->irqid);

  /* Detach Interrupt Handler */

  irq_detach(priv->irqid);
  return OK;
}

/****************************************************************************
 * Name: up_i2creset
 *
 * Description:
 *   Reset an I2C bus
 *
 ****************************************************************************/

#ifdef CONFIG_I2C_RESET
int up_i2creset(FAR struct i2c_master_s * dev)
{
  return OK;
}
#endif /* CONFIG_I2C_RESET */

#endif /* CONFIG_LPC17_I2C0 || CONFIG_LPC17_I2C1 || CONFIG_LPC17_I2C2 */
