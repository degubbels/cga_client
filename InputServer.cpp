//#include <stdio.h>
//
//#include "winsock.h"
//
//#include <SDL_main.h>
//#include <SDL.h>
//
//struct UDPInputPacket {
//	SDL_Keycode down[8];
//};
//
//class InputServer {
//private:
//	bool quit;
//
//	SOCKET serverSocket;
//	sockaddr_in serverSocketAddress;
//
//	const char* SOCKET_ADDRESS = "127.0.0.1";
//	const int SOCKET_PORT = 8880;
//
//public:
//
//	void initSocket() {
//		// Initialise winsock
//		WSADATA wsaData;
//		int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
//		if (ret != 0) {
//			printf("WSA init failed: %d\n", WSAGetLastError());
//			exit(EXIT_FAILURE);
//		}
//
//
//		// Configure address
//		serverSocketAddress.sin_family = AF_INET;
//		serverSocketAddress.sin_addr.s_addr = inet_addr(SOCKET_ADDRESS);
//		serverSocketAddress.sin_port = htons(SOCKET_PORT);
//
//		// Initialize the socket
//		serverSocket = socket(PF_INET, SOCK_DGRAM, 0);
//		if (serverSocket < 0) {
//			printf("Socket init failed: %d\n", WSAGetLastError());
//			exit(EXIT_FAILURE);
//		}
//
//		// DO NOT, NO, PLEASE DO NOT BIND THE SOCKET!
//		// IT WILL MAKE IT IMPOSSIBLE TO SEND PACKETS
//		// AND WILL COST FAR TOO MUCH TIME TO FIGURE OUT WHAT WAS WRONG
//	}
//
//	void startServer() {
//		printf("K||start\n");
//
//		initSocket();
//		inputLoop();
//	}
//
//	void inputLoop() {
//		// Get input
//		// ...
//		 //Handle events on queue
//
//		printf("K||input loop\n");
//
//		UDPInputPacket packet;
//
//		bool inputAvailable;
//		while (!quit) {
//
//			inputAvailable = false;
//
//			SDL_Event e;
//			int i = 0;
//			while (SDL_PollEvent(&e) != 0) {
//
//				if (i > 8) {
//					printf("Too many concurrent user inputs, some lost");
//				}
//
//				// User requests quit
//				if (e.type == SDL_QUIT) {
//					quit = true;
//				}
//
//				// Other input
//				if (e.type == SDL_KEYDOWN) {
//
//					// Send to server
//					packet.down[i] = e.key.keysym.sym;
//					
//					printf("key down: %d\n", e.key.keysym.sym);
//
//					i++;
//					inputAvailable = true;
//				}
//				
//			}
//
//			// Send only if input is available
//			if (inputAvailable) {
//
//				printf("K||send\n");
//				int ret = sendto(
//					serverSocket,
//					(char*)&packet,
//					sizeof(packet),
//					0,
//					(const sockaddr*)&serverSocketAddress,
//					sizeof(serverSocketAddress)
//				);
//
//				if (ret < 0) {
//					printf("UDP packet send failed: %d\n", WSAGetLastError());
//				}
//			}
//		}
//	}
//};