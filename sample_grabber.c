/*
 * Copyright (C) 2009 Micah Dowty
 * Copyright (C) 2010 Uwe Bonnes
 * Copyright (C) 2013 Swift Navigation Inc.
 *
 * Contacts: Fergus Noble <fergus@swift-nav.com>
 *           Colin Beighley <colin@swift-nav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 *
 *   "sample_grabber.c"
 *
 *   Purpose : Allows streaming of raw samples from MAX2769 RF Frontend for 
 *             post-processing and analysis. Samples are 3-bit and saved to 
 *             given file one sample per byte as signed integers.
 *
 *   Usage :   After running set_fifo_mode to set the FT232H on the Piksi to 
 *             fifo mode, use sample_grabber (see arguments below).
 *             End sample capture with ^C (CTRL+C). After finishing sample
 *             capture, run set_uart_mode to set the FT232H on the Piksi back
 *             to UART mode for normal operation.
 *
 *   Options : ./sample_grabber [-s number] [-v] [-h] [filename]
 *             [--size -s]     Number of samples to collect before exiting.
 *                             May be suffixed with a k (1e3) or an M (1e6).
 *                             If no argument is supplied, samples will be
 *                             collected until ^C (CTRL+C) is received.
 *             [--verbose -v]  Print more verbose output.
 *             [--help -h]     Print usage information and exit.
 *             [filename]      A filename to save samples to. If none is 
 *                             supplied then samples will not be saved.
 *
 *   Based on the example "stream_test.c" in libftdi, updated in 2010 by Uwe 
 *   Bonnes <bon@elektron.ikp.physik.tu-darmstadt.de> for libftdi and 
 *   originally written by Micah Dowty <micah@navi.cx> in 2009.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#include "ftdi.h"
#include "pipe/pipe.h"

/* Number of bytes to initially read out of device without saving to file */
#define NUM_FLUSH_BYTES 50000
/* Number of samples in each byte received (regardless of how they're packed)*/
#define SAMPLES_PER_BYTE 2
/* Number of bytes to read out of FIFO and write to disk at a time */
#define WRITE_SLICE_SIZE 50 
/* Maximum number of elements in pipe - 0 means size is unconstrained */
#define PIPE_SIZE 0

/* FPGA FIFO Error Flag is 0th bit, active low */
#define FPGA_FIFO_ERROR_CHECK(byte) (!(byte & 0x01))

static uint64_t total_unflushed_bytes = 0;
static long int bytes_wanted = 0; /* 0 means uninitialized, no limit */

static FILE *outputFile = NULL;

static int exitRequested = 0;

/* Pipe specific structs and pointers */
static pipe_t *sample_pipe;
static pthread_t file_writing_thread;
static pipe_producer_t* pipe_writer;
static pipe_consumer_t* pipe_reader;

static int verbose = 0;

static void sigintHandler(int signum)
{
  exitRequested = 1;
}

static void print_usage(void)
{
  printf(
  "Usage: ./sample_grabber [-s num] [-v] [-h] [filename]\n"
  "Options:\n"
  "  [--size -s]     Number of samples to collect before exiting. Number may\n"
  "                  be suffixed with a k (1e3) or an M (1e6). If no argument\n"
  "                  is supplied, samples will be collected until ^C (CTRL+C)\n"
  "                  is received.\n"
  "  [--verbose -v]  Print more verbose output.\n"
  "  [--help -h]     Print usage information and exit.\n"
  "  [filename]      A filename to save samples to. If none is supplied then\n"
  "                  samples will not be saved.\n"
  "Note : set_fifo_mode must be run before sample_grabber to configure the USB\n"
  "       hardware on the device for FIFO mode. Run set_uart_mode after\n"
  "       sample_grabber to set the device back to UART mode for normal\n"
  "       operation.\n"
  );
  exit(1);
}

/** Parse a string representing a number of samples to an integer.  String can
 * be a plain number or can include a unit suffix. This can be one of 'k' or
 * 'M' which multiply by 1e3 and 1e6 respectively.
 *
 * e.g. "5" -> 5
 *      "2k" -> 2000
 *      "3M" -> 3000000
 *
 * Returns -1 on an error condition.
 */
long int parse_size(char * s)
{
  long int val = -1;
  char last = s[strlen(s)-1];

  /* If the last character is a digit then just return the string as a
   * number.
   */
  if (isdigit(last)) {
    val = atol(s);
    if (val != 0)
      return val;
    else
      return -1;
  }

  /* Last char is a unit suffix, find the numeric part of the value. */
  /* Delete the suffix. */
  s[strlen(s)-1] = 0;
  /* Convert to a long int. */
  val = atol(s);
  if (val == 0)
    return -1;

  /* Multiply according to suffix and return value. */
  switch (last) {
    case 'k':
    case 'K':
      return val*1e3;
      break;

    case 'M':
      return val*1e6;
      break;

    default:
      return -1;
  }
}

static int readCallback(uint8_t *buffer, int length, FTDIProgressInfo *progress, void *userdata)
{
  /*
   * Keep track of number of bytes read - don't record samples until we have 
   * read a large number of bytes. This is to flush out the FIFO in the FPGA 
   * - it is necessary to do this to ensure we receive continuous samples.
   */
  static uint64_t total_num_bytes_received = 0;
  /* Array for extraction of samples from buffer */
  if (length){
    if (total_num_bytes_received >= NUM_FLUSH_BYTES){
      /* Save data to our pipe */
      if (outputFile) {
        /*
         * Convert samples from buffer from signmag to signed and write to 
         * the pipe. They will be read from the pipe and written to disk in a 
         * different thread.
         * Packing of each byte is
         *   [7:5] : Sample 0
         *   [4:2] : Sample 1
         *   [1] : Unused
         *   [0] : FPGA FIFO Error flag, active low. Usually indicates bytes
         *         are not being read out of FPGA FIFO quickly enough to avoid
         *         overflow
         */
        for (uint64_t ci = 0; ci < length; ci++){
          /* Check byte to see if a FIFO error occured */
          if (FPGA_FIFO_ERROR_CHECK(buffer[ci])) {
            printf("FPGA FIFO Error Flag at sample number %ld\n",
                   (long int)(total_unflushed_bytes+ci));
            exitRequested = 1;
          }
        }
        /* Push values into the pipe */
        pipe_push(pipe_writer,(void *)buffer,length);
      }
      total_unflushed_bytes += length;
    }
    total_num_bytes_received += length;
  }

  /* bytes_wanted = 0 means program was not run with a size argument */
  if (bytes_wanted != 0 && total_unflushed_bytes >= bytes_wanted){
    exitRequested = 1;
  }

  /* Print progress : time elapsed, bytes transferred, transfer rate */
  if (verbose) {
    if (progress){
      printf("%10.02fs total time %9.3f MiB captured %7.1f kB/s curr %7.1f kB/s total\n",
              progress->totalTime,
              progress->current.totalBytes / (1024.0 * 1024.0),
              progress->currentRate / 1024.0,
              progress->totalRate / 1024.0);
    }
  }

  return exitRequested ? 1 : 0;
}

static void* file_writer(void* pc_ptr){
  pipe_consumer_t* reader = pc_ptr;
  char buf[WRITE_SLICE_SIZE];
  size_t bytes_read;
  while (!(exitRequested)){
    bytes_read = pipe_pop(reader,buf,WRITE_SLICE_SIZE);
    if (bytes_read > 0){
      if (fwrite(buf,bytes_read,1,outputFile) != 1){
        fprintf(stderr,"Error in writing to file\n");
        exitRequested = 1;
      }
    }
  }
  return NULL;
}

int main(int argc, char **argv){
  struct ftdi_context *ftdi;
  int err;
  FILE *of = NULL;
  char const *outfile  = 0;
  outputFile =0;
  exitRequested = 0;
  char *descstring = NULL;

  static const struct option long_opts[] = {
    {"size",       required_argument, NULL, 's'},
    {"verbose",    no_argument,       NULL, 'v'},
    {"help",       no_argument,       NULL, 'h'},
    {NULL,         no_argument,       NULL, 0}
  };

  opterr = 0;
  int c;
  int option_index = 0;
  while ((c = getopt_long(argc, argv, "s:vh", long_opts, &option_index)) != -1)
    switch (c) {
      case 'v':
        verbose++;
        break;
      case 's': {
        long int samples_wanted = parse_size(optarg);
        if (samples_wanted <= 0) {
          fprintf(stderr, "Invalid size argument.\n");
          return EXIT_FAILURE;
        }
        bytes_wanted = samples_wanted / SAMPLES_PER_BYTE;
        if (bytes_wanted <= 0) {
          fprintf(stderr, "Invalid number of bytes to transfer.\n");
          return EXIT_FAILURE;
        }
        break;
      }
      case 'h':
        print_usage();
        return EXIT_SUCCESS;
      case '?':
        if (optopt == 's')
          fprintf(stderr, "Transfer size option requires an argument.\n");
        else
          fprintf(stderr, "Unknown option `-%c'.\n", optopt);
        return EXIT_FAILURE;
      default:
        abort();
     }
   
   if (optind < argc - 1){
     // Too many extra args
     print_usage();
   } else if (optind == argc - 1){
     // Exactly one extra argument- a dump file
     outfile = argv[optind];
   } else {
     if (verbose) {
       printf("No file name given, will not save samples to file\n");
     }
   }
   
   if ((ftdi = ftdi_new()) == 0){
     fprintf(stderr, "ftdi_new failed\n");
     return EXIT_FAILURE;
   }
   
   if (ftdi_set_interface(ftdi, INTERFACE_A) < 0){
     fprintf(stderr, "ftdi_set_interface failed\n");
     ftdi_free(ftdi);
     return EXIT_FAILURE;
   }
   
   if (ftdi_usb_open_desc(ftdi, 0x0403, 0x8398, descstring, NULL) < 0){
     fprintf(stderr,"Can't open ftdi device: %s\n",ftdi_get_error_string(ftdi));
     ftdi_free(ftdi);
     return EXIT_FAILURE;
   }
   
   /* A timeout value of 1 results in may skipped blocks */
   if(ftdi_set_latency_timer(ftdi, 2)){
     fprintf(stderr,"Can't set latency, Error %s\n",ftdi_get_error_string(ftdi));
     ftdi_usb_close(ftdi);
     ftdi_free(ftdi);
     return EXIT_FAILURE;
   }
   
   if (ftdi_usb_purge_rx_buffer(ftdi) < 0){
     fprintf(stderr,"Can't rx purge %s\n",ftdi_get_error_string(ftdi));
     return EXIT_FAILURE;
   }
   if (outfile)
     if ((of = fopen(outfile,"w+")) == 0)
       fprintf(stderr,"Can't open logfile %s, Error %s\n", outfile, strerror(errno));
   if (of)
     if (setvbuf(of, NULL, _IOFBF , 1<<16) == 0)
       outputFile = of;
   signal(SIGINT, sigintHandler);

   /* Only create pipe if we have a file to write samples to */
   if (outputFile) {
     sample_pipe = pipe_new(sizeof(char),PIPE_SIZE);
     pipe_writer = pipe_producer_new(sample_pipe);
     pipe_reader = pipe_consumer_new(sample_pipe);
     pipe_free(sample_pipe);
   }

   /* Start thread for writing samples to file */
   if (outputFile) {
     pthread_create(&file_writing_thread, NULL, &file_writer, pipe_reader);
   }
   
   /* Read samples from the Piksi. ftdi_readstream blocks until user hits ^C */
   err = ftdi_readstream(ftdi, readCallback, NULL, 8, 256);
   if (err < 0 && !exitRequested)
     exit(1);

   /* Close thread and free pipe pointers */
   if (outputFile) {
     pthread_join(file_writing_thread,NULL);
     pipe_producer_free(pipe_writer);
     pipe_consumer_free(pipe_reader);
   }
   
   /* Close file */
   if (outputFile) {
     fclose(outputFile);
     outputFile = NULL;
   }
   if (verbose) {
     printf("Capture ended.\n");
   }
   
   /* Clean up */
   if (ftdi_set_bitmode(ftdi, 0xff, BITMODE_RESET) < 0){
     fprintf(stderr,"Can't set synchronous fifo mode, Error %s\n",ftdi_get_error_string(ftdi));
     ftdi_usb_close(ftdi);
     ftdi_free(ftdi);
     return EXIT_FAILURE;
   }
   ftdi_usb_close(ftdi);
   ftdi_free(ftdi);
   signal(SIGINT, SIG_DFL);
   exit (0);
}
