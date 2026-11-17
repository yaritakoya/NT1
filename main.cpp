#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Novice.h>
#include <imgui.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace {
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kFieldSize = sizeof(std::uint16_t);
constexpr int kHeaderSize = kFieldSize * 2;
constexpr int kMaxPayloadSize = 1024;
constexpr int kFrameCapacity = kHeaderSize + kMaxPayloadSize;
constexpr int kMaxPlayers = 128;
constexpr int kPlayerWireSize = kFieldSize * 3;
constexpr int kPlayerRadius = 16;
constexpr unsigned int kPlayerColor = 0x55BD79FF;
constexpr unsigned int kBackgroundColor = 0x5B87B3FF;

enum class MessageType : std::uint16_t {
	Text = 1,
	AssignPlayerId = 2,
	JoinRequest = 3,
	WorldState = 4,
	Disconnect = 5,
};

static_assert(kFieldSize == 2 && sizeof(MessageType) == 2);
static_assert(kFieldSize + kMaxPlayers * kPlayerWireSize <= kMaxPayloadSize);

struct PlayerState {
	std::uint16_t id = 0;
	std::uint16_t x = 0;
	std::uint16_t y = 0;
};

struct TcpState {
	SOCKET clientSocket = INVALID_SOCKET;
	bool connected = false;
	int sentBytes = 0;
	int receivedBytes = 0;
	char receiveFrame[kFrameCapacity]{};
	std::vector<PlayerState> players;
	std::uint64_t gameLoop = 0;
	const char* result = "Ready";
	const char* errorStage = nullptr;
	int errorCode = 0;
};

struct UiInput {
	char host[64] = "34.104.202.113";
	char port[8] = "8000";
};

struct UiActions {
	bool connect = false;
	bool disconnect = false;
};

namespace TcpSystem {
void ClearError(TcpState& state) {
	state.errorStage = nullptr;
	state.errorCode = 0;
}

void SetError(TcpState& state, const char* stage, int code) {
	state.errorStage = stage;
	state.errorCode = code;
}

bool CloseSocket(TcpState& state) {
	state.connected = false;
	state.players.clear();
	if (state.clientSocket != INVALID_SOCKET) {
		if (closesocket(state.clientSocket) == SOCKET_ERROR) {
			const int closeError = WSAGetLastError();
			if (closeError != WSAENOTSOCK) {
				SetError(state, "closesocket", closeError);
				return false;
			}
		}
		state.clientSocket = INVALID_SOCKET;
	}
	return true;
}

bool SendAll(TcpState& state, const char* data, int size) {
	if (data == nullptr || size < 0 || size > kFrameCapacity) {
		SetError(state, "send size validation", WSAEINVAL);
		return false;
	}
	state.sentBytes = 0;
	while (state.sentBytes < size) {
		const int sent = send(state.clientSocket, data + state.sentBytes, size - state.sentBytes, 0);
		if (sent <= 0) {
			SetError(state, "send", sent == SOCKET_ERROR ? WSAGetLastError() : 0);
			return false;
		}
		state.sentBytes += sent;
	}
	return true;
}

bool SendJoinRequest(TcpState& state) {
	const std::uint16_t payloadSize = 0;
	const MessageType type = MessageType::JoinRequest;
	char frame[kHeaderSize]{};
	// 教材のWindows用形式に合わせ、長さと種別を2バイトずつ配置する。
	std::memcpy(frame, &payloadSize, kFieldSize);
	std::memcpy(frame + kFieldSize, &type, kFieldSize);
	return SendAll(state, frame, kHeaderSize);
}

void Connect(TcpState& state, const UiInput& input) {
	if (state.connected || !CloseSocket(state)) {
		return;
	}
	ClearError(state);
	state.sentBytes = 0;
	state.receivedBytes = 0;
	state.result = "Connecting...";

	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	addrinfo* address = nullptr;
	const int addressResult = getaddrinfo(input.host, input.port, &hints, &address);
	if (addressResult != 0) {
		SetError(state, "getaddrinfo", addressResult);
		state.result = "Address lookup failed";
		return;
	}

	if (address == nullptr || address->ai_addr == nullptr ||
	    address->ai_addrlen < sizeof(sockaddr_in) ||
	    address->ai_addrlen > static_cast<size_t>((std::numeric_limits<int>::max)())) {
		SetError(state, "address validation", WSAEINVAL);
	} else {
		state.clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (state.clientSocket == INVALID_SOCKET) {
			SetError(state, "socket", WSAGetLastError());
		} else if (connect(state.clientSocket, address->ai_addr,
		                   static_cast<int>(address->ai_addrlen)) == 0) {
			state.connected = true;
		} else {
			SetError(state, "connect", WSAGetLastError());
			CloseSocket(state);
		}
	}
	if (address != nullptr) {
		freeaddrinfo(address);
	}

	if (state.connected) {
		if (!SendJoinRequest(state)) {
			state.result = "JoinRequest failed";
			CloseSocket(state);
		} else {
			state.result = "Joined: waiting for WorldState";
		}
	}
}

void Disconnect(TcpState& state) {
	if (CloseSocket(state)) {
		ClearError(state);
		state.result = "Disconnected";
	}
}

bool ReceiveExact(TcpState& state, int targetSize) {
	if (targetSize < 0 || targetSize > kFrameCapacity ||
	    state.receivedBytes < 0 || state.receivedBytes > targetSize) {
		SetError(state, "receive size validation", WSAEINVAL);
		return false;
	}
	while (state.receivedBytes < targetSize) {
		const int received = recv(state.clientSocket,
		                          state.receiveFrame + state.receivedBytes,
		                          targetSize - state.receivedBytes, 0);
		if (received == SOCKET_ERROR) {
			SetError(state, "recv", WSAGetLastError());
			return false;
		}
		if (received == 0) {
			state.result = "Peer closed";
			return false;
		}
		state.receivedBytes += received;
	}
	return true;
}

bool ReceiveFrame(TcpState& state, std::uint16_t& payloadSize, MessageType& type) {
	state.receivedBytes = 0;
	payloadSize = 0;
	type = MessageType{};
	if (!ReceiveExact(state, kHeaderSize)) {
		return false;
	}
	std::memcpy(&payloadSize, state.receiveFrame, kFieldSize);
	std::memcpy(&type, state.receiveFrame + kFieldSize, kFieldSize);
	if (payloadSize > kMaxPayloadSize) {
		SetError(state, "payload exceeds 1024 bytes", 0);
		return false;
	}
	return ReceiveExact(state, kHeaderSize + payloadSize);
}

bool ApplyWorldState(TcpState& state, const char* payload, int payloadSize) {
	if (payload == nullptr || payloadSize < kFieldSize || payloadSize > kMaxPayloadSize) {
		return false;
	}
	std::uint16_t count = 0;
	std::memcpy(&count, payload, kFieldSize);
	if (count > kMaxPlayers || payloadSize != kFieldSize + count * kPlayerWireSize) {
		return false;
	}

	state.players.resize(count);
	for (size_t i = 0; i < count; ++i) {
		const char* entry = payload + kFieldSize + i * kPlayerWireSize;
		std::memcpy(&state.players[i].id, entry, kFieldSize);
		std::memcpy(&state.players[i].x, entry + kFieldSize, kFieldSize);
		std::memcpy(&state.players[i].y, entry + kFieldSize * 2, kFieldSize);
	}
	state.result = "WorldState updated";
	return true;
}

void ReceiveAndApply(TcpState& state) {
	if (!state.connected) {
		return;
	}
	std::uint16_t payloadSize = 0;
	MessageType type{};
	if (!ReceiveFrame(state, payloadSize, type)) {
		if (state.errorStage != nullptr) {
			state.result = "Receive failed";
		}
		CloseSocket(state);
		return;
	}

	if (type == MessageType::Disconnect) {
		state.result = "Server requested disconnect";
		CloseSocket(state);
	} else if (type == MessageType::WorldState) {
		if (!ApplyWorldState(state, state.receiveFrame + kHeaderSize, payloadSize)) {
			state.result = "Invalid WorldState: ignored";
		}
	}
	// AssignPlayerId、Text、未知種別は本文まで読み取ってから読み飛ばす。
}

void Update(TcpState& state, const UiInput& input, const UiActions& actions) {
	if (state.connected) {
		++state.gameLoop;
	}
	if (actions.disconnect) {
		Disconnect(state);
	} else if (actions.connect) {
		Connect(state, input);
	}
	// blocking recvで1フレームずつ受信する。サーバーの更新間隔に合わせ画面も更新される。
	ReceiveAndApply(state);
}
} // namespace TcpSystem

UiActions DrawConnectionWindow(UiInput& input, const TcpState& state) {
	UiActions actions{};
#ifdef USE_IMGUI
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kWindowWidth), 92.0f), ImGuiCond_Always);
	const bool visible = ImGui::Begin("TCP Client - 01_03", nullptr,
	                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
	                                      ImGuiWindowFlags_NoCollapse);
	if (visible) {
		ImGui::BeginDisabled(state.connected);
		ImGui::SetNextItemWidth(150.0f);
		ImGui::InputText("Host", input.host, sizeof(input.host));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(70.0f);
		ImGui::InputText("Port", input.port, sizeof(input.port));
		ImGui::SameLine();
		actions.connect = ImGui::Button("Connect");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(!state.connected);
		actions.disconnect = ImGui::Button("Disconnect");
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Text("Status: %s", state.connected ? "Connected" : "Not connected");
		ImGui::Text("Game loop: %llu   Players: %u   %s",
		            static_cast<unsigned long long>(state.gameLoop),
		            static_cast<unsigned int>(state.players.size()), state.result);
		if (state.errorStage != nullptr) {
			ImGui::Text("Error: %s (%d)", state.errorStage, state.errorCode);
		}
	}
	ImGui::End();
#endif
	return actions;
}

void DrawPlayers(const std::vector<PlayerState>& players) {
	for (const PlayerState& player : players) {
		Novice::DrawEllipse(player.x, player.y, kPlayerRadius, kPlayerRadius,
		                    0.0f, kPlayerColor, kFillModeSolid);
	}
}
} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
	Novice::Initialize("NT1_01_03", kWindowWidth, kWindowHeight);
	WSADATA wsaData{};
	const int startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (startupResult != 0) {
		char message[128]{};
		sprintf_s(message, "WSAStartup failed: %d", startupResult);
		MessageBoxA(nullptr, message, "TCP initialization error", MB_OK | MB_ICONERROR);
		Novice::Finalize();
		return 1;
	}

	TcpState state{};
	state.players.reserve(kMaxPlayers);
	UiInput input{};
	while (Novice::ProcessMessage() == 0) {
		Novice::BeginFrame();
		const UiActions actions = DrawConnectionWindow(input, state);
		TcpSystem::Update(state, input, actions);
		Novice::DrawBox(0, 0, kWindowWidth, kWindowHeight, 0.0f,
		                kBackgroundColor, kFillModeSolid);
		DrawPlayers(state.players);
		Novice::EndFrame();
	}

	const bool closed = TcpSystem::CloseSocket(state);
	const int cleanupResult = WSACleanup();
	Novice::Finalize();
	return closed && cleanupResult == 0 ? 0 : 1;
}
