#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <mad.h>
#include <alsa/asoundlib.h>

#define RAW_DATA_LEN 16384
snd_pcm_t* playback_handle;

void setalsavolume(long volume)
{
  long min, max;
  snd_mixer_t* handle;
  snd_mixer_selem_id_t* sid;
  const char* card = "default";
  const char* selem_name = "Master";

  snd_mixer_open(&handle, 0);
  snd_mixer_attach(handle, card);
  snd_mixer_selem_register(handle, NULL, NULL);
  snd_mixer_load(handle);

  snd_mixer_selem_id_alloca(&sid);
  snd_mixer_selem_id_set_index(sid, 0);
  snd_mixer_selem_id_set_name(sid, selem_name);
  snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

  snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
  snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);

  snd_mixer_close(handle);
}

int open_device(struct mad_header const *header)
{
   int err;
   char *pcm_name = "plughw:0,0";
   int rate = header->samplerate;
   int channels = 2;
   snd_pcm_hw_params_t* hw_params;

   if (header->mode == 0) {
      channels = 1;
   } else {
      channels = 2;
   }
 
   if ((err = snd_pcm_open(&playback_handle, pcm_name, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
      printf("cannot open audio device %s (%s)\n", pcm_name, snd_strerror (err));
      return -1;
   }
 
   if ((err = snd_pcm_hw_params_malloc(&hw_params)) < 0) {
      printf("cannot allocate hardware parameter structure (%s)\n",
      snd_strerror (err));
      return -1;
   }
 
   if ((err = snd_pcm_hw_params_any(playback_handle, hw_params)) < 0) {  // snd_pcm_hw_params_any, initlize the params
      printf("cannot initialize hardware parameter structure (%s)\n",
      snd_strerror (err));
      return -1;
   }
 
   // 以交错方式播放音频
   if ((err = snd_pcm_hw_params_set_access(playback_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
      printf("cannot set access type (%s)\n",
      snd_strerror (err));
      return -1;
   }
      
   if ((err = snd_pcm_hw_params_set_format(playback_handle, hw_params, SND_PCM_FORMAT_S32_LE)) < 0) {
      printf("cannot set sample format (%s)\n",
      snd_strerror (err));
      return -1;
   }
   
   if ((err = snd_pcm_hw_params_set_rate_near(playback_handle, hw_params, &rate, 0)) < 0) {
      printf("cannot set sample rate (%s)\n",
      snd_strerror (err));
      return -1;
   }
 
   if ((err = snd_pcm_hw_params_set_channels(playback_handle, hw_params, channels)) < 0) {
      printf("cannot set channel count (%s)\n",
      snd_strerror (err));
      return -1;
   }
 
   if ((err = snd_pcm_hw_params(playback_handle, hw_params)) < 0) {  // 使配置生效
      printf("cannot set parameters (%s)\n",
      snd_strerror (err));
      return -1;
   }
 
   snd_pcm_hw_params_free(hw_params);    // 释放之前请求的配置参数空间 snd_pcm_hw_params_malloc得到
   if ((err = snd_pcm_prepare (playback_handle)) < 0) {
      printf("cannot prepare audio interface for use (%s)\n",
      snd_strerror (err));
      return -1;
   }
   return 0;
}

/* static inline signed int scale(mad_fixed_t sample) */
/* { */
/*     /\* round *\/ */
/*     sample +=(1L <<(MAD_F_FRACBITS - 16)); */

/*     /\* clip *\/ */
/*     if(sample >= MAD_F_ONE) */
/*         sample = MAD_F_ONE - 1; */
/*     else if(sample < -MAD_F_ONE) */
/*         sample = -MAD_F_ONE; */

/*     /\* quantize *\/ */
/*     return sample >>(MAD_F_FRACBITS + 1 - 16); */
/* } */

int xrun_recovery(snd_pcm_t *handle, int err)
{
   if (err == -EPIPE) {    /* under-run */
      err = snd_pcm_prepare(handle);
 
   if (err < 0)
      printf("Can't recovery from underrun, prepare failed: %s\n",
         snd_strerror(err));
      return 0;
   } else if (err == -ESTRPIPE) {
      while ((err = snd_pcm_resume(handle)) == -EAGAIN)
         sleep(1);       /* wait until the suspend flag is released */
         if (err < 0) {
            err = snd_pcm_prepare(handle);
         if (err < 0)
            printf("Can't recovery from suspend, prepare failed: %s\n",
              snd_strerror(err));
      }
      return 0;
   }
   return err;
}

static int decode(const char* filename)
{
  int fd;
  struct stat stat;
  
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_synth synth;
  int nbytes;         // bytes readed from the file to the buffer to be decoded

  struct mad_pcm* pcm;
  unsigned int nchannels, nsamples;
  mad_fixed_t *left_ch, *right_ch;
  char* buf;           // Store the pcm data (sample data send to here)
  int sample;          // The final data to send to sound card
  int total = 0;

    
  if ((fd = open(filename, O_RDONLY)) < 0) {
    perror("Open File Error: ");
    return -1;
  }

  if (fstat(fd, &stat) < 0 || stat.st_size == 0) {
    perror("Stat Error or File Size Error: ");
    return -2;
  }

  mad_stream_init(&stream);
  mad_frame_init(&frame);
  mad_synth_init(&synth);
  
  char* data = (char *) malloc(RAW_DATA_LEN);
  char* outbuf = malloc(RAW_DATA_LEN * 4);    // 存放最终的写到声卡的数据
  
  if ((nbytes = read(fd, data, RAW_DATA_LEN)) < 0) {
    perror("Read Error: ");
    return -3;
  }
  
  mad_stream_buffer(&stream, data, nbytes);
  mad_frame_decode(&frame, &stream);

  if (stream.error == MAD_ERROR_LOSTSYNC) {
    const unsigned char* p = stream.this_frame;
    unsigned long n = 0;
    if (memcmp(p, "ID3", 3) == 0) {
      if (((p[6] | p[7] | p[8] | p[9]) & 0x80) == 0) {
	n = p[9] | p[8] << 7 | p[7] << 14 | p[6] << 21;
	mad_stream_skip(&stream, n + 10);
	printf("ID3, skip %d bytes \n", n + 10);
	mad_frame_decode(&frame, &stream);
      }
    }
  }
  
  open_device(&frame.header);               // Open the sound device file,已经得到一帧数据
  setalsavolume(85);
  
  while (1) {
    mad_synth_frame(&synth, &frame);        // 将一帧解码后的数据合成
    pcm = &synth.pcm;
    nchannels = pcm->channels;
    total = nsamples = pcm->length;
    left_ch = pcm->samples[0];
    right_ch = pcm->samples[1];
    char* buf = outbuf;
    int err;
    int sample;                                   // 合成后的数据需要整形以后存放在缓冲区里
    //    printf("nsamples is %d\n", nsamples);   // 一帧数据一般有1152个采样值，一般打印出1152
    while (nsamples--) {
      sample = *left_ch++;
      *(buf++) = sample & 0xFF;             // 对PCM数据整形，存放在buf中
      *(buf++) = (sample >> 8);
      *(buf++) = (sample >> 16);
      *(buf++) = (sample >> 24);
      if (nchannels == 2) {
	sample = *right_ch++;
	*(buf++) = sample & 0xFF;
	*(buf++) = (sample >> 8);
	*(buf++) = (sample >> 16);
	*(buf++) = (sample >> 24);      
      }
    }
    
    buf = outbuf;
    //    printf("decoded complete\n");     // for debug
    if ((err = snd_pcm_writei(playback_handle, buf, total)) < 0) {
      err = xrun_recovery(playback_handle, err);
      if (err < 0) {
	printf("Write error: %s\n", snd_strerror(err));
	exit(1);
      }
    }
    mad_frame_decode(&frame, &stream);         // 继续解码下一帧数据
    if (stream.error == MAD_ERROR_BUFLEN) {    // 不足一帧数据，需要读取新的数据并提供，上一次解码可以认为是失败的
      int undecode_data_size = stream.bufend - stream.next_frame;
      memcpy(data, data + nbytes - undecode_data_size, undecode_data_size);
      size_t breaded = read(fd, data + undecode_data_size, (nbytes - undecode_data_size));
      if (breaded <= 0)
	break;
      memset(data + undecode_data_size + breaded, 0, nbytes - undecode_data_size - breaded);
      int fillsize = breaded + undecode_data_size;
      mad_stream_buffer(&stream, data, fillsize);
      mad_frame_decode(&frame, &stream);       // 重新解码一帧数据
    }
  }
  mad_stream_finish(&stream);
  mad_frame_finish(&frame);
  mad_synth_finish(&synth);
  snd_pcm_close(playback_handle);
  free(data);
  free(outbuf);
  close(fd);
}

    

int main(int argc, char* argv[])
{
  printf("Usage mp3play mp3file\n");
  decode(argv[1]);
  return 0;
}
    
