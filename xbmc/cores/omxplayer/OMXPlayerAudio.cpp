/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
#include "system.h"
#endif

#include "OMXPlayerAudio.h"

#include <stdio.h>
#include <unistd.h>
#include <iomanip>

#include "FileItem.h"
#include "linux/XMemUtils.h"
#include "utils/BitstreamStats.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"

#include "DVDDemuxers/DVDDemuxUtils.h"
#include "utils/MathUtils.h"
#include "settings/AdvancedSettings.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "video/VideoReferenceClock.h"
#include "utils/TimeUtils.h"

#include "OMXPlayer.h"

#include <iostream>
#include <sstream>

class COMXMsgAudioCodecChange : public CDVDMsg
{
public:
  COMXMsgAudioCodecChange(const CDVDStreamInfo &hints, COMXAudioCodecOMX* codec)
    : CDVDMsg(GENERAL_STREAMCHANGE)
    , m_codec(codec)
    , m_hints(hints)
  {}
 ~COMXMsgAudioCodecChange()
  {
    delete m_codec;
  }
  COMXAudioCodecOMX   *m_codec;
  CDVDStreamInfo      m_hints;
};

OMXPlayerAudio::OMXPlayerAudio(OMXClock *av_clock,
                               CDVDMessageQueue& parent)
: CThread("COMXPlayerAudio")
, m_messageQueue("audio")
, m_messageParent(parent)
{
  m_av_clock      = av_clock;
  m_pChannelMap   = NULL;
  m_pAudioCodec   = NULL;
  m_speed         = DVD_PLAYSPEED_NORMAL;
  m_started       = false;
  m_stalled       = false;
  m_audioClock    = 0;
  m_buffer_empty  = false;
  m_nChannels     = 0;
  m_DecoderOpen   = false;
  m_freq          = CurrentHostFrequency();
  m_hints_current.Clear();

  m_av_clock->SetMasterClock(false);

  m_messageQueue.SetMaxDataSize(3 * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(8.0);
}


OMXPlayerAudio::~OMXPlayerAudio()
{
  CloseStream(false);

  m_DllBcmHost.Unload();
}

bool OMXPlayerAudio::OpenStream(CDVDStreamInfo &hints)
{
  /*
  if(IsRunning())
    CloseStream(false);
  */

  if(!m_DllBcmHost.Load())
    return false;

  COMXAudioCodecOMX *codec = new COMXAudioCodecOMX();

  if(!codec || !codec->Open(hints))
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    delete codec; codec = NULL;
    return false;
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(new COMXMsgAudioCodecChange(hints, codec), 0);
  else
  {
    if(!OpenStream(hints, codec))
      return false;
    CLog::Log(LOGNOTICE, "Creating audio thread");
    m_messageQueue.Init();
    Create();
  }

  /*
  if(!OpenStream(hints, codec))
    return false;

  CLog::Log(LOGNOTICE, "Creating audio thread");
  m_messageQueue.Init();
  Create();
  */

  return true;
}

bool OMXPlayerAudio::OpenStream(CDVDStreamInfo &hints, COMXAudioCodecOMX *codec)
{
  SAFE_DELETE(m_pAudioCodec);

  m_hints           = hints;
  m_pAudioCodec     = codec;

  if(m_hints.bitspersample == 0)
    m_hints.bitspersample = 16;

  m_speed           = DVD_PLAYSPEED_NORMAL;
  m_audioClock      = 0;
  m_error           = 0;
  m_errorbuff       = 0;
  m_errorcount      = 0;
  m_integral        = 0;
  m_skipdupcount    = 0;
  m_prevskipped     = false;
  m_syncclock       = true;
  m_passthrough     = IAudioRenderer::ENCODED_NONE;
  m_hw_decode       = false;
  m_errortime       = CurrentHostCounter();
  m_silence         = false;
  m_started         = false;
  m_flush           = false;
  m_nChannels       = 0;
  m_synctype        = SYNC_DISCON;
  m_stalled         = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;
  m_use_passthrough = (g_guiSettings.GetInt("audiooutput.mode") == IAudioRenderer::ENCODED_NONE) ? false : true ;
  m_use_hw_decode   = g_advancedSettings.m_omxHWAudioDecode;

  if(m_use_passthrough)
    m_device = g_guiSettings.GetString("audiooutput.passthroughdevice");
  else
    m_device = g_guiSettings.GetString("audiooutput.audiodevice");

  m_pChannelMap = m_pAudioCodec->GetChannelMap();

  return true /*OpenDecoder()*/;
}

bool OMXPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  // wait until buffers are empty
  if (bWaitForBuffers && m_speed > 0) m_messageQueue.WaitUntilEmpty();

  m_messageQueue.Abort();

  if(IsRunning())
    StopThread();

  m_messageQueue.End();

  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    delete m_pAudioCodec;
    m_pAudioCodec = NULL;
  }

  CloseDecoder();

  m_speed         = DVD_PLAYSPEED_NORMAL;
  m_started       = false;

  return true;
}

void OMXPlayerAudio::OnStartup()
{
}

void OMXPlayerAudio::OnExit()
{
  CLog::Log(LOGNOTICE, "thread end: OMXPlayerAudio::OnExit()");
}



void OMXPlayerAudio::HandleSyncError(double duration)
{
  double clock = m_av_clock->GetClock();
  double error = m_audioClock - clock;
  int64_t now;

  if( fabs(error) > DVD_MSEC_TO_TIME(100) || m_syncclock )
  {
    m_av_clock->Discontinuity(clock+error);
    /*
    if(m_speed == DVD_PLAYSPEED_NORMAL)
    CLog::Log(LOGDEBUG, "OMXPlayerAudio:: Discontinuity - was:%f, should be:%f, error:%f\n", clock, clock+error, error);
    */

    m_errorbuff = 0;
    m_errorcount = 0;
    m_skipdupcount = 0;
    m_error = 0;
    m_syncclock = false;
    m_errortime = m_av_clock->CurrentHostCounter();

    return;
  }

  if (m_speed != DVD_PLAYSPEED_NORMAL)
  {
    m_errorbuff = 0;
    m_errorcount = 0;
    m_integral = 0;
    m_skipdupcount = 0;
    m_error = 0;
    m_errortime = m_av_clock->CurrentHostCounter();
    return;
  }

  //check if measured error for 1 second
  now = m_av_clock->CurrentHostCounter();
  if ((now - m_errortime) >= m_freq)
  {
    m_errortime = now;
    m_error = m_errorbuff / m_errorcount;

    m_errorbuff = 0;
    m_errorcount = 0;

    if (m_synctype == SYNC_DISCON)
    {
      double limit, error;

      /*
      if (m_av_clock->GetRefreshRate(&limit) > 0)
      {
        //when the videoreferenceclock is running, the discontinuity limit is one vblank period
        limit *= DVD_TIME_BASE;

        //make error a multiple of limit, rounded towards zero,
        //so it won't interfere with the sync methods in CXBMCRenderManager::WaitPresentTime
        if (m_error > 0.0)
          error = limit * floor(m_error / limit);
        else
          error = limit * ceil(m_error / limit);
      }
      else
      {
        limit = DVD_MSEC_TO_TIME(10);
        error = m_error;
      }
      */

      limit = DVD_MSEC_TO_TIME(10);
      error = m_error;

      if (fabs(error) > limit - 0.001)
      {
        m_av_clock->Discontinuity(clock+error);
        /*
        if(m_speed == DVD_PLAYSPEED_NORMAL)
          CLog::Log(LOGDEBUG, "COMXPlayerAudio:: Discontinuity - was:%f, should be:%f, error:%f", clock, clock+error, error);
        */
      }
    }
    /*
    else if (m_synctype == SYNC_SKIPDUP && m_skipdupcount == 0 && fabs(m_error) > DVD_MSEC_TO_TIME(10))
    if (m_skipdupcount == 0 && fabs(m_error) > DVD_MSEC_TO_TIME(10))
    {
      //check how many packets to skip/duplicate
      m_skipdupcount = (int)(m_error / duration);
      //if less than one frame off, see if it's more than two thirds of a frame, so we can get better in sync
      if (m_skipdupcount == 0 && fabs(m_error) > duration / 3 * 2)
        m_skipdupcount = (int)(m_error / (duration / 3 * 2));

      if (m_skipdupcount > 0)
        CLog::Log(LOGDEBUG, "OMXPlayerAudio:: Duplicating %i packet(s) of %.2f ms duration",
                  m_skipdupcount, duration / DVD_TIME_BASE * 1000.0);
      else if (m_skipdupcount < 0)
        CLog::Log(LOGDEBUG, "OMXPlayerAudio:: Skipping %i packet(s) of %.2f ms duration ",
                  m_skipdupcount * -1,  duration / DVD_TIME_BASE * 1000.0);
    }
    */
  }
}

bool OMXPlayerAudio::CodecChange()
{
  unsigned int old_bitrate = m_hints.bitrate;
  unsigned int new_bitrate = m_hints_current.bitrate;

  /* only check bitrate changes on CODEC_ID_DTS, CODEC_ID_AC3, CODEC_ID_EAC3 */
  if(m_hints.codec != CODEC_ID_DTS && m_hints.codec != CODEC_ID_AC3 && m_hints.codec != CODEC_ID_EAC3)
    new_bitrate = old_bitrate = 0;
    
  if(m_hints_current.codec          != m_hints.codec ||
     m_hints_current.channels       != m_hints.channels ||
     m_hints_current.samplerate     != m_hints.samplerate ||
     m_hints_current.bitspersample  != m_hints.bitspersample ||
     old_bitrate                    != new_bitrate ||
     !m_DecoderOpen)
  {
    m_hints_current = m_hints;
    return true;
  }

  return false;
}

bool OMXPlayerAudio::Decode(DemuxPacket *pkt, bool bDropPacket)
{
  if(!pkt)
    return false;

  /* last decoder reinit went wrong */
  if(!m_pAudioCodec)
    return true;

  if(pkt->dts != DVD_NOPTS_VALUE)
    m_audioClock = pkt->dts;

  const uint8_t *data_dec = pkt->pData;
  int            data_len = pkt->iSize;

  if(!m_passthrough && !m_hw_decode)
  {
    while(!m_bStop && data_len > 0)
    {
      int len = m_pAudioCodec->Decode((BYTE *)data_dec, data_len);
      if( (len < 0) || (len >  data_len) )
      {
        m_pAudioCodec->Reset();
        break;
      }

      data_dec+= len;
      data_len -= len;

      uint8_t *decoded;
      int decoded_size = m_pAudioCodec->GetData(&decoded);

      if(decoded_size <=0)
        continue;

      int ret = 0;

      m_audioStats.AddSampleBytes(decoded_size);

      if(CodecChange())
      {
        CloseDecoder();

        m_DecoderOpen = OpenDecoder();
        if(!m_DecoderOpen)
          return false;
      }

      while(!m_bStop)
      {
        if(m_flush)
        {
          m_flush = false;
          break;
        }

        if((unsigned long)m_omxAudio.GetSpace() < pkt->iSize)
        {
          Sleep(10);
          continue;
        }
        
        if(!bDropPacket)
        {
          // Zero out the frame data if we are supposed to silence the audio
          if(m_silence)
            memset(decoded, 0x0, decoded_size);

          ret = m_omxAudio.AddPackets(decoded, decoded_size, m_audioClock, m_audioClock);

          if(ret != decoded_size)
          {
            CLog::Log(LOGERROR, "error ret %d decoded_size %d\n", ret, decoded_size);
          }
        }

        int n = (m_nChannels * m_hints.bitspersample * m_hints.samplerate)>>3;
        if (n > 0)
          m_audioClock += ((double)decoded_size * DVD_TIME_BASE) / n;

        if(m_speed == DVD_PLAYSPEED_NORMAL)
          HandleSyncError((((double)decoded_size * DVD_TIME_BASE) / n));
        break;

      }
    }
  }
  else
  {
    if(CodecChange())
    {
      CloseDecoder();

      m_DecoderOpen = OpenDecoder();
      if(!m_DecoderOpen)
        return false;
    }

    while(!m_bStop)
    {
      if(m_flush)
      {
        m_flush = false;
        break;
      }

      if((unsigned long)m_omxAudio.GetSpace() < pkt->iSize)
      {
        Sleep(10);
        continue;
      }
        
      if(!bDropPacket)
      {
        if(m_silence)
          memset(pkt->pData, 0x0, pkt->iSize);

        m_omxAudio.AddPackets(pkt->pData, pkt->iSize, m_audioClock, m_audioClock);
      }

      if(m_speed == DVD_PLAYSPEED_NORMAL)
        HandleSyncError(0);

      m_audioStats.AddSampleBytes(pkt->iSize);

      break;
    }
  }

  if(bDropPacket)
    m_stalled = false;

  if(m_omxAudio.GetDelay() < 0.1)
    m_stalled = true;

  // signal to our parent that we have initialized
  if(m_started == false)
  {
    m_started = true;
    m_messageParent.Put(new CDVDMsgInt(CDVDMsg::PLAYER_STARTED, DVDPLAYER_AUDIO));
  }

  if(!bDropPacket && m_speed == DVD_PLAYSPEED_NORMAL)
  {
    if(GetDelay() < 0.1f && !m_av_clock->OMXAudioBuffer())
    {
      clock_gettime(CLOCK_REALTIME, &m_starttime);
      m_av_clock->OMXAudioBufferStart();
    }
    else if(GetDelay() > (AUDIO_BUFFER_SECONDS * 0.75f) && m_av_clock->OMXAudioBuffer())
    {
      m_av_clock->OMXAudioBufferStop();
    }
    else if(m_av_clock->OMXAudioBuffer())
    {
      clock_gettime(CLOCK_REALTIME, &m_endtime);
      if((m_endtime.tv_sec - m_starttime.tv_sec) > 1)
      {
        m_av_clock->OMXAudioBufferStop();
      }
    }
  }

  return true;
}

void OMXPlayerAudio::Process()
{
  m_audioStats.Start();

  while(!m_bStop)
  {
    CDVDMsg* pMsg;
    int priority = (m_speed == DVD_PLAYSPEED_PAUSE && m_started) ? 1 : 0;
    int timeout = 1000;

    MsgQueueReturnCode ret = m_messageQueue.Get(&pMsg, timeout, priority);

    if (ret == MSGQ_TIMEOUT)
    {
      Sleep(10);
      continue;
    }

    if (MSGQ_IS_ERROR(ret) || ret == MSGQ_ABORT)
    {
      Sleep(10);
      continue;
    }

    if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacket();
      bool bPacketDrop     = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacketDrop();

      if(Decode(pPacket, m_speed > DVD_PLAYSPEED_NORMAL || m_speed < 0 || bPacketDrop))
      {
        if (m_stalled && (m_omxAudio.GetDelay() > (AUDIO_BUFFER_SECONDS * 0.75f)))
        {
          CLog::Log(LOGINFO, "COMXPlayerAudio - Switching to normal playback");
          m_stalled = false;
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      ((CDVDMsgGeneralSynchronize*)pMsg)->Wait( &m_bStop, SYNCSOURCE_AUDIO );
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      CDVDMsgGeneralResync* pMsgGeneralResync = (CDVDMsgGeneralResync*)pMsg;

      if (pMsgGeneralResync->m_timestamp != DVD_NOPTS_VALUE)
        m_audioClock = pMsgGeneralResync->m_timestamp;

      //m_ptsOutput.Add(m_audioClock, m_dvdAudio.GetDelay(), 0);
      if (pMsgGeneralResync->m_clock)
      {
        CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_RESYNC(%f, 1)", m_audioClock);
        m_av_clock->Discontinuity(m_audioClock + (GetDelay() * DVD_TIME_BASE));
        //m_av_clock->OMXUpdateClock(m_audioClock + (GetDelay() * DVD_TIME_BASE));
      }
      else
        CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_RESYNC(%f, 0)", m_audioClock);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      m_started = false;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_FLUSH");
      m_av_clock->Lock();
      m_av_clock->OMXStop(false);
      m_omxAudio.Flush();
      m_av_clock->OMXReset(false);
      m_av_clock->UnLock();
      m_syncclock = true;
      m_stalled   = true;
      m_started   = false;

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_STARTED))
    {
      if(m_started)
        m_messageParent.Put(new CDVDMsgInt(CDVDMsg::PLAYER_STARTED, DVDPLAYER_AUDIO));
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_EOF");
      WaitCompletion();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_DELAY))
    {
      if (m_speed != DVD_PLAYSPEED_PAUSE)
      {
        double timeout = static_cast<CDVDMsgDouble*>(pMsg)->m_value;

        CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::GENERAL_DELAY(%f)", timeout);

        timeout *= (double)DVD_PLAYSPEED_NORMAL / abs(m_speed);
        timeout += m_av_clock->GetAbsoluteClock();

        while(!m_bStop && m_av_clock->GetAbsoluteClock() < timeout)
          Sleep(1);
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::PLAYER_SETSPEED");
      m_speed = static_cast<CDVDMsgInt*>(pMsg)->m_value;
      if (m_speed != DVD_PLAYSPEED_NORMAL)
      {
        m_syncclock = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::AUDIO_SILENCE))
    {
      m_silence = static_cast<CDVDMsgBool*>(pMsg)->m_value;
      if (m_silence)
        CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::AUDIO_SILENCE(%f, 1)", m_audioClock);
      else
        CLog::Log(LOGDEBUG, "COMXPlayerAudio - CDVDMsg::AUDIO_SILENCE(%f, 0)", m_audioClock);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      COMXMsgAudioCodecChange* msg(static_cast<COMXMsgAudioCodecChange*>(pMsg));
      OpenStream(msg->m_hints, msg->m_codec);
      msg->m_codec = NULL;
    }

    pMsg->Release();
  }
}

void OMXPlayerAudio::Flush()
{
  m_flush = true;
  m_messageQueue.Flush();
  m_messageQueue.Put( new CDVDMsg(CDVDMsg::GENERAL_FLUSH), 1);
}

void OMXPlayerAudio::WaitForBuffers()
{
  // make sure there are no more packets available
  m_messageQueue.WaitUntilEmpty();

  // make sure almost all has been rendered
  // leave 500ms to avound buffer underruns
  double delay = GetCacheTime();
  if(delay > 0.5)
    Sleep((int)(1000 * (delay - 0.5)));
}

bool OMXPlayerAudio::Passthrough() const
{
  return m_passthrough;
}

IAudioRenderer::EEncoded OMXPlayerAudio::IsPassthrough(CDVDStreamInfo hints)
{
  int  m_outputmode = 0;
  bool bitstream = false;
  IAudioRenderer::EEncoded passthrough = IAudioRenderer::ENCODED_NONE;
  bool hdmi_audio = false;
  bool hdmi_passthrough_dts = false;
  bool hdmi_passthrough_ac3 = false;

  if (m_DllBcmHost.vc_tv_hdmi_audio_supported(EDID_AudioFormat_ePCM, 2, EDID_AudioSampleRate_e44KHz, EDID_AudioSampleSize_16bit ) == 0)
    hdmi_audio = true;
  if (m_DllBcmHost.vc_tv_hdmi_audio_supported(EDID_AudioFormat_eAC3, 2, EDID_AudioSampleRate_e44KHz, EDID_AudioSampleSize_16bit ) == 0)
    hdmi_passthrough_ac3 = true;
  if (m_DllBcmHost.vc_tv_hdmi_audio_supported(EDID_AudioFormat_eDTS, 2, EDID_AudioSampleRate_e44KHz, EDID_AudioSampleSize_16bit ) == 0)
    hdmi_passthrough_dts = true;
  printf("Audio support hdmi=%d, AC3=%d, DTS=%d\n", hdmi_audio, hdmi_passthrough_ac3, hdmi_passthrough_dts);


  m_outputmode = g_guiSettings.GetInt("audiooutput.mode");

  switch(m_outputmode)
  {
    case 0:
      passthrough = IAudioRenderer::ENCODED_NONE;
      break;
    case 1:
      bitstream = true;
      break;
    case 2:
      bitstream = true;
      break;
  }

  if(bitstream)
  {
    if(hints.codec == CODEC_ID_AC3 && g_guiSettings.GetBool("audiooutput.ac3passthrough"))
    {
      passthrough = IAudioRenderer::ENCODED_IEC61937_AC3;
    }
    if(hints.codec == CODEC_ID_DTS && g_guiSettings.GetBool("audiooutput.dtspassthrough"))
    {
      passthrough = IAudioRenderer::ENCODED_IEC61937_DTS;
    }
  }

  return passthrough;
}

bool OMXPlayerAudio::OpenDecoder()
{
  bool bAudioRenderOpen = false;

  m_nChannels = m_hints.channels;

  m_omxAudio.SetClock(m_av_clock);

  if(m_use_passthrough)
    m_passthrough = IsPassthrough(m_hints);

  if(!m_passthrough && m_use_hw_decode)
    m_hw_decode = COMXAudio::HWDecode(m_hints.codec);

  m_av_clock->Lock();
  m_av_clock->OMXStop(false);
  m_av_clock->HasAudio(false);
  if(m_passthrough || m_use_hw_decode)
  {
    if(m_passthrough)
      m_hw_decode = false;
    bAudioRenderOpen = m_omxAudio.Initialize(NULL, m_device.substr(4), m_pChannelMap,
                                             m_hints, m_av_clock, m_passthrough, m_hw_decode);
  }
  else
  {
    /* 6 channel have to be mapped to 8 for PCM */
    if(m_nChannels == 6)
      m_nChannels = 8;

    bAudioRenderOpen = m_omxAudio.Initialize(NULL, m_device.substr(4), m_nChannels, m_pChannelMap,
                                             m_hints.samplerate, m_hints.bitspersample, 
                                             false, false, m_passthrough);
  }

  m_codec_name = "";
  
  if(!bAudioRenderOpen)
  {
    CLog::Log(LOGERROR, "OMXPlayerAudio : Error open audio output");
    m_av_clock->HasAudio(false);
    m_av_clock->OMXReset(false);
    m_av_clock->UnLock();
    return false;
  }
  else
  {
    if(m_passthrough)
    {
      CLog::Log(LOGINFO, "Audio codec %s channels %d samplerate %d bitspersample %d\n",
        m_codec_name.c_str(), 2, m_hints.samplerate, m_hints.bitspersample);
    }
    else
    {
      CLog::Log(LOGINFO, "Audio codec %s channels %d samplerate %d bitspersample %d\n",
        m_codec_name.c_str(), m_nChannels, m_hints.samplerate, m_hints.bitspersample);
    }
  }

  m_av_clock->HasAudio(true);
  m_av_clock->OMXReset(false);
  m_av_clock->UnLock();
  return true;
}

void OMXPlayerAudio::CloseDecoder()
{
  m_av_clock->Lock();
  m_av_clock->OMXStop(false);
  m_omxAudio.Deinitialize();
  m_av_clock->HasAudio(false);
  m_av_clock->OMXReset(false);
  m_av_clock->UnLock();

  m_DecoderOpen = false;
}

double OMXPlayerAudio::GetDelay()
{
  return m_omxAudio.GetDelay();
}

double OMXPlayerAudio::GetCacheTime()
{
  return m_omxAudio.GetCacheTime();
}

void OMXPlayerAudio::WaitCompletion()
{
  m_omxAudio.WaitCompletion();
}

void OMXPlayerAudio::RegisterAudioCallback(IAudioCallback *pCallback)
{
  m_omxAudio.RegisterAudioCallback(pCallback);

}
void OMXPlayerAudio::UnRegisterAudioCallback()
{
  m_omxAudio.UnRegisterAudioCallback();
}

void OMXPlayerAudio::DoAudioWork()
{
  m_omxAudio.DoAudioWork();
}

void OMXPlayerAudio::SetCurrentVolume(long nVolume)
{
  m_omxAudio.SetCurrentVolume(nVolume);
}

void OMXPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put( new CDVDMsgInt(CDVDMsg::PLAYER_SETSPEED, speed), 1 );
  else
    m_speed = speed;
}

int OMXPlayerAudio::GetAudioBitrate()
{
  return (int)m_audioStats.GetBitrate();
}

std::string OMXPlayerAudio::GetPlayerInfo()
{
  std::ostringstream s;
  s << "aq:"     << setw(2) << min(99,m_messageQueue.GetLevel() + MathUtils::round_int(100.0/8.0*GetCacheTime())) << "%";
  s << ", kB/s:" << fixed << setprecision(2) << (double)GetAudioBitrate() / 1024.0;

  return s.str();
}
