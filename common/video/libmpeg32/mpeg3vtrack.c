#include "libmpeg3.h"
#include "mpeg3private.h"
#include "mpeg3protos.h"
#include "mpeg3vtrack.h"
#include "video/mpeg3video.h"
#include "video/mpeg3videoprotos.h"

#include <stdlib.h>

static
int mpeg3vtrack_get_frame (mpeg3_vtrack_t *track)

{
	u_int32_t code = 0;
	int count, done;
	mpeg3_demuxer_t *demux = track->demuxer;
	unsigned char *output;

	output = track->track_frame_buffer + track->track_frame_buffer_size;
	if (track->track_frame_buffer_size == 0) 
	  count = 0;
	else
	  count = 1;

	done = 0;
	while(done == 0 && 
		code != MPEG3_SEQUENCE_END_CODE &&
		!mpeg3demux_eof(demux))
	{
	  if (track->track_frame_buffer_size + 3 >= 
	      track->track_frame_buffer_maxsize) {
	    int diff = output - track->track_frame_buffer;
	    track->track_frame_buffer_maxsize += 4096;
	    track->track_frame_buffer = 
	      realloc(track->track_frame_buffer, 
		      track->track_frame_buffer_maxsize);
	    if (track->track_frame_buffer == NULL) exit(1);
	    output = track->track_frame_buffer + diff;
	  }
	  

	  code <<= 8;
	  // can't do this yet - we're still reading from the demuxer...
	  *output = mpeg3demux_read_char(track->demuxer);
	  code |= *output++;
	  track->track_frame_buffer_size++;
	  if (code == MPEG3_PICTURE_START_CODE) {
	    count++;
	    if (count == 2) done = 1;
	  }
	}
	return mpeg3demux_eof(demux);
}

static void mpeg3vtrack_cleanup_frame (mpeg3_vtrack_t *track)
{
  long size;

  if (track->track_frame_buffer == NULL) return;

  size = track->track_frame_buffer_size;
  track->track_frame_buffer[0] = track->track_frame_buffer[size - 4];
  track->track_frame_buffer[1] = track->track_frame_buffer[size - 3];
  track->track_frame_buffer[2] = track->track_frame_buffer[size - 2];
  track->track_frame_buffer[3] = track->track_frame_buffer[size - 1];
  track->track_frame_buffer_size = 4;
}


/* Line up on the beginning of the previous code. */
int mpeg3vtrack_prev_code(mpeg3_demuxer_t* demux, unsigned int code)
{
  unsigned int testcode = 0;

  if (mpeg3demux_bof(demux)) return mpeg3demux_bof(demux);

  do {
    testcode >>= 8;
    testcode |= (mpeg3demux_read_prev_char(demux) << 24);
  } while(!mpeg3demux_bof(demux) && testcode != code);

  return mpeg3demux_bof(demux);
}

// Must perform seeking now to get timecode for audio
int mpeg3vtrack_seek_percentage(mpeg3_vtrack_t *vtrack, double percentage)
{
  vtrack->percentage_seek = percentage;
  return 0;
}

int mpeg3vtrack_seek_frame(mpeg3_vtrack_t *vtrack, long frame)
{
  vtrack->frame_seek = frame;
  return 0;
}

int mpeg3vtrack_seek(mpeg3_vtrack_t *track)
{
  long this_gop_start;
  int result = 0;
  int back_step;
  int attempts;
  mpeg3_t *file = track->file;
  int is_video_stream = file->is_video_stream;
  mpeg3video_t *video = track->video;
  double percentage;
  long frame_number;
  int match_refframes = 1;
  mpeg3_demuxer_t *demux = track->demuxer;


  // Must do seeking here so files which don't use video don't seek.
  /* Seek to percentage */
  if(track->percentage_seek >= 0)
    {
      //printf("mpeg3video_seek 1 %f\n", video->percentage_seek);
      percentage = track->percentage_seek;
      track->percentage_seek = -1;
      mpeg3demux_seek_percentage(track->demuxer, percentage);

      // Go to previous I-frame #1

      // wmay - ASDF if we pull into vtrack
      if(track->video->has_gops)
	result = mpeg3vtrack_prev_code(demux, MPEG3_GOP_START_CODE);
      else
	result = mpeg3vtrack_prev_code(demux, MPEG3_SEQUENCE_START_CODE);
      //printf("mpeg3video_seek 3\n");

      /*
       * wmay - don't know if this is needed
       *if(!result) mpeg3bits_getbits_reverse(vstream, 32);
       */
      if (!result) {
	for (result = 0; result < 4; result++) 
	  mpeg3demux_read_prev_char(demux);
      }


      // Go to previous I-frame #2
      // wmay - ASDF if we pull into vtrack
      if(track->video->has_gops)
	result = mpeg3vtrack_prev_code(demux, MPEG3_GOP_START_CODE);
      else
	result = mpeg3vtrack_prev_code(demux, MPEG3_SEQUENCE_START_CODE);

      if (!result) {
	for (result = 0; result < 4; result++) 
	  mpeg3demux_read_prev_char(demux);
      }




      //mpeg3bits_start_forward(vstream);

      // Reread first two I frames
      if(mpeg3demux_tell_percentage(demux) <= 0) 
	{
	  long len;
	  mpeg3demux_seek_percentage(demux, 0);
	  mpeg3vtrack_get_frame(track);
	  // okay - here, do a raw read, then call get_firstframe
	  mpeg3video_get_firstframe(track->video, 
				    track->track_frame_buffer,
				    track->track_frame_buffer_size);
	  
	  mpeg3vtrack_cleanup_frame(track);
	  mpeg3vtrack_get_frame(track);
	  mpeg3video_process_frame(track->video, 
				   track->track_frame_buffer,
				   track->track_frame_buffer_size);
	  mpeg3vtrack_cleanup_frame(track);
	}
      //printf("mpeg3video_seek 4\n");



      // Read up to the correct percentage
      result = 0;
      while(!result && mpeg3demux_tell_percentage(demux) < percentage)
	{
	  result = mpeg3vtrack_get_frame(track);
	  if (!result) {
	    result = mpeg3video_process_frame(track->video, 
					      track->track_frame_buffer,
					      track->track_frame_buffer_size);
	    mpeg3vtrack_cleanup_frame(track);
	  }
	}
      //printf("mpeg3video_seek 5\n");
    }
#if 0
  else
    /* Seek to a frame */
    if(track->frame_seek >= 0)
      {
	frame_number = track->frame_seek;
	track->frame_seek = -1;
	if(frame_number < 0) frame_number = 0;
	if(frame_number > track->video->maxframe) 
	  frame_number = track->video->maxframe;

	//printf("mpeg3video_seek 1 %ld %ld\n", frame_number, video->framenum);

	/* Seek to I frame in table of contents */
	if(track->frame_offsets)
	  {
	    int i;
	    for(i = track->total_keyframe_numbers - 1; i >= 0; i--)
	      {
		if(track->keyframe_numbers[i] <= frame_number)
		  {
		    int frame;
		    int title_number;
		    int64_t byte;

		    // Go 2 I-frames before current position
		    if(i > 0) i--;

		    frame = track->keyframe_numbers[i];
		    title_number = (track->frame_offsets[frame] & 
				    0xff00000000000000) >> 56;
		    byte = track->frame_offsets[frame] & 
		      0xffffffffffffff;

		    video->framenum = track->keyframe_numbers[i];

		    mpeg3demux_open_title(demux, title_number);
		    mpeg3demux_seek_byte(demux, byte);

		    //printf("mpeg3video_seek 2 title_number=%d byte=%llx\n", title_number, byte);

		    // Get first 2 I-frames
		    if(byte == 0)
		      {
			mpeg3vtrack_get_frame(track);

			// okay - here, do a raw read, then call get_firstframe
			mpeg3video_get_firstframe(track->video, 
						  track->track_frame_buffer,
						  track->track_frame_buffer_size);
			mpeg3vtrack_cleanup_frame(track);
			mpeg3vtrack_get_frame(track);
			mpeg3video_process_frame(track->video, 
						 track->track_frame_buffer,
						 track->track_frame_buffer_size);
			mpeg3vtrack_cleanup_frame(track);
		      }
					
					
		    //printf("mpeg3video_seek 2 %ld %ld\n", frame_number, video->framenum);

		    mpeg3video_drop_frames(track, 
					   frame_number - 
					   track->video->framenum);
		    break;
		  }
	      }
	  }
	else
	  /* Seek to start of file */
	  if(frame_number < 16)
	    {
	      track->video->repeat_count = track->video->current_repeat = 0;
	      mpeg3demux_seek_start(vstream);
	      track->video->framenum = 0;
	      result = mpeg3vtrack_drop_frames(track, 
					       frame_number - video->framenum);
	    }
	  else
	    {
	      /* Seek to an I frame. */
	      if((frame_number < video->framenum || 
		  frame_number - video->framenum > MPEG3_SEEK_THRESHOLD))
		{
		  /* Elementary stream */
		  if(is_video_stream)
		    {
		      //mpeg3_t *file = video->file;
		      mpeg3_vtrack_t *track = video->track;
		      int64_t byte = (int64_t)((double)(mpeg3demuxer_total_bytes(vstream->demuxer) / 
							track->total_frames) * 
					       frame_number);
		      long minimum = 65535;
		      int done = 0;

		      //printf("seek elementary %d\n", frame_number);
		      /* Get GOP just before frame */
		      do
			{
			  result = mpeg3demux_seek_byte(demux, byte);

			  if(!result) 
			    result = mpeg3vtrack_prev_code(demux, MPEG3_GOP_START_CODE);
			  mpeg3demux_read_char(demux);
			  if(!result) result = mpeg3video_getgophdr(video);
			  this_gop_start = mpeg3video_goptimecode_to_frame(video);

			  //printf("wanted %ld guessed %ld byte %ld result %d\n", frame_number, this_gop_start, byte, result);
			  if(labs(this_gop_start - frame_number) >= labs(minimum)) 
			    done = 1;
			  else
			    {
			      minimum = this_gop_start - frame_number;
			      byte += (long)((float)(frame_number - this_gop_start) * 
					     (float)(mpeg3demuxer_total_bytes(vstream->demuxer) / 
						     track->total_frames));
			      if(byte < 0) byte = 0;
			    }
			}while(!result && !done);

		      //printf("wanted %d guessed %d\n", frame_number, this_gop_start);
		      if(!result)
			{
			  video->framenum = this_gop_start;
			  result = mpeg3video_drop_frames(video, frame_number - video->framenum);
			}
		    }
		  else




		    /* System stream */
		    {
		      mpeg3bits_seek_time(vstream, (double)frame_number / video->frame_rate);

		      percentage = mpeg3bits_tell_percentage(vstream);
		      //printf("seek frame %ld percentage %f byte %ld\n", frame_number, percentage, mpeg3bits_tell(vstream));
		      mpeg3bits_start_reverse(vstream);
		      mpeg3video_prev_code(vstream, MPEG3_GOP_START_CODE);
		      mpeg3bits_getbits_reverse(vstream, 32);
		      mpeg3bits_start_forward(vstream);
		      //printf("seek system 1 %f\n", (double)frame_number / video->frame_rate);

		      while(!result && mpeg3bits_tell_percentage(vstream) < percentage)
			{
			  result = mpeg3video_read_frame_backend(video, 0);
			  if(match_refframes)
			    mpeg3video_match_refframes(video);

			  //printf("seek system 2 %f %f\n", mpeg3bits_tell_percentage(vstream) / percentage);
			  match_refframes = 0;
			}
		      //printf("seek system 3 %f\n", (double)frame_number / video->frame_rate);
		    }

		  video->framenum = frame_number;
		}
	      else
		// Drop frames
		{
		  mpeg3video_drop_frames(video, frame_number - video->framenum);
		}
	    }
      }
#endif

  return result;
}

int mpeg3vtrack_previous_frame(mpeg3_vtrack_t *vtrack)
{
  int ix;
  if(mpeg3demux_tell_percentage(vtrack->demuxer) <= 0) return 1;

  // Get one picture
  mpeg3vtrack_prev_code(vtrack->demuxer, MPEG3_PICTURE_START_CODE);
  for (ix = 0; ix < 4; ix++) {
    mpeg3demux_read_prev_char(vtrack->demuxer);
  }

  if(mpeg3demux_bof(vtrack->demuxer)) 
    mpeg3demux_seek_percentage(vtrack->demuxer, 0);
  vtrack->video->repeat_count = 0;
  return 0;
}

#if 0
int mpeg3vtrack_drop_frames(mpeg3_vtrack_t *vtrack, long frames)
{
  int result = 0;
  long frame_number = video->framenum + frames;

  /* Read the selected number of frames and skip b-frames */
  while(!result && frame_number > video->framenum)
    {
      result = mpeg3video_read_frame_backend(video, frame_number - video->framenum);
    }
  return result;
}
#endif

static
mpeg3video_t* mpeg3vtrack_new_video (mpeg3_vtrack_t *track,
				     int have_mmx,
				     int is_video_stream,
				     int cpus)
{
  mpeg3video_t *video;
  mpeg3_demuxer_t *demux = track->demuxer;
  int result = 0;

  video = mpeg3video_new(have_mmx, 
			 is_video_stream,
			 cpus);

  mpeg3vtrack_get_frame(track);

  mpeg3bits_use_ptr_len(video->vstream, 
			track->track_frame_buffer, 
			track->track_frame_buffer_size);

  result = mpeg3video_get_header(video, 1);
  //printf("mpeg3video_new 2 %d\n", result);

  if(!result)
    {
      int hour, minute, second, frame;
      int gop_found;

      mpeg3video_initdecoder(video);
      video->decoder_initted = 1;
      track->width = video->horizontal_size;
      track->height = video->vertical_size;
      track->frame_rate = video->frame_rate;

      /* Try to get the length of the file from GOP's */
      if(!track->frame_offsets)
	{
	  if (is_video_stream)
	    {
#if 0
	      printf("In frame offset read\n");
	      /* Load the first GOP */
	      mpeg3demux_seek_start(demux);
	      result = mpeg3video_next_code(video->vstream, MPEG3_GOP_START_CODE);
	      if(!result) mpeg3bits_getbits(video->vstream, 32);
	      if(!result) result = mpeg3video_getgophdr(video);

	      hour = video->gop_timecode.hour;
	      minute = video->gop_timecode.minute;
	      second = video->gop_timecode.second;
	      frame = video->gop_timecode.frame;

	      video->first_frame = gop_to_frame(video, &video->gop_timecode);

	      /*
	       * 			video->first_frame = (long)(hour * 3600 * video->frame_rate + 
	       * 				minute * 60 * video->frame_rate +
	       * 				second * video->frame_rate +
	       * 				frame);
	       */

	      /* GOPs are supposed to have 16 frames */

	      video->frames_per_gop = 16;

	      /* Read the last GOP in the file by seeking backward. */
	      mpeg3bits_seek_end(video->vstream);
	      mpeg3bits_start_reverse(video->vstream);
	      result = mpeg3video_prev_code(video->vstream, MPEG3_GOP_START_CODE);
	      mpeg3bits_start_forward(video->vstream);
	      mpeg3bits_getbits(video->vstream, 8);
	      if(!result) result = mpeg3video_getgophdr(video);

	      hour = video->gop_timecode.hour;
	      minute = video->gop_timecode.minute;
	      second = video->gop_timecode.second;
	      frame = video->gop_timecode.frame;

	      video->last_frame = gop_to_frame(video, &video->gop_timecode);

	      /*
	       * 			video->last_frame = (long)((double)hour * 3600 * video->frame_rate + 
	       * 				minute * 60 * video->frame_rate +
	       * 				second * video->frame_rate +
	       * 				frame);
	       */

	      //printf("mpeg3video_new 3 %p\n", video);

	      /* Count number of frames to end */
	      while(!result)
		{
		  result = mpeg3video_next_code(video->vstream, MPEG3_PICTURE_START_CODE);
		  //printf("mpeg3video_new 2 %d %ld\n", result, video->last_frame);
		  if(!result)
		    {
		      mpeg3bits_getbyte_noptr(video->vstream);
		      video->last_frame++;
		    }
		}

	      track->total_frames = video->last_frame - video->first_frame + 1;
	      //printf("mpeg3video_new 3 %ld\n", track->total_frames);
	      mpeg3bits_seek_start(video->vstream);
#endif
	    }
	  else
	    // Try to get the length of the file from the multiplexing.
	    {
	      video->first_frame = 0;
	      track->total_frames = video->last_frame = 
		(long)(mpeg3demux_length(demux) * 
		       video->frame_rate);
	      video->first_frame = 0;
	    }
	}
      else
	// Get length from table of contents
	{
	  track->total_frames = track->total_frame_offsets;
	}



      video->maxframe = track->total_frames;
#if 0
      mpeg3bits_seek_start(video->vstream);
      mpeg3video_get_firstframe(video);
#else
      mpeg3video_get_firstframe(video,
				track->track_frame_buffer,
				track->track_frame_buffer_size);
#endif
    }
  else
    {
      //printf("mpeg3video_new 3 %p\n", video);
      mpeg3video_delete(video);
      video = 0;
    }
  //printf("mpeg3video_new 4 %p\n", video);

  return video;
}
/* ======================================================================= */
/*                                    ENTRY POINTS */
/* ======================================================================= */


mpeg3_vtrack_t* mpeg3_new_vtrack(mpeg3_t *file, 
	int stream_id, 
	mpeg3_demuxer_t *demuxer,
	int number)
{
	int result = 0;
	mpeg3_vtrack_t *new_vtrack;
	new_vtrack = calloc(1, sizeof(mpeg3_vtrack_t));
	new_vtrack->file = file;
	new_vtrack->demuxer = mpeg3_new_demuxer(file, 0, 1, stream_id);
	if(new_vtrack->demuxer) mpeg3demux_copy_titles(new_vtrack->demuxer, demuxer);
	new_vtrack->current_position = 0;
	new_vtrack->percentage_seek = -1;
	new_vtrack->frame_seek = -1;
	new_vtrack->track_frame_buffer = NULL;
	new_vtrack->track_frame_buffer_maxsize = 0;

//printf("mpeg3_new_vtrack 1\n");
// Copy pointers
	if(file->frame_offsets)
	{
		new_vtrack->frame_offsets = file->frame_offsets[number];
		new_vtrack->total_frame_offsets = file->total_frame_offsets[number];
		new_vtrack->keyframe_numbers = file->keyframe_numbers[number];
		new_vtrack->total_keyframe_numbers = file->total_keyframe_numbers[number];
	}

//printf("mpeg3_new_vtrack 1\n");
//printf("mpeg3_new_vtrack %llx\n", mpeg3demux_tell(new_vtrack->demuxer));
/* Get information about the track here. */
	new_vtrack->video = mpeg3vtrack_new_video(new_vtrack,
						 file->have_mmx,
						 file->is_video_stream,
						 file->cpus);
	if(!new_vtrack->video)
	{
/* Failed */
		mpeg3_delete_vtrack(file, new_vtrack);
		new_vtrack = 0;
	}

//printf("mpeg3_new_vtrack 2\n");
	return new_vtrack;
}

int mpeg3_delete_vtrack(mpeg3_t *file, mpeg3_vtrack_t *vtrack)
{
	if(vtrack->video) mpeg3video_delete(vtrack->video);
	if(vtrack->demuxer) mpeg3_delete_demuxer(vtrack->demuxer);
	free(vtrack);
	return 0;
}

/* Read all the way up to and including the next picture start code */
int mpeg3vtrack_read_raw (mpeg3_vtrack_t *vtrack, 
			  unsigned char *output, 
			  long *size, 
			  long max_size)
{
  u_int32_t code = 0;

  mpeg3_demuxer_t *demux = vtrack->demuxer;
  *size = 0;
  while(code != MPEG3_PICTURE_START_CODE && 
	code != MPEG3_SEQUENCE_END_CODE &&
	*size < max_size && 
	!mpeg3demux_eof(demux))
    {
      code <<= 8;
      *output = mpeg3demux_read_char(demux);
      code |= *output++;
      (*size)++;
    }
  return mpeg3demux_eof(demux);
}

int mpeg3vtrack_read_frame(mpeg3_vtrack_t *track, 
			   long frame_number, 
			   unsigned char **output_rows,
			   int in_x, 
			   int in_y, 
			   int in_w, 
			   int in_h, 
			   int out_w, 
			   int out_h, 
			   int color_model)
{
	int result = 0;

	if(!result) result = mpeg3vtrack_seek(track);

	if (!result) result = mpeg3vtrack_get_frame(track);

//printf("mpeg3video_read_frame 4\n");
	if (!result) {
	  result = mpeg3video_read_frame(track->video,
					 track->track_frame_buffer,
					 track->track_frame_buffer_size,
					 output_rows,
					 in_x, 
					 in_y, 
					 in_w, 
					 in_h, 
					 out_w, 
					 out_h, 
					 color_model);

	  mpeg3vtrack_cleanup_frame(track);
	}
//printf("mpeg3video_read_frame 5\n");

	return result;
}

int mpeg3vtrack_read_yuvframe(mpeg3_vtrack_t *vtrack, 
			      char *y_output,
			      char *u_output,
			      char *v_output,
			      int in_x,
			      int in_y,
			      int in_w,
			      int in_h)
{
	int result = 0;

	if(!result) result = mpeg3vtrack_seek(vtrack);

	if (!result) result = mpeg3vtrack_get_frame(vtrack);


	if(!result) {
	  result = mpeg3video_read_yuvframe(vtrack->video, 
					    vtrack->track_frame_buffer,
					    vtrack->track_frame_buffer_size,
					    y_output, 
					    u_output,
					    v_output,
					    in_x,
					    in_y,
					    in_w,
					    in_h);
	  mpeg3vtrack_cleanup_frame(vtrack);
	}
	return result;
}

int mpeg3vtrack_read_yuvframe_ptr(mpeg3_vtrack_t *vtrack, 
				  char **y_output,
				  char **u_output,
				  char **v_output)
{
	int result = 0;

	if(!result) result = mpeg3vtrack_seek(vtrack);

	if (!result) result = mpeg3vtrack_get_frame(vtrack);


	if(!result) {
	  result = mpeg3video_read_yuvframe_ptr(vtrack->video, 
						vtrack->track_frame_buffer,
						vtrack->track_frame_buffer_size,
						y_output,
						u_output,
						v_output);
	  mpeg3vtrack_cleanup_frame(vtrack);
	}
	return result;
}