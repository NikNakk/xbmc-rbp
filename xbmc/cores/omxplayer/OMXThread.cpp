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

#include "OMXThread.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

OMXThread::OMXThread()
{
  pthread_mutex_init(&m_lock, NULL);
  pthread_attr_init(&m_tattr);
  m_stop      = false;
  m_running   = false;
}

OMXThread::~OMXThread()
{
  pthread_mutex_destroy(&m_lock);
  pthread_attr_destroy(&m_tattr);
}

bool OMXThread::Stop()
{
  if(!m_running)
    return false;

  m_stop = true;
  pthread_join(m_thread, NULL);

  return true;
}

bool OMXThread::Start()
{
  if(m_running)
    return false;

  m_stop    = false;
  m_running = true;

  pthread_create(&m_thread, &m_tattr, &OMXThread::Run, this);
  return true;
}

bool OMXThread::Running()
{
  return m_running;
}

void *OMXThread::Run(void *arg)
{
  OMXThread *thread = static_cast<OMXThread *>(arg);
  thread->Process();
  return NULL;
}

void OMXThread::Lock()
{
  if(!m_running)
    return;

  pthread_mutex_lock(&m_lock);
}

void OMXThread::UnLock()
{
  if(!m_running)
    return;

  pthread_mutex_unlock(&m_lock);
}

