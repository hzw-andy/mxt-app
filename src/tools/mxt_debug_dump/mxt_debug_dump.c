//------------------------------------------------------------------------------
/// \file   mxt_debug_dump.c
/// \brief  Debug dump tool for maxtouch chips
/// \author Atul Tiwari/Nick Dyer
//------------------------------------------------------------------------------
// Copyright 2012 Atmel Corporation. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
//    2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY ATMEL ''AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL ATMEL OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//------------------------------------------------------------------------------

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "libmaxtouch/libmaxtouch.h"
#include "libmaxtouch/info_block.h"
#include "libmaxtouch/log.h"

#define VERSION "1.0"

/* GEN_COMMANDPROCESSOR_T6 Register offsets from T6 base address */
#define MXT_CP_T6_RESET_OFFSET      0x00
#define MXT_CP_T6_BACKUPNV_OFFSET   0x01
#define MXT_CP_T6_CALIBRATE_OFFSET  0x02
#define MXT_CP_T6_REPORTALL_OFFSET  0x03
#define MXT_CP_T6_RESERVED_OFFSET   0x04
#define MXT_CP_T6_DIAGNOSTIC_OFFSET 0x05

/* T6 Debug Diagnostics Commands */
#define PAGE_UP           0x01
#define PAGE_DOWN         0x02
#define DELTAS_MODE       0x10
#define REFS_MODE         0x11

struct mxt_debug_data {
  int x_size;
  int y_size;

  int num_stripes;
  int stripe_width;
  int stripe_starty;
  int stripe_endy;
  uint8_t page_size;
  uint8_t mode;

  int diag_cmd_addr;
  int t37_addr;
  int t37_size;

  unsigned int frame;
  int stripe;
  int page;
  int x_ptr;
  int y_ptr;

  uint8_t *page_buf;
  uint16_t *data_buf;

  FILE *hawkeye;
};

static int get_objects_addr(struct mxt_debug_data *mxt_dd)
{
  int t6_addr;

  /* Obtain command processor's address */
  t6_addr = get_object_address(GEN_COMMANDPROCESSOR_T6, 0);
  if (t6_addr == OBJECT_NOT_FOUND) return -1;

  /* T37 commands address */
  mxt_dd->diag_cmd_addr = t6_addr + MXT_CP_T6_DIAGNOSTIC_OFFSET;

  /* Obtain Debug Diagnostic object's address */
  mxt_dd->t37_addr = get_object_address(DEBUG_DIAGNOSTIC_T37, 0);
  if (mxt_dd->t37_addr == OBJECT_NOT_FOUND) return -1;

  /* Obtain Debug Diagnostic object's size */
  mxt_dd->t37_size = get_object_size((uint8_t)DEBUG_DIAGNOSTIC_T37);
  if (mxt_dd->t37_size == OBJECT_NOT_FOUND) return -1;

  return 0;
}

static int mxt_debug_dump_page(struct mxt_debug_data *mxt_dd)
{
  int failures;
  int ret;
  uint8_t read_command = 1;
  uint8_t read_mode;
  uint8_t read_page;
  uint8_t page_up_cmd = PAGE_UP;

  if (mxt_dd->page == 0)
  {
    LOG(LOG_VERBOSE, "Writing mode command");
    mxt_write_register(&mxt_dd->mode, mxt_dd->diag_cmd_addr, 1);
  }
  else
  {
    mxt_write_register(&page_up_cmd, mxt_dd->diag_cmd_addr, 1);
  }

  /* Read back diagnostic register in T6 command processor until is has been
   * cleared. This means that the chip has actioned the command */
  failures = 0;

  while (read_command)
  {
    ret = mxt_read_register(&read_command, mxt_dd->diag_cmd_addr, 1);
    if (ret < 0)
    {
      LOG(LOG_ERROR, "Failed to read the status of diagnostic mode command");
      return -1;
    }

    if (read_command)
    {
      failures++;

      if (failures > 500)
      {
        LOG(LOG_ERROR, "Timeout waiting for command to be actioned");
        return -1;
      }
    }
  }

  ret = mxt_read_register(&read_mode, mxt_dd->t37_addr, 1);
  if (ret < 0)
  {
    LOG(LOG_ERROR, "Failed to read current mode");
    return -1;
  }

  ret = mxt_read_register(&read_page, mxt_dd->t37_addr + 1, 1);
  if (ret < 0)
  {
    LOG(LOG_ERROR, "Failed to read page number");
    return -1;
  }

  if ((read_mode != mxt_dd->mode) || (read_page != mxt_dd->page))
  {
    LOG(LOG_ERROR, "Bad page/mode in diagnostic data read");
    return -1;
  }

  ret = mxt_read_register(mxt_dd->page_buf,
                          mxt_dd->t37_addr + 2, mxt_dd->page_size);
  if (ret < 0)
  {
    LOG(LOG_ERROR, "Failed to read page");
    return -1;
  }

  return 0;
}

static int mxt_debug_insert_data(struct mxt_debug_data *mxt_dd)
{
  int i;
  uint16_t value;
  int ofs;

  for (i = 0; i < mxt_dd->page_size; i += 2)
  {
    if (mxt_dd->x_ptr > mxt_dd->x_size)
    {
      LOG(LOG_ERROR, "x pointer overrun");
      return -1;
    }

    value = (mxt_dd->page_buf[i+1] << 8) | mxt_dd->page_buf[i];

    ofs = mxt_dd->y_ptr + mxt_dd->x_ptr * mxt_dd->y_size;

    mxt_dd->data_buf[ofs] = value;

    mxt_dd->y_ptr++;

    if (mxt_dd->y_ptr > mxt_dd->stripe_endy)
    {
      mxt_dd->y_ptr = mxt_dd->stripe_starty;
      mxt_dd->x_ptr++;
    }
  }

  return 0;
}

#if 0
static int mxt_debug_print(struct mxt_debug_data *mxt_dd)
{
  int x;
  int y;
  int ofs;
  int16_t value;

  /* clear screen */
  printf("\e[1;1H\e[2J");

  for (x = 0; x < mxt_dd->x_size; x++)
  {
    for (y = 0; y < mxt_dd->y_size; y++)
    {
      ofs = y + x * mxt_dd->y_size;

      value = (int16_t)mxt_dd->data_buf[ofs];

      printf("%6d ", value);

    }
    printf("\n");
  }

  return 0;
}
#endif

static void mxt_hawkeye_generate_control_file(struct mxt_debug_data *mxt_dd)
{
  int x;
  int y;
  FILE *fp;

  fp = fopen("control.txt","w");
  if (!fp) {
    LOG(LOG_ERROR, "Failed to open file!");
    return;
  }

  fprintf(fp, "uint8,1,1,TIN\n");

  for (x = 0; x < mxt_dd->x_size; x++)
  {
    for (y = 0; y < mxt_dd->y_size; y++)
    {
      fprintf(fp, "int16_lsb_msb,%d,%d,X%dY%d_Delta16\n", y+1, x+1, x, y);
    }
  }

  fclose(fp);
}

static int mxt_hawkeye_output(struct mxt_debug_data *mxt_dd)
{
  int x;
  int y;
  int ofs;
  int16_t value;
  time_t t;
  struct tm *tmp;
  char timestr[20];

  /* print timestamp */
  t = time(NULL);
  tmp = localtime(&t);
  if (tmp == NULL)
  {
    return -1;
  }

  if (strftime(timestr, sizeof(timestr), "%T", tmp) == 0)
  {
    return -1;
  }

  fprintf(mxt_dd->hawkeye, "%s,", timestr);

  /* print frame number */
  fprintf(mxt_dd->hawkeye, "%d,", mxt_dd->frame);

  /* iterate through columns */
  for (x = 0; x < mxt_dd->x_size; x++)
  {
    for (y = 0; y < mxt_dd->y_size; y++)
    {
      ofs = y + x * mxt_dd->y_size;

      value = (int16_t)mxt_dd->data_buf[ofs];

      fprintf(mxt_dd->hawkeye, "%d,", value);
    }
  }
  fprintf(mxt_dd->hawkeye, "\n");

  return 0;
}

static unsigned int get_num_frames()
{
  unsigned int frames;

  printf("Number of frames: ");

  scanf("%u", &frames);

  return frames;
}

static int mxt_debug_dump(int mode)
{
  struct mxt_debug_data mxt_dd;
  int x_size, y_size;
  unsigned int frames = 0;
  int pages_per_stripe;
  int num_stripes;
  int ret;
  int page;
  time_t t1;
  time_t t2;

  frames = get_num_frames();

  printf("Reading %u frames\n", frames);

  t1 = time(NULL);

  x_size = info_block.id->matrix_x_size;
  y_size = info_block.id->matrix_y_size;

  mxt_dd.mode = mode;

  ret = get_objects_addr(&mxt_dd);
  if (ret) {
    LOG(LOG_ERROR, "Failed to get object information");
    return -1;
  }

  switch (info_block.id->family_id)
  {
    case 0x80:
      /* mXT224 */
      num_stripes = 1;
      pages_per_stripe = 4;
      mxt_dd.x_size = x_size;

    case 0xA0:
      /* mXT1386 (Galaxy Tab) */
      num_stripes = 3;
      pages_per_stripe = 8;
      mxt_dd.x_size = 27;
      break;

    case 0xA1:
      if(info_block.id->variant_id == 0x03)
      {
        /* mXT540E */
        num_stripes = 1;
        pages_per_stripe = 9;
        mxt_dd.x_size = x_size;
      }
      else
      {
        /* mXT768E */
        num_stripes = 1;
        pages_per_stripe = 12;
        mxt_dd.x_size = x_size;
      }
      break;

    case 0xA2:
      if(info_block.id->variant_id == 0x00)
      {
        /* mXT1664 */
        num_stripes = 1;
        pages_per_stripe = 30;
        mxt_dd.x_size = x_size;
      }
      else
      {
        LOG(LOG_ERROR, "Unrecognized variant ID");
      }

    default:
      LOG(LOG_ERROR, "Unrecognized family ID");
  }

  mxt_dd.page_size = mxt_dd.t37_size - 2;
  mxt_dd.num_stripes = num_stripes;
  mxt_dd.stripe_width = y_size / num_stripes;
  mxt_dd.y_size = y_size;

  LOG(LOG_INFO, "Number of stripes: %d", num_stripes);
  LOG(LOG_INFO, "Pages per stripe: %d", pages_per_stripe);
  LOG(LOG_INFO, "Stripe width: %d", mxt_dd.stripe_width);
  LOG(LOG_INFO, "X size: %d", mxt_dd.x_size);
  LOG(LOG_INFO, "Y size: %d", mxt_dd.y_size);

  /* allocate page/data buffers */
  mxt_dd.page_buf = (uint8_t *)malloc(sizeof(uint8_t) * mxt_dd.t37_size);
  if (!mxt_dd.page_buf) {
    LOG(LOG_ERROR, "malloc failure");
    return -1;
  }

  mxt_dd.data_buf = (uint16_t *)malloc(sizeof(uint16_t)
                                     * mxt_dd.x_size * mxt_dd.y_size);
  if (!mxt_dd.data_buf) {
    LOG(LOG_ERROR, "malloc failure");
    ret = -1;
    goto free_page_buf;
  }

  mxt_dd.hawkeye = fopen("hawkeye.csv","w");
  if (!mxt_dd.hawkeye) {
    printf("Failed to open file!\n");
    ret = -1;
    goto free_page_buf;
  }

  for (mxt_dd.frame = 1; mxt_dd.frame <= frames; mxt_dd.frame++)
  {
    /* iterate through stripes */
    for (mxt_dd.stripe = 0; mxt_dd.stripe < num_stripes; mxt_dd.stripe++)
    {
      /* Select stripe */
      mxt_dd.stripe_starty = mxt_dd.stripe_width * mxt_dd.stripe;
      mxt_dd.stripe_endy = mxt_dd.stripe_starty + mxt_dd.stripe_width - 1;
      mxt_dd.x_ptr = 0;
      mxt_dd.y_ptr = mxt_dd.stripe_starty;

      for (page = 0; page < pages_per_stripe; page++)
      {
        mxt_dd.page = pages_per_stripe * mxt_dd.stripe + page;

        LOG(LOG_INFO, "Stripe %d Page %d", mxt_dd.stripe, mxt_dd.page);

        ret = mxt_debug_dump_page(&mxt_dd);
        if (ret < 0)
        {
          LOG(LOG_ERROR, "Quitting...");
          goto free;
        }

        mxt_debug_insert_data(&mxt_dd);
      }
    }

    mxt_hawkeye_output(&mxt_dd);
  }

  fclose(mxt_dd.hawkeye);

  mxt_hawkeye_generate_control_file(&mxt_dd);

  t2 = time(NULL);
  printf("%d frames in %d seconds\n", frames, (int)(t2-t1));

  ret = 0;

free:
  free(mxt_dd.data_buf);
free_page_buf:
  free(mxt_dd.page_buf);

  return ret;
}

/*!
 * @brief
 * @return Zero.
 */
static int mxt_dd_cmd(char selection)
{
  int exit_loop = 0;

  switch (selection)
  {
    case 'd':
    case 'D':
      mxt_debug_dump(DELTAS_MODE);
      break;
    case 'r':
    case 'R':
      mxt_debug_dump(REFS_MODE);
      break;
    case 'c':
    case 'C':
      /* Get co-ordinates */
      break;
    case 'q':
    case 'Q':
      printf("Quitting the debug dump utility\n");
      exit_loop = 1;
      break;
    default:
      printf("Invalid menu option\n");
      break;
  }
  return exit_loop;
}

/*!
 * @brief  Menu function for the debug dump utility.
 * @return Zero.
 */
static int mxt_dd_menu(void)
{
  char menu_input;
  int exit_loop;

  printf("Debug data dump utility v. %s for Atmel maXTouch chips\n\n", VERSION);

  exit_loop = 0;

  while(!exit_loop)
  {
    printf("\nSelect one of the options:\n\n"
        "Enter D:   (D)elta dump\n"
        "Enter R:   (R)eference dump\n"
        "Enter C:   (C)oordinates dump\n"
        "Enter Q:   (Q)uit the application\n");

    scanf("%1s", &menu_input);

    exit_loop = mxt_dd_cmd(menu_input);
  }

  return 0;
}

/*!
 * @brief  Entry point for the debug dump utility.
 * @return Zero on success, negative for error.
 */
int main (int argc, char *argv[])
{
  int ret;

  /*! Find an mXT device and read the info block */
  ret = mxt_scan();
  if (ret == 0)
  {
    printf("Unable to find any maXTouch devices - exiting the application\n");
    return -1;
  }
  else if (ret < 0)
  {
    printf("Failed to init device - exiting the application\n");
    return -1;
  }

  if (mxt_get_info() < 0)
  {
    printf("Error reading info block, exiting...\n");
    return -1;
  }

  return mxt_dd_menu();
}