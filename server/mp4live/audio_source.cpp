/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is MPEG4IP.
 * 
 * The Initial Developer of the Original Code is Cisco Systems Inc.
 * Portions created by Cisco Systems Inc. are
 * Copyright (C) Cisco Systems Inc. 2000, 2001.  All Rights Reserved.
 * 
 * Contributor(s): 
 *		Dave Mackie		dmackie@cisco.com
 *		Bill May 		wmay@cisco.com
 */

#include "mp4live.h"
#include "audio_source.h"

#ifdef ADD_LAME_ENCODER
#include "audio_lame.h"
#endif

#ifdef ADD_FAAC_ENCODER
#include "audio_faac.h"
#endif

int CAudioSource::ThreadMain(void) 
{
	while (true) {
		int rc;

		if (m_capture) {
			rc = SDL_SemTryWait(m_myMsgQueueSemaphore);
		} else {
			rc = SDL_SemWait(m_myMsgQueueSemaphore);
		}

		// semaphore error
		if (rc == -1) {
			break;
		} 

		// message pending
		if (rc == 0) {
			CMsg* pMsg = m_myMsgQueue.get_message();
		
			if (pMsg != NULL) {
				switch (pMsg->get_value()) {
				case MSG_NODE_STOP_THREAD:
					DoStopCapture();	// ensure things get cleaned up
					delete pMsg;
					return 0;
				case MSG_START_CAPTURE:
					DoStartCapture();
					break;
				case MSG_STOP_CAPTURE:
					DoStopCapture();
					break;
				}

				delete pMsg;
			}
		}

		if (m_capture) {
			ProcessAudio();
		}
	}

	return -1;
}

void CAudioSource::DoStartCapture()
{
	if (m_capture) {
		return;
	}

	if (!Init()) {
		return;
	}

	m_capture = true;
}

void CAudioSource::DoStopCapture()
{
	if (!m_capture) {
		return;
	}

	if (m_pConfig->m_audioEncode) {
		// flush remaining output from encoders
		// and forward it to sinks

		m_encoder->EncodeSamples(NULL, 0);

		ForwardEncodedFrames();
	}

	close(m_audioDevice);
	m_audioDevice = -1;

	m_encoder->Stop();
	delete m_encoder;
	m_encoder = NULL;

	free(m_rawFrameBuffer);
	m_rawFrameBuffer = NULL;

	m_capture = false;
}

bool CAudioSource::Init(void)
{
	m_startTimestamp = 0;
	m_frameNumber = 0;

	if (m_pConfig->m_audioEncode) {
		if (!InitEncoder()) {
			goto init_failure;
		}

		if (m_frameType == CMediaFrame::Mp3AudioFrame) {
			// TBD this may no longer be needed
			m_rawSamplesPerFrame = (u_int16_t)
				((((float)m_pConfig->GetIntegerValue(CONFIG_AUDIO_SAMPLE_RATE) 
				/ (float)m_pConfig->m_audioEncodedSampleRate)
				* m_pConfig->m_audioEncodedSamplesPerFrame) 
				+ 0.5);
		} else {
			m_rawSamplesPerFrame = m_pConfig->m_audioEncodedSamplesPerFrame;
		}

		m_encodedFrameDuration = 
			(m_pConfig->m_audioEncodedSamplesPerFrame * TimestampTicks) 
			/ m_pConfig->m_audioEncodedSampleRate;

	} else {
		m_rawSamplesPerFrame = 
			m_pConfig->GetIntegerValue(CONFIG_AUDIO_SAMPLE_RATE);
	}

	if (!InitDevice()) {
		return false;
	}

	m_rawFrameSize = m_rawSamplesPerFrame
		* m_pConfig->GetIntegerValue(CONFIG_AUDIO_CHANNELS) 
		* sizeof(u_int16_t);

	m_rawFrameBuffer = (u_int16_t*)malloc(m_rawFrameSize);

	if (!m_rawFrameBuffer) {
		goto init_failure;
	}

	m_rawFrameDuration = 
		(m_rawSamplesPerFrame * TimestampTicks) 
		/ m_pConfig->GetIntegerValue(CONFIG_AUDIO_SAMPLE_RATE); 

	// maximum number of passes in ProcessAudio, approx 1 sec.
	m_maxPasses = 
		m_pConfig->GetIntegerValue(CONFIG_AUDIO_SAMPLE_RATE) 
		/ m_rawSamplesPerFrame;

	return true;

init_failure:
	debug_message("audio initialization failed");

	free(m_rawFrameBuffer);
	m_rawFrameBuffer = NULL;

	delete m_encoder;
	m_encoder = NULL;

	close(m_audioDevice);
	m_audioDevice = -1;
	return false;
}

bool CAudioSource::InitDevice(void)
{
	int rc;
	char* deviceName = m_pConfig->GetStringValue(CONFIG_AUDIO_DEVICE_NAME);

	// open the audio device
	m_audioDevice = open(deviceName, O_RDONLY);
	if (m_audioDevice < 0) {
		error_message("Failed to open %s", deviceName);
		return false;
	}

	int format = AFMT_S16_LE;
	rc = ioctl(m_audioDevice, SNDCTL_DSP_SETFMT, &format);
	if (rc < 0 || format != AFMT_S16_LE) {
		error_message("Couldn't set format for %s", deviceName);
		return false;
	}

	u_int32_t channels = 
		m_pConfig->GetIntegerValue(CONFIG_AUDIO_CHANNELS);
	rc = ioctl(m_audioDevice, SNDCTL_DSP_CHANNELS, &channels);
	if (rc < 0 
	  || channels != m_pConfig->GetIntegerValue(CONFIG_AUDIO_CHANNELS)) {
		error_message("Couldn't set audio channels for %s", deviceName);
		return false;
	}

	u_int32_t samplingRate = 
		m_pConfig->GetIntegerValue(CONFIG_AUDIO_SAMPLE_RATE);
	u_int32_t targetSamplingRate = samplingRate;

	rc = ioctl(m_audioDevice, SNDCTL_DSP_SPEED, &samplingRate);

	if (rc < 0 || samplingRate != targetSamplingRate) {
		error_message("Couldn't set sampling rate for %s", deviceName);
		return false;
	}

	return true;
}

bool CAudioSource::InitEncoder()
{
	char* encoderName = 
		m_pConfig->GetStringValue(CONFIG_AUDIO_ENCODER);

	if (!strcasecmp(encoderName, AUDIO_ENCODER_FAAC)) {
#ifdef ADD_FAAC_ENCODER
		m_encoder = new CFaacAudioEncoder();
		m_frameType = CMediaFrame::AacAudioFrame;
#else
		error_message("faac encoder not available in this build");
		return false;
#endif
	} else if (!strcasecmp(encoderName, AUDIO_ENCODER_LAME)) {
#ifdef ADD_LAME_ENCODER
		m_encoder = new CLameAudioEncoder();
		m_frameType = CMediaFrame::Mp3AudioFrame;
#else
		error_message("lame encoder not available in this build");
		return false;
#endif
	} else {
		error_message("unknown encoder specified");
		return false;
	}

	return m_encoder->Init(m_pConfig);
}

void CAudioSource::ProcessAudio(void)
{
	// for efficiency, process 1 second before returning to check for commands
	for (int pass = 0; pass < m_maxPasses; pass++) {

		// get a new buffer, if we've handed off the old one
		// currently only happens when we're doing a raw record
		if (m_rawFrameBuffer == NULL) {
			m_rawFrameBuffer = (u_int16_t*)malloc(m_rawFrameSize);
			if (m_rawFrameBuffer == NULL) {
				debug_message("malloc error");
				break;
			}
		}

		// read a frame's worth of raw PCM data
		u_int32_t bytesRead = 
			read(m_audioDevice, m_rawFrameBuffer, m_rawFrameSize); 

		if (bytesRead <= 0) {
			continue;
		}

		if (m_startTimestamp == 0) {
			m_startTimestamp = GetTimestamp() - m_rawFrameDuration;
		}

		// encode audio frame to MP3
		if (m_pConfig->m_audioEncode) {
			m_encoder->EncodeSamples(m_rawFrameBuffer, bytesRead);

			ForwardEncodedFrames();
		}

		// if desired, forward raw audio to sinks
		if (m_pConfig->GetBoolValue(CONFIG_RECORD_RAW_AUDIO)) {
			CMediaFrame* pFrame =
				new CMediaFrame(CMediaFrame::PcmAudioFrame, 
					m_rawFrameBuffer, bytesRead,
					0, m_rawFrameDuration);
			ForwardFrame(pFrame);
			delete pFrame;

			// we'll get a new buffer on the next pass
			m_rawFrameBuffer = NULL;
		}
	}
}

u_int16_t CAudioSource::ForwardEncodedFrames(void)
{
	u_int8_t* pFrame;
	u_int32_t frameLength;
	u_int16_t numForwarded = 0;

	while (m_encoder->GetEncodedFrame(&pFrame, &frameLength)) {
		// sanity check
		if (pFrame == NULL || frameLength == 0) {
			break;
		}

		// forward the encoded frame to sinks
		Timestamp frameTimestamp = m_startTimestamp 
			+ (m_frameNumber * m_encodedFrameDuration);

		CMediaFrame* pMediaFrame =
			new CMediaFrame(m_frameType,
				pFrame, frameLength,
				frameTimestamp, m_encodedFrameDuration);
		ForwardFrame(pMediaFrame);
		delete pMediaFrame;

		m_frameNumber++;
		numForwarded++;
	}

	return numForwarded;
}

bool CAudioCapabilities::ProbeDevice()
{
	int rc;

	// open the audio device
	int audioDevice = open(m_deviceName, O_RDONLY);
	if (audioDevice < 0) {
		return false;
	}
	m_canOpen = true;

	// union of valid sampling rates for MP3 and AAC
	static const u_int32_t allSamplingRates[] = {
		7350, 8000, 11025, 12000, 16000, 22050, 
		24000, 32000, 44100, 48000, 64000, 88200, 96000
	};
	static const u_int8_t numAllSamplingRates =
		sizeof(allSamplingRates) / sizeof(u_int32_t);

	// for all possible sampling rates
	u_int8_t i;
	for (i = 0; i < numAllSamplingRates; i++) {
		u_int32_t targetRate = allSamplingRates[i];
		u_int32_t samplingRate = targetRate;

		// attempt to set sound card to this sampling rate
		rc = ioctl(audioDevice, SNDCTL_DSP_SPEED, &samplingRate);

		// invalid sampling rate, allow deviation of up to 1 sample/sec
		if (rc < 0 || abs(samplingRate - targetRate) > 1) {
			debug_message("audio device %s doesn't support sampling rate %u",
				m_deviceName, targetRate);
			continue;
		}

		// valid sampling rate
		m_samplingRates[m_numSamplingRates++] = samplingRate;
	}

	// zero out remaining sampling rate entries
	for (i = m_numSamplingRates; i < numAllSamplingRates; i++) {
		m_samplingRates[i] = 0;
	}

	close(audioDevice);

	return true;
}
