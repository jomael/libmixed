/////
// Adapted from http://blogs.zynaptiq.com/bernsee/repo/smbPitchShift.cpp
//

/****************************************************************************
 *
 * COPYRIGHT 1999-2015 Stephan M. Bernsee <s.bernsee [AT] zynaptiq [DOT] com>
 *
 * 						The Wide Open License (WOL)
 *
 * Permission to use, copy, modify, distribute and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice and this license appear in all source copies. 
 * THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY OF
 * ANY KIND. See http://www.dspguru.com/wol.htm for more information.
 *
 *****************************************************************************/ 

#include "internal.h"

void fft(float *fftBuffer, long framesize, long sign){
  float wr, wi, arg, *p1, *p2, temp;
  float tr, ti, ur, ui, *p1r, *p1i, *p2r, *p2i;
  long i, bitm, j, le, le2, k;

  for (i = 2; i < 2*framesize-2; i += 2) {
    for (bitm = 2, j = 0; bitm < 2*framesize; bitm <<= 1) {
      if (i & bitm) j++;
      j <<= 1;
    }
    if (i < j) {
      p1 = fftBuffer+i; p2 = fftBuffer+j;
      temp = *p1; *(p1++) = *p2;
      *(p2++) = temp; temp = *p1;
      *p1 = *p2; *p2 = temp;
    }
  }
  for (k = 0, le = 2; k < (long)(log(framesize)/log(2.)+.5); k++) {
    le <<= 1;
    le2 = le>>1;
    ur = 1.0;
    ui = 0.0;
    arg = M_PI / (le2>>1);
    wr = cos(arg);
    wi = sign*sin(arg);
    for (j = 0; j < le2; j += 2) {
      p1r = fftBuffer+j; p1i = p1r+1;
      p2r = p1r+le2; p2i = p2r+1;
      for (i = j; i < 2*framesize; i += le) {
        tr = *p2r * ur - *p2i * ui;
        ti = *p2r * ui + *p2i * ur;
        *p2r = *p1r - tr; *p2i = *p1i - ti;
        *p1r += tr; *p1i += ti;
        p1r += le; p1i += le;
        p2r += le; p2i += le;
      }
      tr = ur*wr - ui*wi;
      ui = ur*wi + ui*wr;
      ur = tr;
    }
  }
}

void free_pitch_data(struct pitch_data *data){
  if(data->in_fifo)
    free(data->in_fifo);
  data->in_fifo = 0;

  if(data->out_fifo)
    free(data->out_fifo);
  data->out_fifo = 0;

  if(data->fft_workspace)
    free(data->fft_workspace);
  data->fft_workspace = 0;

  if(data->last_phase)
    free(data->last_phase);
  data->last_phase = 0;

  if(data->phase_sum)
    free(data->phase_sum);
  data->phase_sum = 0;

  if(data->output_accumulator)
    free(data->output_accumulator);
  data->output_accumulator = 0;

  if(data->analyzed_frequency)
    free(data->analyzed_frequency);
  data->analyzed_frequency = 0;

  if(data->analyzed_magnitude)
    free(data->analyzed_magnitude);
  data->analyzed_magnitude = 0;

  if(data->synthesized_frequency)
    free(data->synthesized_frequency);
  data->synthesized_frequency = 0;

  if(data->synthesized_magnitude)
    free(data->synthesized_magnitude);
  data->synthesized_magnitude = 0;
}

int make_pitch_data(size_t framesize, size_t oversampling, size_t samplerate, struct pitch_data *data){
  // FIXME: determine which of these can be static and which actually
  //        need to be retained for processing over contiguous buffers
  data->in_fifo = calloc(framesize, sizeof(float));
  data->out_fifo = calloc(framesize, sizeof(float));
  data->fft_workspace = calloc(framesize*2, sizeof(float));
  data->last_phase = calloc(framesize/2+1, sizeof(float));
  data->phase_sum = calloc(framesize/2+1, sizeof(float));
  data->output_accumulator = calloc(framesize*2, sizeof(float));
  data->analyzed_frequency = calloc(framesize, sizeof(float));
  data->analyzed_magnitude = calloc(framesize, sizeof(float));
  data->synthesized_frequency = calloc(framesize, sizeof(float));
  data->synthesized_magnitude = calloc(framesize, sizeof(float));

  if(!data->in_fifo ||
     !data->out_fifo ||
     !data->fft_workspace ||
     !data->last_phase ||
     !data->phase_sum ||
     !data->output_accumulator ||
     !data->analyzed_frequency ||
     !data->analyzed_magnitude ||
     !data->synthesized_frequency ||
     !data->synthesized_magnitude){
    mixed_err(MIXED_OUT_OF_MEMORY);
    free_pitch_data(data);
    return 0;
  }

  data->framesize = framesize;
  data->oversampling = oversampling;
  data->samplerate = samplerate;

  return 1;
}

void pitch_shift(float pitch, float *in, float *out, size_t samples, struct pitch_data *data){
  size_t framesize = data->framesize;
  size_t oversampling = data->oversampling;
  float *in_fifo = data->in_fifo;
  float *out_fifo = data->out_fifo;
  float *fft_workspace = data->fft_workspace;
  float *last_phase = data->last_phase;
  float *phase_sum = data->phase_sum;
  float *output_accumulator = data->output_accumulator;
  float *analyzed_frequency = data->analyzed_frequency;
  float *analyzed_magnitude = data->analyzed_magnitude;
  float *synthesized_frequency = data->synthesized_frequency;
  float *synthesized_magnitude = data->synthesized_magnitude;
  double magnitude, phase, tmp, window, real, imag;
  long i, k, qpd, index;
  
  /* set up some handy variables */
  long framesize2 = framesize/2;
  long step = framesize/oversampling;
  double bin_frequencies = (double)data->samplerate/(double)framesize;
  double expected = 2.*M_PI*(double)step/(double)framesize;
  long fifo_latency = framesize-step;
  if (data->overlap == 0) data->overlap = fifo_latency;

  /* main processing loop */
  for (i = 0; i < samples; i++){

    /* As long as we have not yet collected enough data just read in */
    in_fifo[data->overlap] = in[i];
    out[i] = out_fifo[data->overlap-fifo_latency];
    data->overlap++;

    /* now we have enough data for processing */
    if (data->overlap >= framesize) {
      data->overlap = fifo_latency;

      /* do windowing and re,im interleave */
      for (k = 0; k < framesize;k++) {
        window = -.5*cos(2.*M_PI*(double)k/(double)framesize)+.5;
        fft_workspace[2*k] = in_fifo[k] * window;
        fft_workspace[2*k+1] = 0.;
      }

      /* ***************** ANALYSIS ******************* */
      /* do transform */
      fft(fft_workspace, framesize, -1);

      /* this is the analysis step */
      for (k = 0; k <= framesize2; k++) {

        /* de-interlace FFT buffer */
        real = fft_workspace[2*k];
        imag = fft_workspace[2*k+1];

        /* compute magnitudeitude and phase */
        magnitude = 2.*sqrt(real*real + imag*imag);
        phase = atan2(imag,real);

        /* compute phase difference */
        tmp = phase - last_phase[k];
        last_phase[k] = phase;

        /* subtract expected phase difference */
        tmp -= (double)k*expected;

        /* map delta phase into +/- Pi interval */
        qpd = tmp/M_PI;
        if (qpd >= 0) qpd += qpd&1;
        else qpd -= qpd&1;
        tmp -= M_PI*(double)qpd;

        /* get deviation from bin frequency from the +/- Pi interval */
        tmp = oversampling*tmp/(2.*M_PI);

        /* compute the k-th partials' true frequency */
        tmp = (double)k*bin_frequencies + tmp*bin_frequencies;

        /* store magnitudeitude and true frequency in analysis arrays */
        analyzed_magnitude[k] = magnitude;
        analyzed_frequency[k] = tmp;

      }

      /* ***************** PROCESSING ******************* */
      /* this does the actual pitch shifting */
      memset(synthesized_magnitude, 0, framesize*sizeof(float));
      memset(synthesized_frequency, 0, framesize*sizeof(float));
      for (k = 0; k <= framesize2; k++) { 
        index = k*pitch;
        if (index <= framesize2) { 
          synthesized_magnitude[index] += analyzed_magnitude[k]; 
          synthesized_frequency[index] = analyzed_frequency[k] * pitch; 
        } 
      }
			
      /* ***************** SYNTHESIS ******************* */
      /* this is the synthesis step */
      for (k = 0; k <= framesize2; k++) {

        /* get magnitudeitude and true frequency from synthesis arrays */
        magnitude = synthesized_magnitude[k];
        tmp = synthesized_frequency[k];

        /* subtract bin mid frequency */
        tmp -= (double)k*bin_frequencies;

        /* get bin deviation from freq deviation */
        tmp /= bin_frequencies;

        /* take oversampling into account */
        tmp = 2.*M_PI*tmp/oversampling;

        /* add the overlap phase advance back in */
        tmp += (double)k*expected;

        /* accumulate delta phase to get bin phase */
        phase_sum[k] += tmp;
        phase = phase_sum[k];

        /* get real and imag part and re-interleave */
        fft_workspace[2*k] = magnitude*cos(phase);
        fft_workspace[2*k+1] = magnitude*sin(phase);
      } 

      /* zero negative frequencies */
      for (k = framesize+2; k < 2*framesize; k++) fft_workspace[k] = 0.;

      /* do inverse transform */
      fft(fft_workspace, framesize, 1);

      /* do windowing and add to output accumulator */ 
      for(k=0; k < framesize; k++) {
        window = -.5*cos(2.*M_PI*(double)k/(double)framesize)+.5;
        output_accumulator[k] += 2.*window*fft_workspace[2*k]/(framesize2*oversampling);
      }
      for (k = 0; k < step; k++) out_fifo[k] = output_accumulator[k];

      /* shift accumulator */
      memmove(output_accumulator, output_accumulator+step, framesize*sizeof(float));

      /* move input FIFO */
      for (k = 0; k < fifo_latency; k++) in_fifo[k] = in_fifo[k+step];
    }
  }
}
