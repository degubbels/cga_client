#include "winsock.h"
#include <stdio.h>
#include <string>
#include <iostream>
#include <map>
#include <thread>

// For decoding
extern "C" {
	#include "libavcodec/avcodec.h"
	#include "libavutil/frame.h"
}

#include <SDL_main.h>
#include <SDL.h>

#include "external/imgui/imgui.h"
#include "external/imgui_sdl.h"

const int PACKET_SIZE = 1400;

// Packet definition
struct UDPRecvHeader {
	uint32_t nframe;
	uint32_t nfrag;

	uint32_t nfrags;
	uint32_t framesize;
};

struct UDPRecvPacket {
	UDPRecvHeader header;
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

// Codec
const AVCodecID CODEC_ID = AV_CODEC_ID_MPEG4;
AVCodec* codec;
AVCodecContext* codecContext;

// Receive Socket
u_long UDP_RECEIVE_ADDRESS = INADDR_ANY;
u_short UDP_RECEIVE_PORT = 8888;

sockaddr_in UDPRecvAddress;
SOCKET UDPRecvSocket;

byte UDPRecvBuffer[PACKET_SIZE + sizeof(UDPRecvHeader)];
int UDP_RECEIVE_ADDRESS_LENGTH = sizeof(UDPRecvAddress);

// SDL
SDL_Texture* sdlTexture;
SDL_Window* sdlWindow;
SDL_Renderer* sdlRenderer;

ImGuiContext* imGuiContext;

//
struct UDPInputPacket {
	SDL_Keycode down[8];
	SDL_Keycode up[8];
	bool empty;
};

// Send socket
sockaddr_in UDPSendAddress;
SOCKET UDPSendSocket;

const char* UDP_SEND_ADDRESS = "127.0.0.1";
const int UDP_SEND_PORT = 8880;

// Should the client stop at the end of this frame
bool quit;

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

// Initialise frame receiver socket
void initReceiverSocket() {

	// Initialise winsock
	WSADATA wsaData;
	int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (ret != 0) {
		printf("WSA init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Initialize the socket
	UDPRecvSocket = socket(PF_INET, SOCK_DGRAM, 0);
	if (UDPRecvSocket < 0) {
		printf("Socket init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Configure address
	UDPRecvAddress.sin_family = AF_INET;
	UDPRecvAddress.sin_addr.s_addr = UDP_RECEIVE_ADDRESS;
	UDPRecvAddress.sin_port = htons(UDP_RECEIVE_PORT);

	// Bind socket
	ret = bind(UDPRecvSocket, (const sockaddr*)&UDPRecvAddress, sizeof(UDPRecvAddress));
	if (ret != 0) {
		printf("Socket bind failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}
}

// Initialise input server socket
void initServerSocket() {
	// Initialise winsock
	WSADATA wsaData;
	int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (ret != 0) {
		printf("WSA init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Configure address
	UDPSendAddress.sin_family = AF_INET;
	UDPSendAddress.sin_addr.s_addr = inet_addr(UDP_SEND_ADDRESS);
	UDPSendAddress.sin_port = htons(UDP_SEND_PORT);

	// Initialize the socket
	UDPSendSocket = socket(PF_INET, SOCK_DGRAM, 0);
	if (UDPSendSocket < 0) {
		printf("Socket init failed: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// DO NOT, NO, PLEASE DO NOT BIND THE SOCKET!
	// IT WILL MAKE IT IMPOSSIBLE TO SEND PACKETS
	// AND WILL COST FAR TOO MUCH TIME TO FIGURE OUT WHAT WAS WRONG
}

// Initialise imGUI
void initGUI() {
	imGuiContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(imGuiContext);

	ImGuiSDL::Initialize(sdlRenderer, FRAME_WIDTH, FRAME_HEIGHT);
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

// ImGUI render
void renderGUI() {
	ImGui::NewFrame();

	ImGui::Begin("game", NULL, NULL);
	ImGui::Text("Servas");
	ImGui::End();

	ImGui::Render();
	ImGuiSDL::Render(ImGui::GetDrawData());
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

	renderGUI();

	// Update screen
	SDL_RenderPresent(sdlRenderer);

	// Destroy raw frame data
	frameSize.erase(frameIndex);
	fragsReceived.erase(frameIndex);
	frameMap.erase(frameIndex);
}

// Wait until the next packet arrives
//	Blocks execution
UDPRecvPacket nextPacket() {

	// Receive next udp packet
	int ret = recvfrom(
		UDPRecvSocket,
		(char*)UDPRecvBuffer,
		PACKET_SIZE + sizeof(UDPRecvHeader),
		0,
		(sockaddr*)&UDPRecvAddress,
		&UDP_RECEIVE_ADDRESS_LENGTH
	);

	// Check for error
	if (ret < 0) {
		printf("Receive failure: %d\n", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	// Interpret as packet struct
	return *(reinterpret_cast<UDPRecvPacket*>(UDPRecvBuffer));
}

// Add the given packet to the corresponding frame
// If frame is now complete, render it
void processPacket(UDPRecvPacket packet) {

	int currentFrame = packet.header.nframe;
	int fragmentCount = packet.header.nfrags;

	// Add to map if we haven't started this frame yet.
	if (!frameMap.count(currentFrame)) {
		char* frame = (char*)malloc(fragmentCount * PACKET_SIZE);

		frameMap.emplace(currentFrame, frame);
		frameSize.emplace(currentFrame, packet.header.framesize);
		fragsReceived.emplace(currentFrame, 0);

		//printf("Start new frame: %d\n", currentFrame);
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

// Poll input from sdl
// If input is available, send the keycodes to the game server
void processInput() {

	UDPInputPacket packet;
	bool inputAvailable = false;

	SDL_Event e;
	int cDown = 0;
	int cUp = 0;
	while (SDL_PollEvent(&e) != 0) {

		if (cDown > 8 || cUp > 8) {
			printf("Too many concurrent user inputs, some lost");
			break;
		}

		// User requests quit
		if (e.type == SDL_QUIT) {
			quit = true;
		}

		// Other input
		if (e.type == SDL_KEYDOWN) {

			// Send to server
			packet.down[cDown] = e.key.keysym.sym;

			cDown++;
			inputAvailable = true;
		}

		// Other input
		if (e.type == SDL_KEYUP) {

			// Send to server
			packet.up[cUp] = e.key.keysym.sym;

			cUp++;
			inputAvailable = true;
		}
	}

	packet.empty = !inputAvailable;

	// always send, so we know dt
	int ret = sendto(
		UDPSendSocket,
		(char*)&packet,
		sizeof(packet),
		0,
		(const sockaddr*)&UDPSendAddress,
		sizeof(UDPSendAddress)
	);

	if (ret < 0) {
		printf("UDP packet send failed: %d\n", WSAGetLastError());
	}
}

// Main render loop
void renderLoop() {

	quit = false;
	
	while (!quit) {

		processInput();

		UDPRecvPacket packet = nextPacket();

		// Save packet, render if frame complete
		processPacket(packet);
	}
}

int main(int argc, char* args[]) {

	initDecoder();
	initRenderer();
	initGUI();
	initReceiverSocket();
	initServerSocket();

	// Show video stream from game server
	renderLoop();

	closeRenderer();
	
	return EXIT_SUCCESS;
}