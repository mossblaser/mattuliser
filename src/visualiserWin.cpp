/****************************************
 *
 * visualiserWin.cpp
 * Defines a window class for visualisers.
 *
 * This file is part of mattulizer.
 *
 * Copyright 2010 (c) Matthew Leach.
 *
 * Mattulizer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattulizer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Mattulizer.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "visualiserWin.h"
#include "visualiser.h"
#include "sdlexception.h"
#include "dspmanager.h"
#include "eventHandlers/eventhandler.h"
#include "eventHandlers/quitEvent.h"
#include "eventHandlers/keyQuit.h"
#include <SDL_timer.h>
#include <SDL_audio.h>
#include <iostream>

extern "C"{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#define CIRCBUFSIZE 5

visualiserWin::visualiserWin(int desiredFrameRate,
                             bool vsync,
                             int width,
                             int height,
                             Uint32 flags)
{
	// Set the local members
	this->desiredFrameRate = desiredFrameRate;
	this->shouldVsync = vsync;
	this->currentVis = NULL;
	this->shouldCloseWindow = false;
	this->width = width;
	this->height = height;

	// Set the OpenGL attributes
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	if(vsync)
		SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, 1);
	
	// Create the DSP manmager
	dspman = new DSPManager();
	
	// Create the window
	drawContext = SDL_SetVideoMode(width, height, 0, flags | SDL_OPENGL);
	if(drawContext == NULL)
		throw(SDLException());
	
	// also initialise the standard event handlers.
	initialiseStockEventHandlers();
}

visualiserWin::~visualiserWin()
{
	delete dspman;
	
	// delete all registered event handlers.
	for(std::set<eventHandler*>::iterator i = eventHandlers.begin();
	    i != eventHandlers.end(); i ++)
	{
		delete *i;
	}
	
	// Close the sound device
}

void visualiserWin::initialiseStockEventHandlers()
{
	quitEvent* quitevent = new quitEvent(this);
	keyQuit* keyquit = new keyQuit(this);
	registerEventHandler(quitevent);
	registerEventHandler(keyquit);
}

void visualiserWin::setVisualiser(visualiser* vis)
{
	currentVis = vis;
}

void visualiserWin::closeWindow()
{
	shouldCloseWindow = true;
}

void visualiserWin::registerEventHandler(eventHandler* eH)
{
	eventHandlers.insert(eH);
}

void visualiserWin::eventLoop()
{
	SDL_Event e;
	while(!shouldCloseWindow)
	{
		if(currentVis == NULL)
		{
			// wait for an event if we've not got a visualiser to show
			SDL_WaitEvent(&e);
			handleEvent(&e);
		}
		else
		{
			// handle events...
			if(SDL_PollEvent(&e) == 0)
				handleEvent(&e);
			
			// do some drawing
			Uint32 before = SDL_GetTicks();
			currentVis->draw();
			Uint32 after = SDL_GetTicks();
			
			SDL_GL_SwapBuffers();
			
			if(!shouldVsync)
			{
				// Calculate the time taken to do the drawing
				Uint32 timeTaken = after - before;
				
				int delayTime = ((Uint32)1000/desiredFrameRate) - timeTaken;
				
				if(delayTime > 0)
				{
					// Delay to maintain the framerate
					SDL_Delay((Uint32)delayTime);
				}
			}
		}
	}
}

static void* ffmpegWorkerEntry(void* args)
{
	ffmpegargst* arg = (ffmpegargst*)args;
	AVFormatContext* fmtCtx = (AVFormatContext*)arg->avformatcontext;
	int audioStream = arg->audiostream;
	packetQueue* queue = arg->queue;

	AVPacket packet;
	while(av_read_frame(fmtCtx, &packet) >= 0)
	{
		if(packet.stream_index == audioStream)
			queue->put(&packet);
		else
			av_free_packet(&packet);
	}
}

void visualiserWin::handleEvent(SDL_Event* e)
{
	for(std::set<eventHandler*>::iterator i = eventHandlers.begin();
	    i != eventHandlers.end(); i++)
	{
		eventHandler* eH = const_cast<eventHandler*>(*i);
		// call the handler if the type is the same
		if(eH->eventType() == e->type)
		eH->handleEvent(e);
	}
}

DSPManager* visualiserWin::getDSPManager() const
{
	return dspman;
}

int static decodeFrame(AVCodecContext* codecCtx, uint8_t* buffer,
                       int bufferSize, packetQueue* queue)
{
	static AVPacket* packet = NULL;
	static int packetSize = 0;
	static uint8_t* packetData = NULL;

	//Get a packet.
	if(packetSize == 0)
	{
		packet = queue->get();
		if(!packet)
			return -1;
		packetSize = packet->size;
		packetData = packet->data;
	}

	int dataSize = bufferSize;
	int framesRead = avcodec_decode_audio2(codecCtx, (int16_t*)buffer,
	                                       &dataSize, packetData, packetSize);

	if(framesRead < 0)
	{
		//Skip this frame if we have an error.
		packetSize = 0;
		return 0;
	}

	packetSize -= framesRead;
	packetData -= framesRead;

	return dataSize;
}

void static audioThreadEntryPoint(void* udata, uint8_t* stream, int len)
{
	sdlargst* args = (sdlargst*)udata;
	DSPManager* dspman = static_cast<DSPManager*>(args->dspman);
	AVCodecContext* codecCtx = (AVCodecContext*)args->avcodeccontext;
	packetQueue* queue = args->queue;

	static uint8_t buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int bufLength = 0;
	static unsigned int bufCurrentIndex = 0;
	uint8_t* streamIndex = stream;

	int samplesLeft = len;
	while(samplesLeft > 0)
	{
		if(bufCurrentIndex >= bufLength)
		{
			// No more data in the buffer, get some more.
			int decodeSize = decodeFrame(codecCtx, buf, sizeof(buf), queue);
			if(decodeSize < 0)
			{
				// something went wrong... silence.
				bufCurrentIndex = AVCODEC_MAX_AUDIO_FRAME_SIZE;
				memset(buf, 0, AVCODEC_MAX_AUDIO_FRAME_SIZE);
			}
			else
			{
			  bufLength = decodeSize;
			}
			//Reset the index for the new data.
			bufCurrentIndex = 0;
		}
		int numberOfSamples = bufLength - bufCurrentIndex;
		if(numberOfSamples > samplesLeft)
			numberOfSamples = samplesLeft;
		memcpy(streamIndex, (uint8_t*)buf + bufCurrentIndex, numberOfSamples);
		samplesLeft -= numberOfSamples;
		streamIndex += numberOfSamples;
		bufCurrentIndex += numberOfSamples;
	}

	dspman->processAudioPCM(NULL, stream, len);
	
	if(dspman->cbuf == NULL)
		dspman->cbuf = new circularBuffer::circularBuffer(CIRCBUFSIZE, sizeof(uint8_t) * len);
	memcpy(dspman->cbuf->add(), stream, sizeof(uint8_t) * len);
	memcpy(stream, dspman->cbuf->pop(), sizeof(uint8_t) * len);
}

bool visualiserWin::play(std::string &file)
{
	//Initalise ffmpeg.
	av_register_all();

	//Attempt to open the file.
	AVFormatContext* fmtCtx;
	if(av_open_input_file(&fmtCtx, file.c_str(), NULL, 0, NULL) != 0)
	{
		std::cerr << "Could not open file." << std::endl;
		return false;
	}

	if(av_find_stream_info(fmtCtx) < 0)
	{
		std::cerr << "Could not find stream information." << std::cerr;
		return false;
	}

	AVCodecContext* codecCtx;
	int audioStream = -1;
	for(int i = 0; i < fmtCtx->nb_streams; i++)
	{
		if(fmtCtx->streams[i]->codec->codec_type ==
		   CODEC_TYPE_AUDIO)
		{
			audioStream = i;
			break;
		}
	}

	if(audioStream == -1)
	{
		std::cerr << "Couldn't find audio stream." << std::endl;
		return false;
	}

	codecCtx = fmtCtx->streams[audioStream]->codec;

	AVCodec *codec;
	codec = avcodec_find_decoder(codecCtx->codec_id);
	if(!codec)
	{
		std::cerr << "Could not find codec!" << std::endl;
		return false;
	}
	avcodec_open(codecCtx, codec);

	SDL_AudioSpec wantedSpec;
	SDL_AudioSpec gotSpec;

	packetQueue* queue = new packetQueue;

	sdlargst* SDLArgs = new sdlargst;

	SDLArgs->avcodeccontext = codecCtx;
	SDLArgs->queue = queue;
	SDLArgs->dspman = dspman;

	wantedSpec.freq = codecCtx->sample_rate;
	wantedSpec.format = AUDIO_S16SYS;
	wantedSpec.channels = codecCtx->channels;
	wantedSpec.silence = 0;
	wantedSpec.samples = 1024;
	wantedSpec.callback = audioThreadEntryPoint;
	wantedSpec.userdata = (void*)SDLArgs;

	if(SDL_OpenAudio(&wantedSpec, &gotSpec) < 0)
	{
		throw(SDLException());
		return false;
	}

	SDL_PauseAudio(0);

	//Construct worker thread arguments.
	ffmpegargst* args = new ffmpegargst;
	args->audiostream = audioStream;
	args->avformatcontext = fmtCtx;
	args->queue = queue;

	//Begin ffmpeg worker thread.
	ffmpegworkerthread = new pthread_t;

	//Run the thread.
	pthread_create(ffmpegworkerthread, NULL, ffmpegWorkerEntry, args);

	// Also run the sound.
	return false;
}
