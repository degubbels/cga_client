#include "winsock.h"
#include <stdio.h>
#include <string>
#include <iostream>
#include <map>

// For decoding
extern "C" {
	#include "libavcodec/avcodec.h"
	#include "libavutil/frame.h""
	#include "libswscale/swscale.h"
}

#include <SDL_main.h>
#include <SDL.h>

const int PACKET_SIZE = 1400;

// Packet definition
struct UDPHeader {
	uint32_t nframe;
	uint32_t nfrag;

	uint32_t nfrags;
	uint32_t framesize;
};

struct UDPPacket {
	UDPHeader header;
	char packet[PACKET_SIZE] = {0};
};

// Reassembled frames
std::map<int, char*> frameMap;
std::map<int, int> fragsReceived;
std::map<int, int> frameSize;

// Static frame information
const int FRAME_WIDTH = 1280;
const int FRAME_HEIGHT = 720;
const AVPixelFormat FRAME_FORMAT = AV_PIX_FMT_YUV420P;

const AVCodecID CODEC_ID = AV_CODEC_ID_MPEG4;

AVCodec* codec;
AVCodecContext* codecContext;

// Socket
u_long UDP_ADDRESS = INADDR_ANY;
u_short UDP_PORT = 8888;

sockaddr_in UDPAddress;
SOCKET UDPSocket;

byte UDPBuffer[PACKET_SIZE + sizeof(UDPHeader)];
int UDP_ADDRESS_LENGTH = sizeof(UDPAddress);


// SDL
SDL_Texture* sdlTexture;
SDL_Window* sdlWindow;
SDL_Renderer* sdlRenderer;


// Partly from: https://lazyfoo.net/tutorials/SDL/07_texture_loading_and_rendering/index.php


// Initialise decoder
void initDecoder() {

	// Note: av_codec_register all is no longer necessary and deprecated in ffmpeg >4.0

	// Get codec
	codec = avcodec_find_decoder(CODEC_ID);
	if (!codec) {
		printf("Codec not found.. %s\n", stderr);
		exit(EXIT_FAILURE);
	}

	// Get codec context
	codecContext = avcodec_alloc_context3(codec);
	codecContext->width = FRAME_WIDTH;
	codecContext->height = FRAME_HEIGHT;

	// Open codec
	int ret = avcodec_open2(codecContext, codec, NULL);
	if (ret < 0) {
		printf("could not open codec.. %s\n", stderr);
		exit(EXIT_FAILURE);
	}
}

// Initialise SDL renderer
void initRenderer() {

	// Init sdl library
	SDL_Init(SDL_INIT_EVERYTHING);


	//Create window
	sdlWindow = SDL_CreateWindow("CGA Client", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, FRAME_WIDTH, FRAME_HEIGHT, SDL_WINDOW_SHOWN);
	if (sdlWindow == NULL) {
		printf("Window could not be created! SDL Error: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	// Create renderer for window
	sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
	if (sdlRenderer == NULL) {
		printf("Renderer could not be created! SDL Error: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	// Init texture to white screen
	SDL_SetRenderDrawColor(sdlRenderer, 0xFF, 0xFF, 0xFF, 0xFF);
	sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, FRAME_WIDTH, FRAME_HEIGHT);
}

// Initialise socket
void initSocket() {

	// Initialise winsock

	WSADATA wsaData;
	int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (ret != 0) {
		printf("WSA init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Initialize the socket
	UDPSocket = socket(PF_INET, SOCK_DGRAM, 0);
	if (UDPSocket < 0) {
		printf("Socket init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Configure address
	UDPAddress.sin_family = AF_INET;
	UDPAddress.sin_addr.s_addr = UDP_ADDRESS;
	UDPAddress.sin_port = htons(UDP_PORT);

	// Bind socket
	ret = bind(UDPSocket, (const sockaddr*)&UDPAddress, sizeof(UDPAddress));
	if (ret != 0) {
		printf("Socket bind failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}
}

// Close SDL renderer
void closeRenderer() {

	SDL_DestroyTexture(sdlTexture);
	SDL_DestroyRenderer(sdlRenderer);
	SDL_DestroyWindow(sdlWindow);
	sdlTexture = NULL;
	sdlWindow = NULL;
	sdlRenderer = NULL;

	//Quit SDL subsystems
	SDL_Quit();
}

// Decode a frame
// Param frameIndex; the index in the frameMap of the frame to render
AVFrame* decodeFrame(int frameIndex) {

	// Allocate packet
	AVPacket* packet = av_packet_alloc();
	if (!packet) {
		printf("Packet alloc failed..\n");
		exit(EXIT_FAILURE);
	}

	// Fill packet with frame data
	packet->data = (uint8_t*)frameMap[frameIndex];
	packet->size = frameSize[frameIndex];

	// Allocate frame
	AVFrame* frame = av_frame_alloc();
	if (!frame) {
		printf("Frame alloc failed..\n");
		exit(EXIT_FAILURE);
	}

	frame->width = FRAME_WIDTH;
	frame->height = FRAME_HEIGHT;
	frame->format = AV_PIX_FMT_YUV420P;

	// Send packet to decoder
	int ret = avcodec_send_packet(codecContext, packet);
	if (ret < 0) {
		fprintf(stderr, "Error sending a packet for decoding: %d\n", ret);
		exit(EXIT_FAILURE);
	}

	// Get frame from decoder
	ret = avcodec_receive_frame(codecContext, frame);
	if (ret == AVERROR(EAGAIN)) {
		printf("Error during decoding: partial frame\n");
	} else if (ret == AVERROR_EOF) {
		printf("Error during decoding: Error during decoding\n");
	} else if (ret < 0) {
		fprintf(stderr, "Unknown error during decoding\n");
	}

	return frame;
}



/*
	Wait until the next packet arrives
	Blocks execution
*/
UDPPacket nextPacket() {

	// Receive next udp packet
	int ret = recvfrom(
		UDPSocket,
		(char*)UDPBuffer,
		PACKET_SIZE + sizeof(UDPHeader),
		0,
		(sockaddr*)&UDPAddress,
		&UDP_ADDRESS_LENGTH
		);

	// Check for error
	if (ret < 0) {
		printf("Receive failure: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Interpret as packet struct
	return *(reinterpret_cast<UDPPacket*>(UDPBuffer));
}

// Render the frame from the given index in the frame map
void renderFrame(int frameIndex) {

	// Get decoded frame
	AVFrame* frame = decodeFrame(frameIndex);

	SDL_UpdateYUVTexture(
		sdlTexture,
		NULL,	// Update entire image
		frame->data[0],
		frame->linesize[0],
		frame->data[1],
		frame->linesize[1],
		frame->data[2],
		frame->linesize[2]
		);

	// Clear screen
	SDL_RenderClear(sdlRenderer);

	// Render texture to screen
	SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);

	// Update screen
	SDL_RenderPresent(sdlRenderer);
}

// Add the given packet to the corresponding frame
// If frame is now complete, render it
void processPacket(UDPPacket packet) {

	int currentFrame = packet.header.nframe;
	int fragmentCount = packet.header.nfrags;

	// Add to map if we haven't started this frame yet.
	if (!frameMap.count(currentFrame)) {
		char* frame = (char*)malloc(fragmentCount * PACKET_SIZE);

		frameMap.emplace(currentFrame, frame);
		frameSize.emplace(currentFrame, packet.header.framesize);
		fragsReceived.emplace(currentFrame, 0);

		printf("Start new frame: %d\n", currentFrame);
	}

	// Size of this fragment
	int fragsize = PACKET_SIZE;
	if (packet.header.nfrag == packet.header.nfrags - 1) {

		// Last fragment is smaller
		int fragsize = packet.header.framesize - (PACKET_SIZE * (packet.header.nfrags - 1));
	}

	// Save packet at its spot in the frame
	int loc = packet.header.nfrag * PACKET_SIZE;
	memcpy_s(&frameMap[packet.header.nframe][loc], PACKET_SIZE, packet.packet, fragsize);
	fragsReceived[packet.header.nframe]++;

	// Frame has been fully assembled
	if (fragsReceived[packet.header.nframe] == packet.header.nfrags) {

		// Export frame
		renderFrame(currentFrame);
	}
}

// Main render loop
void renderLoop() {

	bool quit = false;
	
	while (!quit) {

		UDPPacket packet = nextPacket();

		// Save packet, render if frame complete
		processPacket(packet);
	}
}

int main(int argc, char* args[]) {

	initDecoder();
	initRenderer();
	initSocket();

	renderLoop();

	closeRenderer();
	
	return EXIT_SUCCESS;
}