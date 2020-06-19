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

// For image export
#define STB_IMAGE_IMPLEMENTATION  
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

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
const char* FILENAME = "out/frame_";
const int FRAME_WIDTH = 1280;
const int FRAME_HEIGHT = 720;
const AVPixelFormat FRAME_FORMAT = AV_PIX_FMT_YUV420P;

const AVCodecID CODEC_ID = AV_CODEC_ID_MPEG4;

AVCodec* codec;
AVCodecContext* codecContext;

// Socket
sockaddr_in UDPAddress;
SOCKET UDPSocket;

SDL_Texture* gTexture;
SDL_Rect* gRect;
SDL_Window* gWindow;
SDL_Renderer* gRenderer;


// Partly from: https://lazyfoo.net/tutorials/SDL/07_texture_loading_and_rendering/index.php


// Initialise decoder
void initDecoder() {

	// Note: av_codec_register all is no longer necessary and deprecated in ffmped >4.0

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
	if (avcodec_open2(codecContext, codec, NULL) < 0) {
		printf("could not open codec.. %s\n", stderr);
		exit(EXIT_FAILURE);
	}
}

// Initialise SDL renderer
void initRenderer() {

	SDL_Init(SDL_INIT_EVERYTHING);

	bool success;

	//Create window
	gWindow = SDL_CreateWindow("SDL Tutorial", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, FRAME_WIDTH, FRAME_HEIGHT, SDL_WINDOW_SHOWN);
	if (gWindow == NULL)
	{
		printf("Window could not be created! SDL Error: %s\n", SDL_GetError());
		success = false;
	}
	else
	{
		//Create renderer for window
		gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED);
		if (gRenderer == NULL)
		{
			printf("Renderer could not be created! SDL Error: %s\n", SDL_GetError());
			success = false;
		}
		else
		{
			//Initialize renderer color
			SDL_SetRenderDrawColor(gRenderer, 0xFF, 0xFF, 0xFF, 0xFF);

			// Init texture
			gTexture = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, FRAME_WIDTH, FRAME_HEIGHT);

		}
	}
}

void closeRenderer()
{
	//Free loaded image
	SDL_DestroyTexture(gTexture);
	gTexture = NULL;

	//Destroy window    
	SDL_DestroyRenderer(gRenderer);
	SDL_DestroyWindow(gWindow);
	gWindow = NULL;
	gRenderer = NULL;

	//Quit SDL subsystems
	SDL_Quit();
}

// Convert back to RGBA format for export to picture
static void convert_frame(AVFrame* inFrame, AVFrame* outFrame) {

	SwsContext* ctx = sws_getContext(
		FRAME_WIDTH, FRAME_HEIGHT,
		(AVPixelFormat) inFrame->format,
		FRAME_WIDTH, FRAME_HEIGHT,
		(AVPixelFormat) outFrame->format,
		0, 0, 0, 0  // No filters or flags
	);
	
	sws_scale(
		ctx,
		inFrame->data, inFrame->linesize,
		0, FRAME_HEIGHT, // Process entire image
		outFrame->data, outFrame->linesize
	);
}

// Save frame as png picture
static void save_picture(AVFrame* frame, const char* filename) {
	
	// Create out picture object
	AVFrame* outframe = av_frame_alloc();

	outframe->width = FRAME_WIDTH;
	outframe->height = FRAME_HEIGHT;
	outframe->format = AV_PIX_FMT_RGBA;

	// Allocate destination frame
	int ret = av_frame_get_buffer(outframe, 32);
	if (ret < 0) {
		fprintf(stderr, "could not alloc the frame data\n");
		exit(1);
	}

	// Convert back to RGBA
	convert_frame(frame, outframe);

	// Write to picture
	stbi_write_png(
		filename,
		outframe->width, outframe->height,
		4,
		outframe->data[0],
		outframe->linesize[0]
	);

	printf("File written %s/n", filename);
}

// Decode frame
// Returns whether a frame was successfully decoded
static bool decode(AVCodecContext* dec_ctx, AVFrame* frame, AVPacket* pkt, const char* filename) {

	// Send packet to decoder
	int ret = avcodec_send_packet(dec_ctx, pkt);
	if (ret < 0) {
		fprintf(stderr, "Error sending a packet for decoding\n");
		exit(1);
	}

	// Get frame from decoder
	ret = avcodec_receive_frame(dec_ctx, frame);
	if (ret == AVERROR(EAGAIN)) {
		printf("partial frame..\n");
		return false;
	} else if (ret == AVERROR_EOF) {
		printf("decode error.. eof\n");
		return false;
	} else if (ret < 0) {
		fprintf(stderr, "Error during decoding\n");
		exit(1);
	}

	printf("saving frame with out_id=%3d\n", dec_ctx->frame_number);
	fflush(stdout);

	// the picture is allocated by the decoder. no need to free it
	save_picture(frame, filename);

	// Successully decoded
	return true;
}

/*
	Decode a frame
	Param frameIndex; the index in the frameMap of the frame to render
*/
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

// Export frame
void write_frame(int nframe) {
	printf("Decoding frame %d...\n", nframe);
	
	// Allocate packet
	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		printf("Packet alloc failed..\n");
		exit(EXIT_FAILURE);
	}

	// Create picture object
	AVFrame* picture = av_frame_alloc();

	picture->width = FRAME_WIDTH;
	picture->height = FRAME_HEIGHT;
	picture->format = AV_PIX_FMT_YUV420P;

	// Fill av-packet with data
	pkt->data = (uint8_t*) frameMap[nframe];
	pkt->size = frameSize[nframe];

	// Create picture file name for this frame
	std::string filename = FILENAME + std::to_string(nframe) + ".png";

	// Decode packet
	if (decode(codecContext, picture, pkt, filename.c_str() )) {
		printf("decoded frame.\n");
	}
}

// Process received udp-packet
void accept_fragment(UDPPacket packet) {

	// Add to map if we haven't started this frame yet.
	if (!frameMap.count(packet.header.nframe)) {
		char* frame = (char*)malloc(packet.header.nfrags*PACKET_SIZE);

		frameMap.emplace(packet.header.nframe, frame);
		frameSize.emplace(packet.header.nframe, packet.header.framesize);
		fragsReceived.emplace(packet.header.nframe, 0);

		printf("Start new frame: %d\n", packet.header.nframe);
	}

	// Size of this fragment
	int fragsize = PACKET_SIZE;
	if (packet.header.nfrag == packet.header.nfrags -1) {

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
		write_frame(packet.header.nframe);
	}
}

// Initialise socket
void initSocket() {

	// Initialise winsock
	WSADATA wsaData;
	printf("Initialising Winsock...");
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		printf("WSA init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}
	printf("Initialised.\n");

	// Initialize the socket
	UDPSocket = socket(PF_INET, SOCK_DGRAM, 0);
	if (UDPSocket < 0) {
		printf("Socket init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}
	printf("Socket created.\n");

	// Configure address
	UDPAddress.sin_family = AF_INET;
	UDPAddress.sin_addr.s_addr = INADDR_ANY;
	UDPAddress.sin_port = htons(8888);

	if (bind(UDPSocket, (const sockaddr*)&UDPAddress, sizeof(UDPAddress)) != 0) {
		printf("Socket bind failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}
	printf("Socket bound.\n");

}

// Receive udp-packets
void udp_recv() {
	byte recvbuffer[PACKET_SIZE+sizeof(UDPHeader)];

	int slen = sizeof(UDPAddress);

	// Keep receiving frames
	printf("Listening for messages at port 8888...\n");
	while (true) {

		// Receive next udp packet
		int ret = recvfrom(
			UDPSocket,
			(char*)recvbuffer,
			PACKET_SIZE + sizeof(UDPHeader),
			0,
			(sockaddr*)&UDPAddress,
			&slen
			);

		if (ret < 0) {
			printf("Receive failure: %d\n", WSAGetLastError());
			exit(EXIT_FAILURE);
		}

		// Interpret as packet struct
		UDPPacket packet = *(reinterpret_cast<UDPPacket*>(recvbuffer));

		// Handle packet
		accept_fragment(packet);
	}
}


byte recvBuffer[PACKET_SIZE + sizeof(UDPHeader)];
int slen = sizeof(UDPAddress);
/*
	Wait until the next packet arrives
	Blocks execution
*/
UDPPacket nextPacket() {

	// Receive next udp packet
	int ret = recvfrom(
		UDPSocket,
		(char*)recvBuffer,
		PACKET_SIZE + sizeof(UDPHeader),
		0,
		(sockaddr*)&UDPAddress,
		&slen
		);

	// Check for error
	if (ret < 0) {
		printf("Receive failure: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Interpret as packet struct
	return *(reinterpret_cast<UDPPacket*>(recvBuffer));
}

void renderFrame(int frameIndex) {

	AVFrame* frame = decodeFrame(frameIndex);

	SDL_UpdateYUVTexture(
		gTexture,
		NULL,	// Update entire image
		frame->data[0],
		frame->linesize[0],
		frame->data[1],
		frame->linesize[1],
		frame->data[2],
		frame->linesize[2]
		);

	// Clear screen
	SDL_RenderClear(gRenderer);

	// Render texture to screen
	SDL_RenderCopy(gRenderer, gTexture, NULL, NULL);

	// Update screen
	SDL_RenderPresent(gRenderer);
}

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

// 
void renderLoop() {

	bool quit = false;
	
	while (!quit) {

		UDPPacket packet = nextPacket();

		// Save packet, render if frame complete
		processPacket(packet);
	}
}

int main(int argc, char* args[]) {

	// Prepare
	initDecoder();
	initRenderer();
	initSocket();

	renderLoop();
	
	return EXIT_SUCCESS;
}