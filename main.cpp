#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Novice.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <imgui.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

namespace {
constexpr int kHeaderSize = 4;
constexpr unsigned short kTextType = 1;
constexpr int kMaxBodySize = 1024;

unsigned short ReadUInt16(const char* bytes) {
	return static_cast<unsigned short>((static_cast<unsigned char>(bytes[0]) << 8) |
	                                    static_cast<unsigned char>(bytes[1]));
}

void WriteUInt16(char* bytes, unsigned short value) {
	bytes[0] = static_cast<char>((value >> 8) & 0xff);
	bytes[1] = static_cast<char>(value & 0xff);
}

bool SendAll(SOCKET socket, const char* data, int length, int& totalSent) {
	totalSent = 0;
	while (totalSent < length) {
		int result = send(socket, data + totalSent, length - totalSent, 0);
		if (result <= 0) {
			return false;
		}
		totalSent += result;
	}
	return true;
}

bool ReceiveExact(SOCKET socket, char* data, int length, int& totalReceived) {
	totalReceived = 0;
	while (totalReceived < length) {
		int result = recv(socket, data + totalReceived, length - totalReceived, 0);
		if (result <= 0) {
			return false;
		}
		totalReceived += result;
	}
	return true;
}

void CloseSocket(SOCKET& socket) {
	if (socket != INVALID_SOCKET) {
		shutdown(socket, SD_BOTH);
		closesocket(socket);
		socket = INVALID_SOCKET;
	}
}
} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
	Novice::Initialize("NT1_01_00", 640, 360);
	char host[64] = "34.104.202.113";
	char port[8] = "8001";
	bool connected = false;

	WSADATA wsaData{};
	SOCKET sock = INVALID_SOCKET;
	int sentBytes = 0;
	int receivedBytes = 0;

	char text[256] = "";
	char echo[256] = "";
	char message[128] = "";

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		Novice::Finalize();
		return 1;
	}

	while (Novice::ProcessMessage() == 0) {
		Novice::BeginFrame();

#ifdef USE_IMGUI
		ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
		ImGui::SetNextWindowSize(ImVec2(600, 320), ImGuiCond_Once);
		ImGui::Begin("TCP Connection");

		ImGui::BeginDisabled(connected);
		ImGui::InputText("Host", host, sizeof(host));
		ImGui::InputText("Port", port, sizeof(port));

		if (ImGui::Button("Connect")) {
			addrinfo hints{};
			addrinfo* result = nullptr;
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_protocol = IPPROTO_TCP;

			int portNumber = std::atoi(port);
			if (portNumber >= 1 && portNumber <= 65535 && getaddrinfo(host, port, &hints, &result) == 0) {
				for (addrinfo* address = result; address != nullptr; address = address->ai_next) {
					sock = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
					if (sock == INVALID_SOCKET) {
						continue;
					}
					if (connect(sock, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
						connected = true;
						break;
					}
					closesocket(sock);
					sock = INVALID_SOCKET;
				}
				freeaddrinfo(result);
			}

			sentBytes = 0;
			receivedBytes = 0;
			memset(echo, 0, sizeof(echo));
			strcpy_s(message, sizeof(message), connected ? "Connected" : "Connect Error");
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::BeginDisabled(!connected);
		if (ImGui::Button("Disconnect")) {
			CloseSocket(sock);
			connected = false;
			strcpy_s(message, sizeof(message), "Disconnected");
		}
		ImGui::EndDisabled();

		ImGui::Text("Status: %s", connected ? "Connected" : "Not connected");
		ImGui::Separator();

		ImGui::BeginDisabled(!connected);
		ImGui::InputText("Text", text, sizeof(text));
		if (ImGui::Button("Send")) {
			memset(echo, 0, sizeof(echo));
			sentBytes = 0;
			receivedBytes = 0;
			message[0] = '\0';

			const int bodyLength = static_cast<int>(strlen(text));
			if (bodyLength == 0) {
				strcpy_s(message, sizeof(message), "Text is empty");
			} else if (bodyLength > 0xffff) {
				strcpy_s(message, sizeof(message), "Text is too long");
			} else {
				char header[kHeaderSize]{};
				WriteUInt16(header, static_cast<unsigned short>(bodyLength));
				WriteUInt16(header + 2, kTextType);

				int headerSent = 0;
				int bodySent = 0;
				bool ok = SendAll(sock, header, kHeaderSize, headerSent);
				sentBytes += headerSent;
				if (ok) {
					ok = SendAll(sock, text, bodyLength, bodySent);
					sentBytes += bodySent;
				}

				if (ok) {
					char responseHeader[kHeaderSize]{};
					int headerReceived = 0;
					ok = ReceiveExact(sock, responseHeader, kHeaderSize, headerReceived);
					receivedBytes += headerReceived;
					if (ok) {
						const unsigned short responseLength = ReadUInt16(responseHeader);
						const unsigned short responseType = ReadUInt16(responseHeader + 2);
						if (responseLength > kMaxBodySize || responseType != kTextType) {
							strcpy_s(message, sizeof(message), "Invalid response header");
							ok = false;
						} else {
						int bodyReceived = 0;
						ok = ReceiveExact(sock, echo, responseLength, bodyReceived);
						receivedBytes += bodyReceived;
						echo[bodyReceived] = '\0';
						if (ok) {
						strcpy_s(message, sizeof(message), "Reply received");
						}
					}
				}

				if (!ok) {
					if (message[0] == '\0') {
						strcpy_s(message, sizeof(message), "Send/Receive Error");
					}
					CloseSocket(sock);
					connected = false;
				}
			}
		}
		ImGui::EndDisabled();

		ImGui::Separator();
		ImGui::Text("Reply: %s", echo);
		ImGui::Text("Sent: %d bytes", sentBytes);
		ImGui::Text("Received: %d bytes", receivedBytes);
		ImGui::Text("Total: %d bytes", sentBytes + receivedBytes);
		ImGui::Text("Message: %s", message);
		ImGui::End();
#endif

		Novice::EndFrame();
	}

	CloseSocket(sock);
	WSACleanup();
	Novice::Finalize();
	return 0;
}
