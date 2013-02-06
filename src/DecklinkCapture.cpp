/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
** 
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
** 
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include "../include/DecklinkCapture.h"

using namespace std;

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate(pthread_cond_t* m_sleepCond, IDeckLinkOutput* m_deckLinkOutput, IDeckLinkVideoConversion* m_deckLinkConverter)
 : m_refCount(0), g_timecodeFormat(0), frameCount(0)
{
	sleepCond = m_sleepCond;
	deckLinkOutput = m_deckLinkOutput;
	deckLinkConverter = m_deckLinkConverter;

	pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate()
{
	pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount++;
	pthread_mutex_unlock(&m_mutex);

	return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount--;
	pthread_mutex_unlock(&m_mutex);

	if (m_refCount == 0)
	{
		delete this;
		return 0;
	}

	return (ULONG)m_refCount;
}

tr1::shared_ptr<openshot::Frame> DeckLinkCaptureDelegate::GetFrame(int requested_frame)
{
	tr1::shared_ptr<openshot::Frame> f;

	#pragma omp critical (blackmagic_queue)
	{
		if (final_frames.size() > 0)
		{
			cout << "remaining: " << final_frames.size() << endl;
			f = final_frames.front();
			final_frames.pop_front();
		}
//		// Try for up to 1 second before giving up
//		for(int x = 0; x < 60; x++)
//		{
//			if (final_frames.size() > 0)
//			{
//				// Get the oldest frame
//				cout << "Found the frame! Remaining: " << final_frames.size() << endl;
//				f = final_frames.front();
//				break;
//			} else
//			{
//				// No frames yet... so sleep for a 1/60 second
//				cout << "sleeping for 1/60 a second" << endl;
//				usleep(20000);
//			}
//		}
	}

	return f;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
	// Handle Video Frame
	if(videoFrame)
	{	

		if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
		{
			fprintf(stderr, "Frame received (#%lu) - No input signal detected\n", frameCount);
		}
		else
		{
			const char *timecodeString = NULL;
			if (g_timecodeFormat != 0)
			{
				IDeckLinkTimecode *timecode;
				if (videoFrame->GetTimecode(g_timecodeFormat, &timecode) == S_OK)
				{
					timecode->GetString(&timecodeString);
				}
			}

			fprintf(stderr, "Frame received (#%lu) [%s] - Size: %li bytes\n",
				frameCount,
				timecodeString != NULL ? timecodeString : "No timecode",
				videoFrame->GetRowBytes() * videoFrame->GetHeight());

			if (timecodeString)
				free((void*)timecodeString);

			// Create a new copy of the YUV frame object
			IDeckLinkMutableVideoFrame *m_yuvFrame = NULL;

			int width = videoFrame->GetWidth();
			int height = videoFrame->GetHeight();

			HRESULT res = deckLinkOutput->CreateVideoFrame(
									width,
									height,
									videoFrame->GetRowBytes(),
									bmdFormat8BitYUV,
									bmdFrameFlagDefault,
									&m_yuvFrame);

			// Copy pixel and audio to copied frame
			void *frameBytesSource;
			void *frameBytesDest;
			videoFrame->GetBytes(&frameBytesSource);
			m_yuvFrame->GetBytes(&frameBytesDest);
			memcpy(frameBytesDest, frameBytesSource, videoFrame->GetRowBytes() * height);

			// Add raw YUV frame to queue
			raw_video_frames.push_back(m_yuvFrame);

			// Process frames once we have a few (to take advantage of multiple threads)
			if (raw_video_frames.size() >= omp_get_num_procs())
			{

//omp_set_num_threads(1);
omp_set_nested(true);
#pragma  omp parallel
{
#pragma  omp single
{
				// Loop through each queued image frame
				while (!raw_video_frames.empty())
				{
					// Get front frame (from the queue)
					IDeckLinkMutableVideoFrame* frame = raw_video_frames.front();

					// declare local variables (for OpenMP)
					IDeckLinkOutput *copy_deckLinkOutput(deckLinkOutput);
					IDeckLinkVideoConversion *copy_deckLinkConverter(deckLinkConverter);
					unsigned long copy_frameCount(frameCount);

					#pragma omp task firstprivate(copy_deckLinkOutput, copy_deckLinkConverter, frame, copy_frameCount)
					{
						// *********** CONVERT YUV source frame to RGB ************
						void *frameBytes;
						void *audioFrameBytes;

						// Create a new RGB frame object
						IDeckLinkMutableVideoFrame *m_rgbFrame = NULL;

						int width = videoFrame->GetWidth();
						int height = videoFrame->GetHeight();

						HRESULT res = copy_deckLinkOutput->CreateVideoFrame(
												width,
												height,
												width * 4,
												bmdFormat8BitARGB,
												bmdFrameFlagDefault,
												&m_rgbFrame);

						if(res != S_OK)
							cout << "BMDOutputDelegate::StartRunning: Error creating RGB frame, res:" << res << endl;

						// Create a RGB version of this YUV video frame
						copy_deckLinkConverter->ConvertFrame(frame, m_rgbFrame);

						// Get RGB Byte array
						m_rgbFrame->GetBytes(&frameBytes);

						// *********** CREATE OPENSHOT FRAME **********
						tr1::shared_ptr<openshot::Frame> f(new openshot::Frame(frameCount, width, height, "#000000", 2048, 2));

						// Add Image data to openshot frame
						f->AddImage(width, height, "ARGB", Magick::CharPixel, (uint8_t*)frameBytes);


						#pragma omp critical (blackmagic_queue)
						{
							// Add to final queue
							final_frames.push_back(f);

							// Don't keep too many frames (remove old frames)
							while (final_frames.size() > 20)
								// Remove oldest frame
								final_frames.pop_front();
						}


						// Remove background color
						//f->TransparentColors("#737e72", 20.0);

						// Display Image DEBUG
						//if (copy_frameCount > 300)
						//	#pragma omp critical (image_magick)
						//	f->Display();

						// Release RGB data
						if (m_rgbFrame)
							m_rgbFrame->Release();
						// Release RGB data
						if (frame)
							frame->Release();

					} // end task

					// Remove front item
					raw_video_frames.pop_front();
				} // end while
} // omp single
} // omp parallel

			}
		}

		// Increment frame count
		frameCount++;

		//if (g_maxFrames > 0 && frameCount >= g_maxFrames)
		//{
		//	pthread_cond_signal(sleepCond);
		//}
	}

    return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}



